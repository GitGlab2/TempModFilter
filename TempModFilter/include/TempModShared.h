/*++
    TempModShared.h

    Contract shared between the kernel-mode filter driver and the user-mode
    client.  This is the ONLY file both sides include, therefore it must stay
    free of UM/KM specific headers.

    Include order:
        kernel  : #include <ntddk.h>   then this file
        user    : #include <windows.h> then <winioctl.h> then this file

    Layout rules:
        - fixed-width/padded members, no bitfields, no pointers
        - identical packing on both sides (natural alignment)
--*/

#pragma once

/* ------------------------------------------------------------------------- */
/* Configuration                                                              */
/* ------------------------------------------------------------------------- */

#define TEMPMOD_POLICY_VERSION           1u
#define TEMPMOD_STATS_VERSION            1u

#define TEMPMOD_PATTERN_MAX              64u
#define TEMPMOD_DEFAULT_MAX_REPLACEMENTS 256u
#define TEMPMOD_MAX_REPLACEMENTS         4096u

/* IOCTL_TEMPMOD_SET_POLICY flags */
#define TEMPMOD_FLAG_RESERVED            0u

/* ------------------------------------------------------------------------- */
/* Names                                                                     */
/* ------------------------------------------------------------------------- */

#define TEMPMOD_DEVICE_NAME  L"\\Device\\TempModFilter"
#define TEMPMOD_DOS_NAME     L"\\DosDevices\\TempModFilter"
#define TEMPMOD_WIN32_NAME   L"\\\\.\\TempModFilter"

/* ------------------------------------------------------------------------- */
/* IOCTL codes                                                               */
/* ------------------------------------------------------------------------- */

#define TEMPMOD_DEVICE_TYPE  FILE_DEVICE_UNKNOWN
#define TEMPMOD_IOCTL_BASE   0x800

/*
    Arm / re-arm the filter.

    Input : TEMPMOD_POLICY
    Output: TEMPMOD_POLICY (normalised copy actually stored by the driver)
*/
#define IOCTL_TEMPMOD_SET_POLICY        \
    CTL_CODE(TEMPMOD_DEVICE_TYPE, TEMPMOD_IOCTL_BASE + 0, METHOD_BUFFERED, FILE_WRITE_DATA)

/* Output: TEMPMOD_POLICY */
#define IOCTL_TEMPMOD_GET_POLICY        \
    CTL_CODE(TEMPMOD_DEVICE_TYPE, TEMPMOD_IOCTL_BASE + 1, METHOD_BUFFERED, FILE_READ_DATA)

/* Output: TEMPMOD_STATS */
#define IOCTL_TEMPMOD_GET_STATS         \
    CTL_CODE(TEMPMOD_DEVICE_TYPE, TEMPMOD_IOCTL_BASE + 2, METHOD_BUFFERED, FILE_READ_DATA)

/* No input, no output. Zeroes every counter. */
#define IOCTL_TEMPMOD_RESET_STATS       \
    CTL_CODE(TEMPMOD_DEVICE_TYPE, TEMPMOD_IOCTL_BASE + 3, METHOD_BUFFERED, FILE_WRITE_DATA)

/* No input, no output. Disarms the policy (pattern length = 0). */
#define IOCTL_TEMPMOD_CLEAR_POLICY      \
    CTL_CODE(TEMPMOD_DEVICE_TYPE, TEMPMOD_IOCTL_BASE + 4, METHOD_BUFFERED, FILE_WRITE_DATA)

/* ------------------------------------------------------------------------- */
/* Structures                                                                */
/* ------------------------------------------------------------------------- */

/*
    Policy = "inside every matching IOCTL output buffer, overwrite every
    occurrence of Pattern with Replacement".

    TargetIoControl == 0 means "any device control request".
    MaxReplacementsPerRequest == 0 means "use TEMPMOD_DEFAULT_MAX_REPLACEMENTS".
*/
typedef struct _TEMPMOD_POLICY
{
    ULONG Version;                     /* TEMPMOD_POLICY_VERSION             */
    ULONG Enabled;                     /* 0 = pass-through, 1 = active       */
    ULONG TargetIoControl;             /* exact IOCTL code, or 0 for all     */
    ULONG PatternLength;               /* 1 .. TEMPMOD_PATTERN_MAX           */
    UCHAR Pattern[TEMPMOD_PATTERN_MAX];/* bytes to look for                  */
    UCHAR Replacement;                 /* byte written over each match       */
    UCHAR ReservedBytes[3];            /* keep the layout stable             */
    ULONG MaxReplacementsPerRequest;   /* 0 .. TEMPMOD_MAX_REPLACEMENTS      */
    ULONG Flags;                       /* reserved, must be 0                */
} TEMPMOD_POLICY, *PTEMPMOD_POLICY;

typedef struct _TEMPMOD_STATS
{
    ULONG Version;                     /* TEMPMOD_STATS_VERSION              */
    ULONG RequestsObserved;            /* every request that crossed the     */
                                       /* filter queue                       */
    ULONG BuffersScanned;              /* output buffers actually inspected  */
    ULONG ReplacementsTotal;           /* pattern occurrences overwritten    */
    ULONG PolicyArmed;                 /* snapshot of Enabled at query time  */
    ULONG Reserved[3];
} TEMPMOD_STATS, *PTEMPMOD_STATS;

/* Compile-time layout guard (both sides must agree). */
typedef char TEMPMOD_POLICY_SIZE_CHECK[(sizeof(TEMPMOD_POLICY) == 92) ? 1 : -1];
typedef char TEMPMOD_STATS_SIZE_CHECK[(sizeof(TEMPMOD_STATS) == 32) ? 1 : -1];
