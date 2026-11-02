/*++
    FilterDriver.c

    TempModFilter - upper filter driver that performs a *temporary*
    (in-memory only) modification on I/O request packets.

    What it does
    ------------
    It registers itself as an upper filter of a device class (the INF targets
    DiskDrive) and forwards every request to the lower stack.  On completion
    of a device-control request it optionally rewrites the output buffer that
    is still sitting in RAM, then hands it back to the caller.

    What it never does
    ------------------
      * no writes to disk, firmware, SPI flash, NVRAM or EEPROM
      * no patching of kernel structures (SSDT, IDT, EPROCESS, ...)
      * no bypass of PatchGuard, DSE, HVCI or VBS
      * no permanent change of any kind

    Power cycle -> RAM is rebuilt -> every trace of the modification is gone.

    Configuration surface
    ---------------------
    A control device (\\\\.\\TempModFilter) exposes IOCTL_TEMPMOD_* so a
    privileged user-mode client can arm / disarm the policy at runtime.
--*/

#include <ntddk.h>
#include <wdf.h>

#include "../include/TempModShared.h"
#include "FilterPolicy.h"

/* ------------------------------------------------------------------------- */
/* Logging                                                                    */
/* ------------------------------------------------------------------------- */

#define TempModTrace(...) DbgPrint("TempModFilter: " __VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* Prototypes                                                                 */
/* ------------------------------------------------------------------------- */

DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD          TempModEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEFAULT        TempModEvtIoDefault;
EVT_WDF_REQUEST_COMPLETION_ROUTINE TempModEvtRequestComplete;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL TempModEvtControlDeviceControl;

/* ------------------------------------------------------------------------- */
/* Globals                                                                    */
/* ------------------------------------------------------------------------- */

static TEMPMOD_STATE g_State;               /* single, driver-wide policy      */
static WDFDEVICE     g_ControlDevice;       /* NULL until DriverEntry succeeds */

/* ------------------------------------------------------------------------- */
/* Control device                                                             */
/* ------------------------------------------------------------------------- */

static NTSTATUS
TempModCreateControlDevice(
    _In_ WDFDRIVER Driver
    )
{
    PWDFDEVICE_INIT        deviceInit;
    WDFDEVICE              device;
    WDF_IO_QUEUE_CONFIG    queueConfig;
    WDFQUEUE               queue;
    WDF_OBJECT_ATTRIBUTES  attributes;
    UNICODE_STRING         deviceName;
    UNICODE_STRING         symbolicLink;
    UNICODE_STRING         sddl;
    NTSTATUS               status;

    /*
        Only SYSTEM and the local Administrators group may open the control
        device.  The default security descriptor of a control device is too
        permissive for a component that can rewrite I/O buffers.
    */
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

    deviceInit = WdfControlDeviceInitAllocate(Driver, &sddl);
    if (deviceInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlInitUnicodeString(&deviceName, TEMPMOD_DEVICE_NAME);
    status = WdfDeviceInitAssignName(deviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    WdfDeviceInitSetDeviceType(deviceInit, TEMPMOD_DEVICE_TYPE);
    WdfDeviceInitSetCharacteristics(deviceInit, FILE_DEVICE_SECURE_OPEN, FALSE);
    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(deviceInit, FALSE);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        TempModTrace("WdfDeviceCreate(control) failed 0x%08X\n", status);
        /* WdfDeviceCreate consumes deviceInit, even on failure. */
        return status;
    }

    RtlInitUnicodeString(&symbolicLink, TEMPMOD_DOS_NAME);
    status = WdfDeviceCreateSymbolicLink(device, &symbolicLink);
    if (!NT_SUCCESS(status)) {
        TempModTrace("CreateSymbolicLink failed 0x%08X\n", status);
        WdfObjectDelete(device);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoDeviceControl = TempModEvtControlDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        TempModTrace("WdfIoQueueCreate(control) failed 0x%08X\n", status);
        WdfObjectDelete(device);
        return status;
    }

    status = WdfDeviceConfigureRequestDispatching(
                 device, queue, WdfRequestTypeDeviceControl);
    if (!NT_SUCCESS(status)) {
        TempModTrace("ConfigureRequestDispatching failed 0x%08X\n", status);
        WdfObjectDelete(device);
        return status;
    }

    g_ControlDevice = device;
    WdfControlFinishInitializing(device);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* Control-device dispatch (PASSIVE_LEVEL)                                    */
/* ------------------------------------------------------------------------- */

VOID
TempModEvtControlDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    switch (IoControlCode) {

    case IOCTL_TEMPMOD_SET_POLICY:
    {
        PTEMPMOD_POLICY input = NULL;
        PTEMPMOD_POLICY output = NULL;

        status = WdfRequestRetrieveInputBuffer(
                     Request, sizeof(TEMPMOD_POLICY),
                     (PVOID*)&input, NULL);
        if (!NT_SUCCESS(status)) {
            break;
        }

        status = TempModSetPolicy(&g_State, input);
        if (!NT_SUCCESS(status)) {
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(
                     Request, sizeof(TEMPMOD_POLICY),
                     (PVOID*)&output, NULL);
        if (NT_SUCCESS(status)) {
            TempModGetPolicy(&g_State, output);
            information = sizeof(TEMPMOD_POLICY);
        }

        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_TEMPMOD_GET_POLICY:
    {
        PTEMPMOD_POLICY output = NULL;

        status = WdfRequestRetrieveOutputBuffer(
                     Request, sizeof(TEMPMOD_POLICY),
                     (PVOID*)&output, NULL);
        if (!NT_SUCCESS(status)) {
            break;
        }

        TempModGetPolicy(&g_State, output);
        information = sizeof(TEMPMOD_POLICY);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_TEMPMOD_GET_STATS:
    {
        PTEMPMOD_STATS output = NULL;

        status = WdfRequestRetrieveOutputBuffer(
                     Request, sizeof(TEMPMOD_STATS),
                     (PVOID*)&output, NULL);
        if (!NT_SUCCESS(status)) {
            break;
        }

        TempModGetStats(&g_State, output);
        information = sizeof(TEMPMOD_STATS);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_TEMPMOD_RESET_STATS:
    {
        TempModResetStats(&g_State);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_TEMPMOD_CLEAR_POLICY:
    {
        TempModClearPolicy(&g_State);
        status = STATUS_SUCCESS;
        break;
    }

    default:
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, information);
}

/* ------------------------------------------------------------------------- */
/* Filter device (one instance per filtered device)                           */
/* ------------------------------------------------------------------------- */

NTSTATUS
TempModEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDFDEVICE           device;
    WDF_IO_QUEUE_CONFIG queueConfig;
    NTSTATUS            status;

    UNREFERENCED_PARAMETER(Driver);

    /* Mandatory: this driver is a filter, not a function driver. */
    WdfFdoInitSetFilter(DeviceInit);

    status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status)) {
        TempModTrace("WdfDeviceCreate(filter) failed 0x%08X\n", status);
        return status;
    }

    /*
        One default queue that receives every request type.  Parallel
        dispatch keeps us from serialising the whole storage stack behind a
        single in-flight request; all shared state is protected by the
        spinlock in g_State, so parallel completion is safe here.
    */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queueConfig, WdfIoQueueDispatchParallel);

    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoDefault = TempModEvtIoDefault;

    status = WdfIoQueueCreate(device, &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        TempModTrace("WdfIoQueueCreate(filter) failed 0x%08X\n", status);
        WdfObjectDelete(device);
        return status;
    }

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* Data path: forward, then optionally rewrite the returned buffer            */
/* ------------------------------------------------------------------------- */

VOID
TempModEvtIoDefault(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request
    )
{
    WDFDEVICE  device = WdfIoQueueGetDevice(Queue);
    WDFIOTARGET target = WdfDeviceGetIoTarget(device);
    NTSTATUS   status;

    InterlockedIncrement(&g_State.RequestsObserved);

    /*
        Keep the original stack parameters: this is a filter, the request must
        reach the lower driver exactly as it was built by the I/O manager.
        WdfRequestFormatRequestUsingCurrentType returns VOID.
    */
    WdfRequestFormatRequestUsingCurrentType(Request);

    WdfRequestSetCompletionRoutine(Request, TempModEvtRequestComplete, NULL);

    if (!WdfRequestSend(Request, target, WDF_NO_SEND_OPTIONS)) {

        status = WdfRequestGetStatus(Request);

        TempModTrace("WdfRequestSend failed 0x%08X\n", status);
        WdfRequestComplete(
            Request,
            NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status);
    }
}

VOID
TempModEvtRequestComplete(
    _In_ WDFREQUEST                     Request,
    _In_ WDFIOTARGET                    Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT                     Context
    )
{
    TEMPMOD_POLICY policy;
    WDFMEMORY      memory;
    PVOID          buffer;
    size_t         bufferLength = 0;
    size_t         validLength;
    ULONG          replaced;

    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Context);

    if (Params->Type != WdfRequestTypeDeviceControl ||
        !NT_SUCCESS(Params->IoStatus.Status)) {
        goto Complete;
    }

    memory = Params->Parameters.Ioctl.Output.Buffer;
    if (memory == NULL) {
        goto Complete;
    }

    buffer = WdfMemoryGetBuffer(memory, &bufferLength);
    if (buffer == NULL || bufferLength == 0) {
        goto Complete;
    }

    TempModGetPolicy(&g_State, &policy);

    if (!policy.Enabled || policy.RuleCount == 0) {
        goto Complete;
    }

    /* Rule-level TargetIoControl filtering is done inside TempModApplyPolicy. */

    /*
        Only the bytes the lower driver actually produced may be touched;
        the rest of the buffer is uninitialised.
    */
    validLength = bufferLength;

    if (Params->IoStatus.Information > 0 &&
        Params->IoStatus.Information < validLength) {
        validLength = (size_t)Params->IoStatus.Information;
    }

    InterlockedIncrement(&g_State.BuffersScanned);

    replaced = TempModApplyPolicy((PUCHAR)buffer, validLength,
                                  Params->Parameters.Ioctl.IoControlCode,
                                  &policy);

    if (replaced > 0) {
        InterlockedExchangeAdd(&g_State.ReplacementsTotal, (LONG)replaced);
    }

Complete:

    /*
        Information must be preserved, otherwise every caller sees a
        zero-length reply (this is the bug in the guide's sample).
    */
    WdfRequestCompleteWithInformation(
        Request, Params->IoStatus.Status, Params->IoStatus.Information);
}

/* ------------------------------------------------------------------------- */
/* Driver entry / unload                                                      */
/* ------------------------------------------------------------------------- */

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS          status;

    TempModInitializeState(&g_State);
    g_ControlDevice = NULL;

    /*
        No EvtDriverUnload on purpose.  A filter that sits in the storage
        stack must never disappear underneath live device objects; without an
        unload routine the driver object is simply not unloadable and removal
        always means "reboot", which is exactly the semantics this project is
        about.
    */
    WDF_DRIVER_CONFIG_INIT(&config, TempModEvtDeviceAdd);

    status = WdfDriverCreate(
                 DriverObject, RegistryPath,
                 WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        TempModTrace("WdfDriverCreate failed 0x%08X\n", status);
        return status;
    }

    status = TempModCreateControlDevice(WdfGetDriver());
    if (!NT_SUCCESS(status)) {
        TempModTrace("control device creation failed 0x%08X\n", status);
        return status;
    }

    TempModTrace("loaded, control device \\\\.\\TempModFilter\n");

    return STATUS_SUCCESS;
}

