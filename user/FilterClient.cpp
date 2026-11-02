/*++
    FilterClient.cpp

    Small administrative / demonstration client for TempModFilter.

    It only talks to the driver's control device (\\\\.\\TempModFilter) and,
    for the "demo" command, to a disk device (\\\\.\\PhysicalDriveN, read-only
    queries) so you can observe the effect of the filter round-trip:

        serial before  ->  arm policy  ->  serial masked  ->
        clear policy   ->  serial back to the original

    That last step is the whole point: the driver changed nothing permanent.

    Requires an elevated console (the control device only allows SYSTEM and
    Administrators).
--*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../include/TempModShared.h"

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void PrintWin32Error(const char* what, DWORD error)
{
    char message[512] = { 0 };

    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        message, (DWORD)sizeof(message) - 1, NULL);

    /* Trim trailing CR/LF. */
    size_t length = strlen(message);
    while (length > 0 &&
           (message[length - 1] == '\r' || message[length - 1] == '\n')) {
        message[--length] = '\0';
    }

    fprintf(stderr, "[-] %s failed (error %lu): %s\n",
            what, (unsigned long)error, message);
}

static HANDLE OpenFilterDevice(void)
{
    HANDLE handle = CreateFileW(
        TEMPMOD_WIN32_NAME,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (handle == INVALID_HANDLE_VALUE) {
        PrintWin32Error("CreateFile(\\\\.\\TempModFilter)", GetLastError());
        fprintf(stderr,
                "    Is the driver installed and started?  "
                "Run scripts\\Install.ps1, then reboot.\n");
    }

    return handle;
}

static BOOL SendIoctl(
    HANDLE handle,
    DWORD  code,
    void*  input,
    DWORD  inputLength,
    void*  output,
    DWORD  outputLength,
    DWORD* returned)
{
    DWORD bytes = 0;

    if (!DeviceIoControl(handle, code, input, inputLength,
                         output, outputLength, &bytes, NULL)) {
        PrintWin32Error("DeviceIoControl", GetLastError());
        return FALSE;
    }

    if (returned != NULL) {
        *returned = bytes;
    }

    return TRUE;
}

static const char* YesNo(ULONG value)
{
    return (value != 0) ? "yes" : "no";
}

static void PrintPolicy(const TEMPMOD_POLICY& policy)
{
    printf("    version        : %lu\n", (unsigned long)policy.Version);
    printf("    armed          : %s\n", YesNo(policy.Enabled));
    printf("    target IOCTL   : 0x%08lX%s\n",
           (unsigned long)policy.TargetIoControl,
           (policy.TargetIoControl == 0) ? " (all device controls)" : "");
    printf("    pattern length : %lu\n", (unsigned long)policy.PatternLength);
    printf("    pattern        : ");
    for (ULONG i = 0; i < policy.PatternLength && i < TEMPMOD_PATTERN_MAX; i++) {
        unsigned char c = policy.Pattern[i];
        if (c >= 0x20 && c < 0x7F) {
            printf("%c", c);
        } else {
            printf("\\x%02X", c);
        }
    }
    if (policy.PatternLength == 0) {
        printf("(none)");
    }
    printf("\n");
    printf("    replacement    : 0x%02X\n", (unsigned)policy.Replacement);
    printf("    max per request: %lu\n",
           (unsigned long)policy.MaxReplacementsPerRequest);
}

static void PrintStats(const TEMPMOD_STATS& stats)
{
    printf("    version            : %lu\n", (unsigned long)stats.Version);
    printf("    requests observed  : %lu\n", (unsigned long)stats.RequestsObserved);
    printf("    buffers scanned    : %lu\n", (unsigned long)stats.BuffersScanned);
    printf("    replacements total : %lu\n", (unsigned long)stats.ReplacementsTotal);
    printf("    policy armed       : %s\n", YesNo(stats.PolicyArmed));
}

/* ------------------------------------------------------------------------- */
/* Argument parsing                                                          */
/* ------------------------------------------------------------------------- */

static const char* FindOption(int argc, char** argv, const char* name)
{
    for (int i = 2; i < (argc - 1); i++) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static BOOL HasFlag(int argc, char** argv, const char* name)
{
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL ParsePattern(const char* text, TEMPMOD_POLICY& policy)
{
    size_t length = 0;

    if (text == NULL) {
        return FALSE;
    }

    if (strncmp(text, "hex:", 4) == 0) {

        const char* cursor = text + 4;

        while (*cursor != '\0' && length < TEMPMOD_PATTERN_MAX) {

            char pair[3] = { 0 };
            char* end = NULL;
            unsigned long value;

            while (*cursor == ' ') {
                cursor++;
            }

            if (cursor[0] == '\0') {
                break;
            }

            pair[0] = cursor[0];
            pair[1] = (cursor[0] != '\0' && cursor[1] != '\0') ? cursor[1] : '0';

            value = strtoul(pair, &end, 16);
            if (end == pair || *end != '\0') {
                fprintf(stderr, "[-] invalid hex byte near '%s'\n", cursor);
                return FALSE;
            }

            policy.Pattern[length++] = (UCHAR)value;

            /* Skip the two hex digits. */
            cursor += (cursor[1] != '\0') ? 2 : 1;
        }

    } else {

        length = strlen(text);

        if (length == 0 || length > TEMPMOD_PATTERN_MAX) {
            fprintf(stderr,
                    "[-] pattern must be 1..%u bytes\n",
                    (unsigned)TEMPMOD_PATTERN_MAX);
            return FALSE;
        }

        memcpy(policy.Pattern, text, length);
    }

    if (length == 0) {
        fprintf(stderr, "[-] empty pattern\n");
        return FALSE;
    }

    policy.PatternLength = (ULONG)length;
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* Commands                                                                  */
/* ------------------------------------------------------------------------- */

static int CommandSet(int argc, char** argv)
{
    const char* patternText = FindOption(argc, argv, "--pattern");
    const char* replaceText = FindOption(argc, argv, "--replace");
    const char* ioctlText   = FindOption(argc, argv, "--ioctl");
    const char* maxText     = FindOption(argc, argv, "--max");

    if (patternText == NULL) {
        fprintf(stderr,
                "usage: FilterClient set --pattern <text|hex:41 42> "
                "[--replace 0x2A] [--ioctl 0x2D1400] [--max 8]\n");
        return 2;
    }

    TEMPMOD_POLICY policy;
    memset(&policy, 0, sizeof(policy));

    policy.Version = TEMPMOD_POLICY_VERSION;
    policy.Enabled = 1;
    policy.Replacement = (UCHAR)((replaceText != NULL)
                                     ? strtoul(replaceText, NULL, 0)
                                     : '*');
    policy.TargetIoControl = (ioctlText != NULL)
                                 ? (ULONG)strtoul(ioctlText, NULL, 0)
                                 : 0;
    policy.MaxReplacementsPerRequest = (maxText != NULL)
                                           ? (ULONG)strtoul(maxText, NULL, 0)
                                           : 0;

    if (!ParsePattern(patternText, policy)) {
        return 2;
    }

    HANDLE handle = OpenFilterDevice();
    if (handle == INVALID_HANDLE_VALUE) {
        return 1;
    }

    TEMPMOD_POLICY stored;
    memset(&stored, 0, sizeof(stored));

    BOOL ok = SendIoctl(handle, IOCTL_TEMPMOD_SET_POLICY,
                        &policy, sizeof(policy),
                        &stored, sizeof(stored), NULL);

    CloseHandle(handle);

    if (!ok) {
        return 1;
    }

    printf("[+] policy armed\n");
    PrintPolicy(stored);
    return 0;
}

static int CommandClear(void)
{
    HANDLE handle = OpenFilterDevice();
    if (handle == INVALID_HANDLE_VALUE) {
        return 1;
    }

    BOOL ok = SendIoctl(handle, IOCTL_TEMPMOD_CLEAR_POLICY,
                        NULL, 0, NULL, 0, NULL);

    CloseHandle(handle);

    if (!ok) {
        return 1;
    }

    printf("[+] policy cleared (pass-through mode)\n");
    return 0;
}

static int CommandStatus(void)
{
    HANDLE handle = OpenFilterDevice();
    if (handle == INVALID_HANDLE_VALUE) {
        return 1;
    }

    TEMPMOD_POLICY policy;
    memset(&policy, 0, sizeof(policy));

    BOOL ok = SendIoctl(handle, IOCTL_TEMPMOD_GET_POLICY,
                        NULL, 0, &policy, sizeof(policy), NULL);

    CloseHandle(handle);

    if (!ok) {
        return 1;
    }

    printf("[+] current policy\n");
    PrintPolicy(policy);
    return 0;
}

static int CommandStats(int reset)
{
    HANDLE handle = OpenFilterDevice();
    if (handle == INVALID_HANDLE_VALUE) {
        return 1;
    }

    if (reset) {
        if (!SendIoctl(handle, IOCTL_TEMPMOD_RESET_STATS,
                       NULL, 0, NULL, 0, NULL)) {
            CloseHandle(handle);
            return 1;
        }
        printf("[+] counters reset\n");
    }

    TEMPMOD_STATS stats;
    memset(&stats, 0, sizeof(stats));

    BOOL ok = SendIoctl(handle, IOCTL_TEMPMOD_GET_STATS,
                        NULL, 0, &stats, sizeof(stats), NULL);

    CloseHandle(handle);

    if (!ok) {
        return 1;
    }

    printf("[+] counters\n");
    PrintStats(stats);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Demo: prove the modification is volatile                                  */
/* ------------------------------------------------------------------------- */

static BOOL QueryDiskSerial(HANDLE disk, char* output, size_t outputLength)
{
    STORAGE_PROPERTY_QUERY query;
    BYTE  buffer[1024];
    DWORD returned = 0;

    memset(&query, 0, sizeof(query));
    query.PropertyId = StorageDeviceProperty;
    query.QueryType  = PropertyStandardQuery;

    if (!DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY,
                         &query, sizeof(query),
                         buffer, (DWORD)sizeof(buffer),
                         &returned, NULL)) {
        PrintWin32Error("IOCTL_STORAGE_QUERY_PROPERTY", GetLastError());
        return FALSE;
    }

    if (returned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        fprintf(stderr, "[-] short storage descriptor (%lu bytes)\n",
                (unsigned long)returned);
        return FALSE;
    }

    const STORAGE_DEVICE_DESCRIPTOR* descriptor =
        (const STORAGE_DEVICE_DESCRIPTOR*)buffer;

    if (descriptor->SerialNumberOffset == 0 ||
        descriptor->SerialNumberOffset >= returned) {
        fprintf(stderr, "[-] this device reports no serial number\n");
        return FALSE;
    }

    const char* serial =
        (const char*)buffer + descriptor->SerialNumberOffset;

    size_t length = 0;
    while ((descriptor->SerialNumberOffset + length) < returned &&
           serial[length] != '\0' &&
           length + 1 < outputLength) {
        length++;
    }

    memcpy(output, serial, length);
    output[length] = '\0';
    return TRUE;
}

static int CommandDemo(int argc, char** argv)
{
    const char* driveText = FindOption(argc, argv, "--drive");
    int driveIndex = (driveText != NULL) ? atoi(driveText) : 0;

    wchar_t path[64];
    _snwprintf_s(path, _countof(path), _TRUNCATE,
                 L"\\\\.\\PhysicalDrive%d", driveIndex);

    HANDLE disk = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ |
                                                  FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
    if (disk == INVALID_HANDLE_VALUE) {
        PrintWin32Error("CreateFile(PhysicalDrive)", GetLastError());
        fprintf(stderr, "    Run this from an elevated console.\n");
        return 1;
    }

    char before[256] = { 0 };
    char masked[256] = { 0 };
    char after[256]  = { 0 };

    if (!QueryDiskSerial(disk, before, sizeof(before))) {
        CloseHandle(disk);
        return 1;
    }

    printf("\n[1/4] serial BEFORE arming the filter : \"%s\"\n", before);

    if (strlen(before) == 0) {
        fprintf(stderr, "[-] empty serial, nothing to demonstrate\n");
        CloseHandle(disk);
        return 1;
    }

    HANDLE filter = OpenFilterDevice();
    if (filter == INVALID_HANDLE_VALUE) {
        CloseHandle(disk);
        return 1;
    }

    TEMPMOD_POLICY policy;
    memset(&policy, 0, sizeof(policy));
    policy.Version = TEMPMOD_POLICY_VERSION;
    policy.Enabled = 1;
    policy.TargetIoControl = IOCTL_STORAGE_QUERY_PROPERTY;
    policy.Replacement = (UCHAR)'*';
    policy.MaxReplacementsPerRequest = 16;

    if (!ParsePattern(before, policy)) {
        CloseHandle(filter);
        CloseHandle(disk);
        return 1;
    }

    TEMPMOD_POLICY stored;
    memset(&stored, 0, sizeof(stored));

    if (!SendIoctl(filter, IOCTL_TEMPMOD_SET_POLICY,
                   &policy, sizeof(policy), &stored, sizeof(stored), NULL)) {
        CloseHandle(filter);
        CloseHandle(disk);
        return 1;
    }

    printf("[2/4] policy armed for IOCTL 0x%08lX, pattern \"%s\"\n",
           (unsigned long)IOCTL_STORAGE_QUERY_PROPERTY, before);

    if (!QueryDiskSerial(disk, masked, sizeof(masked))) {
        CloseHandle(filter);
        CloseHandle(disk);
        return 1;
    }

    printf("[3/4] serial WITH the filter active    : \"%s\"\n", masked);

    if (!SendIoctl(filter, IOCTL_TEMPMOD_CLEAR_POLICY, NULL, 0, NULL, 0, NULL)) {
        CloseHandle(filter);
        CloseHandle(disk);
        return 1;
    }

    CloseHandle(filter);

    if (!QueryDiskSerial(disk, after, sizeof(after))) {
        CloseHandle(disk);
        return 1;
    }

    printf("[4/4] serial AFTER clearing the policy : \"%s\"\n", after);

    CloseHandle(disk);

    BOOL maskedOk = (strcmp(before, masked) != 0) &&
                    (strchr(masked, '*') != NULL);
    BOOL restored = (strcmp(before, after) == 0);

    printf("\n");
    printf("    modified in RAM      : %s\n", maskedOk ? "yes" : "NO");
    printf("    restored after clear : %s\n", restored ? "yes" : "NO");
    printf("    nothing touched the disk: the buffer only ever existed "
           "in memory\n");

    if (!maskedOk || !restored) {
        fprintf(stderr,
                "\n[-] unexpected result. Check that the filter is attached "
                "to the disk class (see scripts\\Install.ps1).\n");
        return 1;
    }

    printf("\n[+] round-trip OK\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Simulate: run the same redaction algorithm in user mode (no driver)       */
/* ------------------------------------------------------------------------- */

static ULONG SimulateRedact(BYTE* buffer, size_t length, const TEMPMOD_POLICY& policy)
{
    size_t patternLength = policy.PatternLength;

    if (patternLength == 0 ||
        patternLength > TEMPMOD_PATTERN_MAX ||
        patternLength > length) {
        return 0;
    }

    ULONG limit = policy.MaxReplacementsPerRequest;
    if (limit == 0) {
        limit = TEMPMOD_DEFAULT_MAX_REPLACEMENTS;
    }

    ULONG replacements = 0;

    for (size_t index = 0;
         (index + patternLength) <= length && replacements < limit;
         index++) {

        if (buffer[index] != policy.Pattern[0]) {
            continue;
        }

        if (memcmp(buffer + index, policy.Pattern, patternLength) != 0) {
            continue;
        }

        memset(buffer + index, policy.Replacement, patternLength);
        replacements++;
        index += (patternLength - 1);
    }

    return replacements;
}

static void DumpBytes(const char* label, const BYTE* data, size_t length)
{
    printf("  %s\n", label);

    for (size_t offset = 0; offset < length; offset += 16) {
        printf("    %04zX  ", offset);

        for (size_t i = 0; i < 16; i++) {
            if (offset + i < length) {
                printf("%02X ", data[offset + i]);
            } else {
                printf("   ");
            }
        }

        printf(" |");

        for (size_t i = 0; i < 16 && offset + i < length; i++) {
            BYTE c = data[offset + i];
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }

        printf("|\n");
    }
}

static int CommandSimulate(int argc, char** argv)
{
    const char* patternText = FindOption(argc, argv, "--pattern");
    const char* replaceText = FindOption(argc, argv, "--replace");
    const char* maxText     = FindOption(argc, argv, "--max");
    const char* sampleText  = FindOption(argc, argv, "--sample");

    TEMPMOD_POLICY policy;
    memset(&policy, 0, sizeof(policy));

    policy.Version = TEMPMOD_POLICY_VERSION;
    policy.Enabled = 1;
    policy.Replacement = (UCHAR)((replaceText != NULL)
                                     ? strtoul(replaceText, NULL, 0)
                                     : '*');
    policy.MaxReplacementsPerRequest = (maxText != NULL)
                                           ? (ULONG)strtoul(maxText, NULL, 0)
                                           : 0;

    if (!ParsePattern((patternText != NULL) ? patternText
                                            : "WD-WCC4E1234567",
                      policy)) {
        return 2;
    }

    const char* sample = (sampleText != NULL)
        ? sampleText
        : "serial: WD-WCC4E1234567\r\n"
          "model : ST2000DM001\r\n"
          "serial: WD-WCC4E1234567\r\n"
          "flag  : keep-me-intact\r\n";

    size_t length = strlen(sample);
    if (length == 0) {
        fprintf(stderr, "[-] empty sample\n");
        return 2;
    }

    BYTE* buffer = (BYTE*)malloc(length);
    if (buffer == NULL) {
        fprintf(stderr, "[-] out of memory\n");
        return 1;
    }
    memcpy(buffer, sample, length);

    printf("[*] user-mode simulation - no driver, no disk access\n\n");
    PrintPolicy(policy);
    printf("\n");

    DumpBytes("before", buffer, length);

    ULONG replacements = SimulateRedact(buffer, length, policy);

    printf("\n");
    DumpBytes("after", buffer, length);

    printf("\n    replacements   : %lu\n", (unsigned long)replacements);
    printf("    scope          : only the copy above, nothing was written\n");

    free(buffer);
    return (replacements > 0) ? 0 : 1;
}

/* ------------------------------------------------------------------------- */

static void Usage(void)
{
    printf(
        "TempModFilter client\n"
        "\n"
        "  status                      show the armed policy\n"
        "  stats [--reset]             show (or reset) counters\n"
        "  set --pattern <p> [opts]    arm the filter\n"
        "        --replace 0x2A        byte written over each match ('*')\n"
        "        --ioctl   0x2D1400    target IOCTL, 0 = all\n"
        "        --max     8           replacements per request\n"
        "  clear                       disarm (pass-through)\n"
        "  demo [--drive 0]            before/after round-trip on a disk\n"
        "  simulate [--pattern <p>]    run the redaction algorithm in user\n"
        "        [--replace 0x2A] [--max 8] [--sample <text>]\n"
        "                              mode, no driver required\n"
        "  help                        this text\n"
        "\n"
        "  pattern syntax:  \"ABC123\"  or  \"hex:41 42 43\"\n");
}

int main(int argc, char** argv)
{
    /* Arabic/UTF-8 friendly console, harmless otherwise. */
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        Usage();
        return 2;
    }

    const char* command = argv[1];

    if (_stricmp(command, "set") == 0) {
        return CommandSet(argc, argv);
    }
    if (_stricmp(command, "clear") == 0) {
        return CommandClear();
    }
    if (_stricmp(command, "status") == 0 || _stricmp(command, "get") == 0) {
        return CommandStatus();
    }
    if (_stricmp(command, "stats") == 0) {
        return CommandStats(HasFlag(argc, argv, "--reset"));
    }
    if (_stricmp(command, "demo") == 0) {
        return CommandDemo(argc, argv);
    }
    if (_stricmp(command, "simulate") == 0) {
        return CommandSimulate(argc, argv);
    }
    if (_stricmp(command, "help") == 0 || strcmp(command, "--help") == 0) {
        Usage();
        return 0;
    }

    fprintf(stderr, "[-] unknown command: %s\n\n", command);
    Usage();
    return 2;
}
