/*++
    FilterClient.cpp

    Administrative / demonstration client for TempModFilter, now an HWID
    spoofer front-end.

    Requires an elevated console (the control device only allows SYSTEM and
    Administrators).
--*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <rpc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>
#include <time.h>
#include <oleauto.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "rpcrt4.lib")

#include "../include/TempModShared.h"

/* ------------------------------------------------------------------------- */
/* Constants                                                                  */
/* ------------------------------------------------------------------------- */

static const char* BACKUP_DIR  = "C:\\ProgramData\\TempModFilter";
static const char* BACKUP_FILE = "C:\\ProgramData\\TempModFilter\\identity-backup.txt";

static const char* DISK_IOCTL_NAME = "IOCTL_STORAGE_QUERY_PROPERTY";

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

static void RandomBytes(unsigned char* buf, size_t n)
{
    if (buf == NULL || n == 0) return;
    BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

static void RandomAlnum(char* out, size_t n)
{
    static const char* alpha = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    unsigned char tmp[256];
    size_t i;
    if (n == 0) { out[0] = '\0'; return; }
    if (n > sizeof(tmp)) n = sizeof(tmp);
    RandomBytes(tmp, n);
    for (i = 0; i < n; i++) {
        out[i] = alpha[tmp[i] % 36];
    }
    out[n] = '\0';
}

static void MakeGuidString(char* out, size_t outlen)
{
    UUID u;
    (void)UuidCreate(&u);
    _snprintf_s(out, outlen, _TRUNCATE,
                "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                (unsigned long)u.Data1, (unsigned)u.Data2, (unsigned)u.Data3,
                (unsigned)u.Data4[0], (unsigned)u.Data4[1],
                (unsigned)u.Data4[2], (unsigned)u.Data4[3],
                (unsigned)u.Data4[4], (unsigned)u.Data4[5],
                (unsigned)u.Data4[6], (unsigned)u.Data4[7]);
}

static void MakeRandomMac(char* out)
{
    unsigned char b[6];
    RandomBytes(b, 6);
    b[0] = (unsigned char)((b[0] & 0xFC) | 0x02); /* locally administered, unicast */
    _snprintf_s(out, 13, _TRUNCATE, "%02X%02X%02X%02X%02X%02X",
                b[0], b[1], b[2], b[3], b[4], b[5]);
}

/* ------------------------------------------------------------------------- */
/* Policy printing                                                           */
/* ------------------------------------------------------------------------- */

static void PrintPolicy(const TEMPMOD_POLICY& policy)
{
    printf("    version        : %lu\n", (unsigned long)policy.Version);
    printf("    armed          : %s\n", YesNo(policy.Enabled));
    printf("    rule count     : %lu\n", (unsigned long)policy.RuleCount);
    printf("    max per request: %lu\n", (unsigned long)policy.MaxReplacementsPerRequest);
    for (ULONG r = 0; r < policy.RuleCount && r < TEMPMOD_MAX_RULES; r++) {
        const TEMPMOD_RULE& rule = policy.Rules[r];
        printf("  [rule %lu]\n", (unsigned long)r);
        printf("    target IOCTL   : 0x%08lX%s\n",
               (unsigned long)rule.TargetIoControl,
               (rule.TargetIoControl == 0) ? " (all device controls)" : "");
        printf("    pattern length : %lu\n", (unsigned long)rule.PatternLength);
        printf("    pattern        : ");
        for (ULONG i = 0; i < rule.PatternLength && i < TEMPMOD_PATTERN_MAX; i++) {
            unsigned char c = rule.Pattern[i];
            if (c >= 0x20 && c < 0x7F) printf("%c", c);
            else printf("\\x%02X", c);
        }
        if (rule.PatternLength == 0) printf("(none)");
        printf("\n");
        printf("    replacement    : ");
        ULONG rlen = rule.ReplacementLength;
        if (rlen == 0) rlen = rule.PatternLength;
        if (rlen == 0) {
            printf("(none)");
        } else {
            BOOL printable = TRUE;
            for (ULONG i = 0; i < rlen; i++) {
                if (rule.Replacement[i] < 0x20 || rule.Replacement[i] >= 0x7F) {
                    printable = FALSE;
                    break;
                }
            }
            if (printable) {
                for (ULONG i = 0; i < rlen; i++) printf("%c", (char)rule.Replacement[i]);
            } else {
                for (ULONG i = 0; i < rlen; i++) printf("\\x%02X", (unsigned)rule.Replacement[i]);
            }
        }
        printf("  (length %lu)\n", (unsigned long)rlen);
    }
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

static BOOL ParsePattern(const char* text, TEMPMOD_RULE& rule)
{
    size_t length = 0;

    if (text == NULL) return FALSE;

    if (strncmp(text, "hex:", 4) == 0) {
        const char* cursor = text + 4;
        while (*cursor != '\0' && length < TEMPMOD_PATTERN_MAX) {
            char pair[3] = { 0 };
            char* end = NULL;
            unsigned long value;
            while (*cursor == ' ') cursor++;
            if (cursor[0] == '\0') break;
            pair[0] = cursor[0];
            pair[1] = (cursor[0] != '\0' && cursor[1] != '\0') ? cursor[1] : '0';
            value = strtoul(pair, &end, 16);
            if (end == pair || *end != '\0') {
                fprintf(stderr, "[-] invalid hex byte near '%s'\n", cursor);
                return FALSE;
            }
            rule.Pattern[length++] = (UCHAR)value;
            cursor += (cursor[1] != '\0') ? 2 : 1;
        }
    } else {
        length = strlen(text);
        if (length == 0 || length > TEMPMOD_PATTERN_MAX) {
            fprintf(stderr, "[-] pattern must be 1..%u bytes\n", (unsigned)TEMPMOD_PATTERN_MAX);
            return FALSE;
        }
        memcpy(rule.Pattern, text, length);
    }

    if (length == 0) {
        fprintf(stderr, "[-] empty pattern\n");
        return FALSE;
    }

    rule.PatternLength = (ULONG)length;
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* Base64 helpers                                                            */
/* ------------------------------------------------------------------------- */

static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void Base64Encode(const unsigned char* in, size_t inlen, char* out, size_t outlen)
{
    size_t i, j = 0;
    for (i = 0; i < inlen && j + 4 < outlen; i += 3) {
        unsigned int v = (in[i] << 16) | ((i + 1 < inlen ? in[i + 1] : 0) << 8) | (i + 2 < inlen ? in[i + 2] : 0);
        out[j++] = B64[(v >> 18) & 0x3F];
        out[j++] = B64[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < inlen) ? B64[(v >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < inlen) ? B64[v & 0x3F] : '=';
    }
    out[j] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Registry helpers                                                          */
/* ------------------------------------------------------------------------- */

static BOOL RegReadString(HKEY root, const char* subkey, const char* name, char* out, size_t outlen)
{
    HKEY key = NULL;
    LONG res = RegOpenKeyExA(root, subkey, 0, KEY_QUERY_VALUE, &key);
    if (res != ERROR_SUCCESS) return FALSE;
    DWORD type = 0, size = (DWORD)outlen;
    res = RegQueryValueExA(key, name, NULL, &type, (LPBYTE)out, &size);
    RegCloseKey(key);
    if (res != ERROR_SUCCESS || type != REG_SZ) return FALSE;
    out[outlen - 1] = '\0';
    return TRUE;
}

static BOOL RegReadDword(HKEY root, const char* subkey, const char* name, DWORD* out, BOOL* exists)
{
    HKEY key = NULL;
    LONG res = RegOpenKeyExA(root, subkey, 0, KEY_QUERY_VALUE, &key);
    if (res != ERROR_SUCCESS) { if (exists) *exists = FALSE; return FALSE; }
    DWORD type = 0, size = sizeof(DWORD), val = 0;
    res = RegQueryValueExA(key, name, NULL, &type, (LPBYTE)&val, &size);
    RegCloseKey(key);
    if (res != ERROR_SUCCESS) { if (exists) *exists = FALSE; return FALSE; }
    if (exists) *exists = TRUE;
    if (type == REG_DWORD && out) *out = val;
    return TRUE;
}

static BOOL RegWriteString(HKEY root, const char* subkey, const char* name, const char* data)
{
    HKEY key = NULL;
    LONG res = RegOpenKeyExA(root, subkey, 0, KEY_SET_VALUE, &key);
    if (res != ERROR_SUCCESS) {
        res = RegCreateKeyExA(root, subkey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
        if (res != ERROR_SUCCESS) return FALSE;
    }
    res = RegSetValueExA(key, name, 0, REG_SZ, (const BYTE*)data, (DWORD)(strlen(data) + 1));
    RegCloseKey(key);
    return (res == ERROR_SUCCESS);
}

static BOOL RegWriteDword(HKEY root, const char* subkey, const char* name, DWORD data)
{
    HKEY key = NULL;
    LONG res = RegOpenKeyExA(root, subkey, 0, KEY_SET_VALUE, &key);
    if (res != ERROR_SUCCESS) {
        res = RegCreateKeyExA(root, subkey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
        if (res != ERROR_SUCCESS) return FALSE;
    }
    res = RegSetValueExA(key, name, 0, REG_DWORD, (const BYTE*)&data, sizeof(DWORD));
    RegCloseKey(key);
    return (res == ERROR_SUCCESS);
}

static BOOL RegDeleteValue2(HKEY root, const char* subkey, const char* name)
{
    HKEY key = NULL;
    LONG res = RegOpenKeyExA(root, subkey, 0, KEY_SET_VALUE, &key);
    if (res != ERROR_SUCCESS) return FALSE;
    res = RegDeleteValueA(key, name);
    RegCloseKey(key);
    return (res == ERROR_SUCCESS || res == ERROR_FILE_NOT_FOUND);
}

/* ------------------------------------------------------------------------- */
/* Backup / restore                                                          */
/* ------------------------------------------------------------------------- */

static void BackupEnsureDir(void)
{
    CreateDirectoryA(BACKUP_DIR, NULL);
}

static BOOL BackupContains(const char* key, const char* value)
{
    FILE* f = fopen(BACKUP_FILE, "r");
    if (!f) return FALSE;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        /* line: key\tvalue\ttype\tdata */
        char k[512] = { 0 }, v[256] = { 0 };
        if (sscanf(line, "%511[^\t]\t%255[^\t]", k, v) >= 2) {
            if (strcmp(k, key) == 0 && strcmp(v, value) == 0) {
                fclose(f);
                return TRUE;
            }
        }
    }
    fclose(f);
    return FALSE;
}

static void BackupValue(const char* key, const char* value)
{
    if (BackupContains(key, value)) return;
    BackupEnsureDir();

    /* Read current value. */
    char data[512] = { 0 };
    DWORD dword = 0;
    BOOL exists = FALSE;
    BOOL isDword = FALSE;

    /* Try string first. */
    if (RegReadString(HKEY_LOCAL_MACHINE, key, value, data, sizeof(data))) {
        exists = TRUE;
    } else {
        BOOL e = FALSE;
        if (RegReadDword(HKEY_LOCAL_MACHINE, key, value, &dword, &e) && e) {
            exists = TRUE;
            isDword = TRUE;
        } else {
            exists = FALSE;
        }
    }

    FILE* f = fopen(BACKUP_FILE, "a");
    if (!f) return;

    if (!exists) {
        fprintf(f, "%s\t%s\tMISSING\t\n", key, value);
    } else if (isDword) {
        fprintf(f, "%s\t%s\tDWORD\t%08lX\n", key, value, (unsigned long)dword);
    } else {
        char enc[1024] = { 0 };
        Base64Encode((const unsigned char*)data, strlen(data), enc, sizeof(enc));
        fprintf(f, "%s\t%s\tSZ\t%s\n", key, value, enc);
    }
    fclose(f);
}

static void RestoreBackup(void)
{
    FILE* f = fopen(BACKUP_FILE, "r");
    if (!f) {
        printf("[*] no backup file found.\n");
        return;
    }
    char line[4096];
    int restored = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char key[512] = { 0 }, val[256] = { 0 }, type[32] = { 0 }, payload[2048] = { 0 };
        /* parse tab-separated */
        char* p = line;
        char* t1 = strchr(p, '\t');
        if (!t1) continue;
        *t1 = '\0'; strncpy(key, p, sizeof(key)-1); p = t1 + 1;
        char* t2 = strchr(p, '\t');
        if (!t2) continue;
        *t2 = '\0'; strncpy(val, p, sizeof(val)-1); p = t2 + 1;
        char* t3 = strchr(p, '\t');
        if (!t3) continue;
        *t3 = '\0'; strncpy(type, p, sizeof(type)-1); p = t3 + 1;
        /* payload until newline */
        size_t pl = strlen(p);
        while (pl > 0 && (p[pl-1] == '\r' || p[pl-1] == '\n')) p[--pl] = '\0';
        strncpy(payload, p, sizeof(payload)-1);

        if (strcmp(type, "MISSING") == 0) {
            RegDeleteValue2(HKEY_LOCAL_MACHINE, key, val);
            restored++;
        } else if (strcmp(type, "DWORD") == 0) {
            unsigned long v = strtoul(payload, NULL, 16);
            RegWriteDword(HKEY_LOCAL_MACHINE, key, val, (DWORD)v);
            restored++;
        } else if (strcmp(type, "SZ") == 0) {
            /* base64 decode */
            /* simple decode */
            char dec[1024] = { 0 };
            size_t din = 0, i = 0;
            /* build index */
            int idx[256]; for (int x = 0; x < 256; x++) idx[x] = -1;
            for (int x = 0; x < 64; x++) idx[(unsigned char)B64[x]] = x;
            size_t len = strlen(payload);
            unsigned char buf[3];
            int bufpos = 0;
            for (i = 0; i < len; i++) {
                if (payload[i] == '=') break;
                int c = idx[(unsigned char)payload[i]];
                if (c < 0) continue;
                buf[bufpos++] = (unsigned char)c;
                if (bufpos == 4) {
                    dec[din++] = (char)((buf[0] << 2) | (buf[1] >> 4));
                    dec[din++] = (char)((buf[1] << 4) | (buf[2] >> 2));
                    dec[din++] = (char)((buf[2] << 6) | buf[3]);
                    bufpos = 0;
                    if (din >= sizeof(dec) - 4) break;
                }
            }
            if (bufpos >= 2) {
                dec[din++] = (char)((buf[0] << 2) | (buf[1] >> 4));
                if (bufpos >= 3) dec[din++] = (char)((buf[1] << 4) | (buf[2] >> 2));
            }
            dec[din] = '\0';
            RegWriteString(HKEY_LOCAL_MACHINE, key, val, dec);
            restored++;
        }
    }
    fclose(f);
    printf("[+] restored %d value(s) from backup.\n", restored);
    DeleteFileA(BACKUP_FILE);
}

/* ------------------------------------------------------------------------- */
/* Disk info                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct _DiskInfo {
    int index;
    char vendor[128];
    char product[128];
    char revision[64];
    char serial[256];
    BOOL ok;
} DiskInfo;

static BOOL QueryDiskInfo(int driveIndex, DiskInfo* info)
{
    memset(info, 0, sizeof(*info));
    info->index = driveIndex;
    info->ok = FALSE;

    wchar_t path[64];
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"\\\\.\\PhysicalDrive%d", driveIndex);

    HANDLE disk = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
    if (disk == INVALID_HANDLE_VALUE) return FALSE;

    STORAGE_PROPERTY_QUERY query;
    memset(&query, 0, sizeof(query));
    query.PropertyId = StorageDeviceProperty;
    query.QueryType  = PropertyStandardQuery;

    BYTE buffer[4096];
    DWORD returned = 0;
    if (!DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY,
                         &query, sizeof(query),
                         buffer, sizeof(buffer),
                         &returned, NULL)) {
        CloseHandle(disk);
        return FALSE;
    }

    if (returned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        CloseHandle(disk);
        return FALSE;
    }

    const STORAGE_DEVICE_DESCRIPTOR* d = (const STORAGE_DEVICE_DESCRIPTOR*)buffer;

    if (d->VendorIdOffset && d->VendorIdOffset < returned) {
        strncpy(info->vendor, (const char*)buffer + d->VendorIdOffset, sizeof(info->vendor) - 1);
    }
    if (d->ProductIdOffset && d->ProductIdOffset < returned) {
        strncpy(info->product, (const char*)buffer + d->ProductIdOffset, sizeof(info->product) - 1);
    }
    if (d->ProductRevisionOffset && d->ProductRevisionOffset < returned) {
        strncpy(info->revision, (const char*)buffer + d->ProductRevisionOffset, sizeof(info->revision) - 1);
    }
    if (d->SerialNumberOffset && d->SerialNumberOffset < returned) {
        strncpy(info->serial, (const char*)buffer + d->SerialNumberOffset, sizeof(info->serial) - 1);
    }

    CloseHandle(disk);
    info->ok = TRUE;
    return TRUE;
}

static void PrintDiskInfo(const DiskInfo* info)
{
    printf("  PhysicalDrive%d\n", info->index);
    if (!info->ok) { printf("    (not accessible)\n"); return; }
    printf("    vendor   : %s\n", info->vendor[0] ? info->vendor : "(none)");
    printf("    model    : %s\n", info->product[0] ? info->product : "(none)");
    printf("    firmware : %s\n", info->revision[0] ? info->revision : "(none)");
    printf("    serial   : %s\n", info->serial[0] ? info->serial : "(none)");
}

/* ------------------------------------------------------------------------- */
/* Low-level commands                                                        */
/* ------------------------------------------------------------------------- */

static int CommandStatus(void)
{
    HANDLE handle = OpenFilterDevice();
    if (handle == INVALID_HANDLE_VALUE) return 1;

    TEMPMOD_POLICY policy;
    memset(&policy, 0, sizeof(policy));

    BOOL ok = SendIoctl(handle, IOCTL_TEMPMOD_GET_POLICY, NULL, 0, &policy, sizeof(policy), NULL);
    CloseHandle(handle);
    if (!ok) return 1;

    printf("[+] current policy\n");
    PrintPolicy(policy);
    return 0;
}

static int CommandStats(int reset)
{
    HANDLE handle = OpenFilterDevice();
    if (handle == INVALID_HANDLE_VALUE) return 1;

    if (reset) {
        if (!SendIoctl(handle, IOCTL_TEMPMOD_RESET_STATS, NULL, 0, NULL, 0, NULL)) {
            CloseHandle(handle);
            return 1;
        }
        printf("[+] counters reset\n");
    }

    TEMPMOD_STATS stats;
    memset(&stats, 0, sizeof(stats));

    BOOL ok = SendIoctl(handle, IOCTL_TEMPMOD_GET_STATS, NULL, 0, &stats, sizeof(stats), NULL);
    CloseHandle(handle);
    if (!ok) return 1;

    printf("[+] counters\n");
    PrintStats(stats);
    return 0;
}

static int CommandClear(void)
{
    HANDLE handle = OpenFilterDevice();
    if (handle == INVALID_HANDLE_VALUE) return 1;

    BOOL ok = SendIoctl(handle, IOCTL_TEMPMOD_CLEAR_POLICY, NULL, 0, NULL, 0, NULL);
    CloseHandle(handle);
    if (!ok) return 1;

    printf("[+] policy cleared (pass-through mode)\n");
    return 0;
}

static int CommandSet(int argc, char** argv)
{
    const char* patternText      = FindOption(argc, argv, "--pattern");
    const char* replaceText      = FindOption(argc, argv, "--replace");
    const char* replacementText  = FindOption(argc, argv, "--replacement");
    const char* ioctlText        = FindOption(argc, argv, "--ioctl");
    const char* maxText          = FindOption(argc, argv, "--max");

    if (patternText == NULL) {
        fprintf(stderr, "usage: FilterClient set --pattern <text|hex:41 42> [--replacement <s>|--replace 0x2A] [--ioctl 0x..] [--max N]\n");
        return 2;
    }

    TEMPMOD_POLICY policy;
    memset(&policy, 0, sizeof(policy));
    policy.Version = TEMPMOD_POLICY_VERSION;
    policy.Enabled = 1;
    policy.RuleCount = 1;

    TEMPMOD_RULE& rule = policy.Rules[0];
    memset(&rule, 0, sizeof(rule));

    if (replacementText != NULL) {
        size_t rlen = strlen(replacementText);
        if (rlen > TEMPMOD_PATTERN_MAX) rlen = TEMPMOD_PATTERN_MAX;
        memcpy(rule.Replacement, replacementText, rlen);
        rule.ReplacementLength = (ULONG)rlen;
    } else {
        UCHAR byte = (UCHAR)((replaceText != NULL) ? strtoul(replaceText, NULL, 0) : '*');
        for (ULONG i = 0; i < TEMPMOD_PATTERN_MAX; i++) rule.Replacement[i] = byte;
        rule.ReplacementLength = 0;
    }

    rule.TargetIoControl = (ioctlText != NULL) ? (ULONG)strtoul(ioctlText, NULL, 0) : 0;

    if (!ParsePattern(patternText, rule)) return 2;

    if (rule.ReplacementLength > rule.PatternLength) rule.ReplacementLength = rule.PatternLength;

    policy.MaxReplacementsPerRequest = (maxText != NULL) ? (ULONG)strtoul(maxText, NULL, 0) : 0;

    HANDLE handle = OpenFilterDevice();
    if (handle == INVALID_HANDLE_VALUE) return 1;

    TEMPMOD_POLICY stored;
    memset(&stored, 0, sizeof(stored));

    BOOL ok = SendIoctl(handle, IOCTL_TEMPMOD_SET_POLICY, &policy, sizeof(policy), &stored, sizeof(stored), NULL);
    CloseHandle(handle);
    if (!ok) return 1;

    printf("[+] policy armed\n");
    PrintPolicy(stored);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Demo                                                                      */
/* ------------------------------------------------------------------------- */

static int CommandDemo(int argc, char** argv)
{
    const char* driveText = FindOption(argc, argv, "--drive");
    int driveIndex = (driveText != NULL) ? atoi(driveText) : 0;

    wchar_t path[64];
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"\\\\.\\PhysicalDrive%d", driveIndex);

    HANDLE disk = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
    if (disk == INVALID_HANDLE_VALUE) {
        PrintWin32Error("CreateFile(PhysicalDrive)", GetLastError());
        return 1;
    }

    /* query serial via temp function */
    DiskInfo info;
    if (!QueryDiskInfo(driveIndex, &info) || !info.serial[0]) {
        fprintf(stderr, "[-] no serial for drive %d\n", driveIndex);
        CloseHandle(disk);
        return 1;
    }

    char before[256], masked[256], after[256];
    strcpy_s(before, sizeof(before), info.serial);
    memset(masked, 0, sizeof(masked));
    memset(after, 0, sizeof(after));

    printf("\n[1/4] serial BEFORE arming the filter : \"%s\"\n", before);

    HANDLE filter = OpenFilterDevice();
    if (filter == INVALID_HANDLE_VALUE) { CloseHandle(disk); return 1; }

    TEMPMOD_POLICY policy;
    memset(&policy, 0, sizeof(policy));
    policy.Version = TEMPMOD_POLICY_VERSION;
    policy.Enabled = 1;
    policy.RuleCount = 1;
    policy.Rules[0].TargetIoControl = IOCTL_STORAGE_QUERY_PROPERTY;
    policy.Rules[0].Replacement[0] = (UCHAR)'*';
    policy.Rules[0].ReplacementLength = 0;
    policy.MaxReplacementsPerRequest = 16;
    if (!ParsePattern(before, policy.Rules[0])) { CloseHandle(filter); CloseHandle(disk); return 1; }

    TEMPMOD_POLICY stored;
    memset(&stored, 0, sizeof(stored));
    if (!SendIoctl(filter, IOCTL_TEMPMOD_SET_POLICY, &policy, sizeof(policy), &stored, sizeof(stored), NULL)) {
        CloseHandle(filter); CloseHandle(disk); return 1;
    }

    printf("[2/4] policy armed for IOCTL 0x%08lX, pattern \"%s\"\n",
           (unsigned long)IOCTL_STORAGE_QUERY_PROPERTY, before);

    DiskInfo info2;
    if (QueryDiskInfo(driveIndex, &info2)) strcpy_s(masked, sizeof(masked), info2.serial);
    printf("[3/4] serial WITH the filter active    : \"%s\"\n", masked);

    SendIoctl(filter, IOCTL_TEMPMOD_CLEAR_POLICY, NULL, 0, NULL, 0, NULL);
    CloseHandle(filter);

    DiskInfo info3;
    if (QueryDiskInfo(driveIndex, &info3)) strcpy_s(after, sizeof(after), info3.serial);
    printf("[4/4] serial AFTER clearing the policy : \"%s\"\n", after);

    CloseHandle(disk);

    BOOL maskedOk = (strcmp(before, masked) != 0);
    BOOL restored = (strcmp(before, after) == 0);

    printf("\n    modified in RAM      : %s\n", maskedOk ? "yes" : "NO");
    printf("    restored after clear : %s\n", restored ? "yes" : "NO");

    if (!maskedOk || !restored) {
        fprintf(stderr, "\n[-] unexpected result. Check that the filter is attached to the disk class.\n");
        return 1;
    }
    printf("\n[+] round-trip OK\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Spoof: disk                                                               */
/* ------------------------------------------------------------------------- */

static int AddDiskRule(TEMPMOD_POLICY& policy, const char* current, const char* replacement, ULONG ioctl)
{
    if (policy.RuleCount >= TEMPMOD_MAX_RULES) {
        fprintf(stderr, "[-] too many rules (max %u)\n", (unsigned)TEMPMOD_MAX_RULES);
        return 0;
    }
    if (current == NULL || current[0] == '\0') return 1; /* skip empty */

    TEMPMOD_RULE& rule = policy.Rules[policy.RuleCount];
    memset(&rule, 0, sizeof(rule));
    rule.TargetIoControl = ioctl;

    size_t clen = strlen(current);
    if (clen > TEMPMOD_PATTERN_MAX) clen = TEMPMOD_PATTERN_MAX;
    memcpy(rule.Pattern, current, clen);
    rule.PatternLength = (ULONG)clen;

    /* replacement padded/truncated to clen */
    char buf[TEMPMOD_PATTERN_MAX + 1];
    memset(buf, ' ', clen);
    buf[clen] = '\0';
    size_t rlen = strlen(replacement);
    if (rlen > clen) rlen = clen;
    memcpy(buf, replacement, rlen);
    memcpy(rule.Replacement, buf, clen);
    rule.ReplacementLength = (ULONG)rlen; /* driver pads rest with spaces */

    policy.RuleCount++;
    return 1;
}

static int CommandSpoofDisk(int argc, char** argv)
{
    const char* driveText = FindOption(argc, argv, "--drive");
    int driveIndex = (driveText != NULL) ? atoi(driveText) : 0;
    int allDrives = HasFlag(argc, argv, "--all-drives");
    int randomMode = HasFlag(argc, argv, "--random");
    int allFields = HasFlag(argc, argv, "--all-fields") || HasFlag(argc, argv, "--all");

    const char* serialVal = FindOption(argc, argv, "--serial");
    const char* modelVal  = FindOption(argc, argv, "--model");
    const char* fwVal     = FindOption(argc, argv, "--firmware");
    const char* vendVal   = FindOption(argc, argv, "--vendor");

    /* default field: serial */
    int doSerial = allFields || serialVal != NULL || !(HasFlag(argc, argv, "--model") || HasFlag(argc, argv, "--firmware") || HasFlag(argc, argv, "--vendor"));
    int doModel  = allFields || modelVal != NULL || HasFlag(argc, argv, "--model");
    int doFw     = allFields || fwVal != NULL || HasFlag(argc, argv, "--firmware");
    int doVend   = allFields || vendVal != NULL || HasFlag(argc, argv, "--vendor");

    /* determine target drives */
    int drives[16];
    int driveCount = 0;
    if (allDrives) {
        for (int d = 0; d < 16; d++) {
            DiskInfo di;
            if (QueryDiskInfo(d, &di) && di.ok) drives[driveCount++] = d;
        }
    } else {
        drives[driveCount++] = driveIndex;
    }

    if (driveCount == 0) {
        fprintf(stderr, "[-] no accessible drives\n");
        return 1;
    }

    TEMPMOD_POLICY policy;
    memset(&policy, 0, sizeof(policy));
    policy.Version = TEMPMOD_POLICY_VERSION;
    policy.Enabled = 1;
    policy.MaxReplacementsPerRequest = TEMPMOD_MAX_REPLACEMENTS;

    /* First pass: query and build rules. */
    DiskInfo infos[16];
    for (int i = 0; i < driveCount; i++) {
        QueryDiskInfo(drives[i], &infos[i]);
    }

    for (int i = 0; i < driveCount; i++) {
        DiskInfo* di = &infos[i];
        if (!di->ok) continue;

        if (doVend && di->vendor[0]) {
            char repl[TEMPMOD_PATTERN_MAX + 1];
            if (vendVal) strncpy(repl, vendVal, TEMPMOD_PATTERN_MAX);
            else if (randomMode) RandomAlnum(repl, strlen(di->vendor));
            else strncpy(repl, di->vendor, TEMPMOD_PATTERN_MAX);
            repl[TEMPMOD_PATTERN_MAX] = '\0';
            AddDiskRule(policy, di->vendor, repl, IOCTL_STORAGE_QUERY_PROPERTY);
        }
        if (doModel && di->product[0]) {
            char repl[TEMPMOD_PATTERN_MAX + 1];
            if (modelVal) strncpy(repl, modelVal, TEMPMOD_PATTERN_MAX);
            else if (randomMode) RandomAlnum(repl, strlen(di->product));
            else strncpy(repl, di->product, TEMPMOD_PATTERN_MAX);
            repl[TEMPMOD_PATTERN_MAX] = '\0';
            AddDiskRule(policy, di->product, repl, IOCTL_STORAGE_QUERY_PROPERTY);
        }
        if (doFw && di->revision[0]) {
            char repl[TEMPMOD_PATTERN_MAX + 1];
            if (fwVal) strncpy(repl, fwVal, TEMPMOD_PATTERN_MAX);
            else if (randomMode) RandomAlnum(repl, strlen(di->revision));
            else strncpy(repl, di->revision, TEMPMOD_PATTERN_MAX);
            repl[TEMPMOD_PATTERN_MAX] = '\0';
            AddDiskRule(policy, di->revision, repl, IOCTL_STORAGE_QUERY_PROPERTY);
        }
        if (doSerial && di->serial[0]) {
            char repl[TEMPMOD_PATTERN_MAX + 1];
            if (serialVal) strncpy(repl, serialVal, TEMPMOD_PATTERN_MAX);
            else if (randomMode) RandomAlnum(repl, strlen(di->serial));
            else strncpy(repl, di->serial, TEMPMOD_PATTERN_MAX);
            repl[TEMPMOD_PATTERN_MAX] = '\0';
            AddDiskRule(policy, di->serial, repl, IOCTL_STORAGE_QUERY_PROPERTY);
        }
    }

    if (policy.RuleCount == 0) {
        fprintf(stderr, "[-] no rules to apply (empty fields?)\n");
        return 1;
    }

    /* Arm. */
    HANDLE filter = OpenFilterDevice();
    if (filter == INVALID_HANDLE_VALUE) return 1;

    TEMPMOD_POLICY stored;
    memset(&stored, 0, sizeof(stored));
    if (!SendIoctl(filter, IOCTL_TEMPMOD_SET_POLICY, &policy, sizeof(policy), &stored, sizeof(stored), NULL)) {
        CloseHandle(filter);
        return 1;
    }
    CloseHandle(filter);

    printf("[+] disk identity spoof armed (%lu rule(s))\n", (unsigned long)stored.RuleCount);
    printf("    filter stays armed until 'clear' or reboot.\n\n");

    /* Show observed. */
    for (int i = 0; i < driveCount; i++) {
        DiskInfo di;
        if (QueryDiskInfo(drives[i], &di)) PrintDiskInfo(&di);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Spoof: MAC                                                                */
/* ------------------------------------------------------------------------- */

typedef struct _AdapterInfo {
    int index;
    char desc[256];
    char netCfg[256];
    char currentMac[32];
} AdapterInfo;

static int EnumerateAdapters(AdapterInfo* out, int maxOut)
{
    const char* base = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}";
    HKEY key = NULL;
    LONG res = RegOpenKeyExA(HKEY_LOCAL_MACHINE, base, 0, KEY_READ, &key);
    if (res != ERROR_SUCCESS) return 0;

    int count = 0;
    char subname[64];
    DWORD i = 0;
    for (i = 0; count < maxOut; i++) {
        DWORD slen = sizeof(subname);
        if (RegEnumKeyExA(key, i, subname, &slen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (strlen(subname) != 4) continue;
        /* check DriverDesc */
        char path[512];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", base, subname);
        char desc[256] = { 0 };
        char netCfg[256] = { 0 };
        char mac[32] = { 0 };
        if (!RegReadString(HKEY_LOCAL_MACHINE, path, "DriverDesc", desc, sizeof(desc))) continue;
        RegReadString(HKEY_LOCAL_MACHINE, path, "NetCfgInstanceId", netCfg, sizeof(netCfg));
        RegReadString(HKEY_LOCAL_MACHINE, path, "NetworkAddress", mac, sizeof(mac));

        AdapterInfo* a = &out[count];
        memset(a, 0, sizeof(*a));
        a->index = atoi(subname);
        strncpy(a->desc, desc, sizeof(a->desc) - 1);
        strncpy(a->netCfg, netCfg, sizeof(a->netCfg) - 1);
        strncpy(a->currentMac, mac, sizeof(a->currentMac) - 1);
        count++;
    }
    RegCloseKey(key);
    return count;
}

static int CommandSpoofMac(int argc, char** argv)
{
    const char* indexText = FindOption(argc, argv, "--index");
    const char* addrText  = FindOption(argc, argv, "--address");
    int randomMode = HasFlag(argc, argv, "--random");
    int allAdapters = HasFlag(argc, argv, "--all");

    AdapterInfo adapters[64];
    int n = EnumerateAdapters(adapters, 64);
    if (n == 0) {
        fprintf(stderr, "[-] no network adapters found\n");
        return 1;
    }

    /* select */
    int selected[64];
    int selCount = 0;
    if (allAdapters) {
        for (int i = 0; i < n; i++) selected[selCount++] = i;
    } else if (indexText != NULL) {
        int idx = atoi(indexText);
        for (int i = 0; i < n; i++) {
            if (adapters[i].index == idx) { selected[selCount++] = i; break; }
        }
        if (selCount == 0) {
            /* fallback: positional index into list */
            if (idx >= 0 && idx < n) selected[selCount++] = idx;
        }
    } else {
        /* first adapter */
        selected[selCount++] = 0;
    }

    if (selCount == 0) {
        fprintf(stderr, "[-] no adapter selected\n");
        return 1;
    }

    for (int s = 0; s < selCount; s++) {
        AdapterInfo* a = &adapters[selected[s]];
        char mac[32] = { 0 };
        if (addrText != NULL && !randomMode) {
            /* normalize */
            int mi = 0;
            for (const char* p = addrText; *p && mi < 12; p++) {
                if (isxdigit((unsigned char)*p)) mac[mi++] = (char)toupper((unsigned char)*p);
            }
            mac[mi] = '\0';
            if (mi != 12) {
                fprintf(stderr, "[-] MAC must contain 12 hex digits\n");
                return 2;
            }
        } else {
            MakeRandomMac(mac);
        }

        char path[512];
        _snprintf_s(path, sizeof(path), _TRUNCATE,
                    "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\%04d",
                    a->index);

        BackupValue(path, "NetworkAddress");

        if (!RegWriteString(HKEY_LOCAL_MACHINE, path, "NetworkAddress", mac)) {
            fprintf(stderr, "[-] failed to set MAC for adapter %04d\n", a->index);
            continue;
        }
        printf("[+] adapter %04d (%s): NetworkAddress = %s\n", a->index, a->desc, mac);
        printf("    restart the adapter (or reboot) for the change to take effect.\n");
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Spoof: registry identity                                                  */
/* ------------------------------------------------------------------------- */

static const char* K_CRYPTO = "SOFTWARE\\Microsoft\\Cryptography";
static const char* K_CV    = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
static const char* K_HWPROF= "SYSTEM\\CurrentControlSet\\Control\\IDConfigDB\\Hardware Profiles\\0001";
static const char* K_SQM   = "SOFTWARE\\Microsoft\\SQMClient";
static const char* K_TCP   = "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters";
static const char* K_COMP  = "SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ComputerName";
static const char* K_ACT   = "SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ActiveComputerName";

static int CommandSpoofGuid(int argc, char** argv)
{
    const char* valueText = FindOption(argc, argv, "--value");
    char guid[64] = { 0 };
    if (valueText != NULL) strncpy(guid, valueText, sizeof(guid) - 1);
    else MakeGuidString(guid, sizeof(guid));

    BackupValue(K_CRYPTO, "MachineGuid");
    if (!RegWriteString(HKEY_LOCAL_MACHINE, K_CRYPTO, "MachineGuid", guid)) {
        fprintf(stderr, "[-] Failed to set MachineGuid (run elevated).\n");
        return 1;
    }
    printf("[+] MachineGuid set to %s\n", guid);
    return 0;
}

static int CommandSpoofProductId(int argc, char** argv)
{
    const char* valueText = FindOption(argc, argv, "--value");
    char pid[64] = { 0 };
    if (valueText != NULL) strncpy(pid, valueText, sizeof(pid) - 1);
    else _snprintf_s(pid, sizeof(pid), _TRUNCATE, "%05d-%03d-%07d-%05d",
                     rand() % 99999, rand() % 999, rand() % 9999999, rand() % 99999);

    BackupValue(K_CV, "ProductId");
    if (!RegWriteString(HKEY_LOCAL_MACHINE, K_CV, "ProductId", pid)) {
        fprintf(stderr, "[-] Failed to set ProductId (run elevated).\n");
        return 1;
    }
    printf("[+] ProductId set to %s\n", pid);
    return 0;
}

static int CommandSpoofHostname(int argc, char** argv)
{
    const char* valueText = FindOption(argc, argv, "--value");
    if (valueText == NULL) {
        fprintf(stderr, "usage: FilterClient hostname --value <name>\n");
        return 2;
    }

    BackupValue(K_COMP, "ComputerName");
    BackupValue(K_ACT, "ComputerName");
    BackupValue(K_TCP, "Hostname");
    BackupValue(K_TCP, "NV Hostname");

    RegWriteString(HKEY_LOCAL_MACHINE, K_COMP, "ComputerName", valueText);
    RegWriteString(HKEY_LOCAL_MACHINE, K_ACT, "ComputerName", valueText);
    RegWriteString(HKEY_LOCAL_MACHINE, K_TCP, "Hostname", valueText);
    RegWriteString(HKEY_LOCAL_MACHINE, K_TCP, "NV Hostname", valueText);

    printf("[+] Hostname set to %s (reboot required)\n", valueText);
    return 0;
}

static int CommandSpoofHwProfile(int argc, char** argv)
{
    const char* valueText = FindOption(argc, argv, "--value");
    char g[64] = { 0 };
    if (valueText != NULL) strncpy(g, valueText, sizeof(g) - 1);
    else MakeGuidString(g, sizeof(g));

    BackupValue(K_HWPROF, "HwProfileGuid");
    /* HwProfileGuid is often stored as a GUID string with braces */
    char wrapped[80];
    if (g[0] != '{') _snprintf_s(wrapped, sizeof(wrapped), _TRUNCATE, "{%s}", g);
    else strncpy(wrapped, g, sizeof(wrapped) - 1);

    if (!RegWriteString(HKEY_LOCAL_MACHINE, K_HWPROF, "HwProfileGuid", wrapped)) {
        fprintf(stderr, "[-] Failed to set HwProfileGuid (run elevated).\n");
        return 1;
    }
    printf("[+] HwProfileGuid set to %s\n", wrapped);
    return 0;
}

static int CommandSpoofSqm(int argc, char** argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);
    char g[64] = { 0 };
    MakeGuidString(g, sizeof(g));
    char wrapped[80];
    _snprintf_s(wrapped, sizeof(wrapped), _TRUNCATE, "{%s}", g);

    BackupValue(K_SQM, "MachineId");
    if (!RegWriteString(HKEY_LOCAL_MACHINE, K_SQM, "MachineId", wrapped)) {
        fprintf(stderr, "[-] Failed to set SQM MachineId (run elevated).\n");
        return 1;
    }
    printf("[+] SQM MachineId set to %s\n", wrapped);
    return 0;
}

static int CommandSpoofInstallDate(int argc, char** argv)
{
    const char* valueText = FindOption(argc, argv, "--value");
    DWORD val = 0;
    if (valueText != NULL) val = (DWORD)strtoul(valueText, NULL, 0);
    else {
        time_t now = time(NULL);
        val = (DWORD)(now - (rand() % (3 * 365 * 86400)));
    }

    BackupValue(K_CV, "InstallDate");
    if (!RegWriteDword(HKEY_LOCAL_MACHINE, K_CV, "InstallDate", val)) {
        fprintf(stderr, "[-] Failed to set InstallDate (run elevated).\n");
        return 1;
    }
    printf("[+] InstallDate set to %lu\n", (unsigned long)val);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Show identity                                                             */
/* ------------------------------------------------------------------------- */

static int CommandShow(void)
{
    printf("=== Current machine identity ===\n\n");

    /* Disks */
    printf("[disks]\n");
    for (int d = 0; d < 16; d++) {
        DiskInfo di;
        if (QueryDiskInfo(d, &di) && di.ok) PrintDiskInfo(&di);
    }

    /* Volumes */
    printf("\n[volumes]\n");
    for (char c = 'C'; c <= 'Z'; c++) {
        char root[8] = { c, ':', '\\', '\0' };
        char label[256] = { 0 }, fs[64] = { 0 };
        DWORD serial = 0, maxcomp = 0, flags = 0;
        if (GetVolumeInformationA(root, label, sizeof(label), &serial, &maxcomp, &flags, fs, sizeof(fs))) {
            printf("  %c: serial=%08lX label=\"%s\" fs=%s\n", c, (unsigned long)serial, label, fs);
        }
    }

    /* MACs */
    printf("\n[adapters]\n");
    AdapterInfo adapters[64];
    int n = EnumerateAdapters(adapters, 64);
    for (int i = 0; i < n; i++) {
        printf("  %04d  %s  mac=%s  cfg=%s\n",
               adapters[i].index, adapters[i].desc,
               adapters[i].currentMac[0] ? adapters[i].currentMac : "(hardware default)",
               adapters[i].netCfg);
    }

    /* Registry identity */
    printf("\n[registry]\n");
    char buf[256] = { 0 };
    if (RegReadString(HKEY_LOCAL_MACHINE, K_CRYPTO, "MachineGuid", buf, sizeof(buf))) printf("  MachineGuid   : %s\n", buf);
    memset(buf, 0, sizeof(buf));
    if (RegReadString(HKEY_LOCAL_MACHINE, K_CV, "ProductId", buf, sizeof(buf))) printf("  ProductId     : %s\n", buf);
    memset(buf, 0, sizeof(buf));
    if (RegReadString(HKEY_LOCAL_MACHINE, K_HWPROF, "HwProfileGuid", buf, sizeof(buf))) printf("  HwProfileGuid : %s\n", buf);
    memset(buf, 0, sizeof(buf));
    if (RegReadString(HKEY_LOCAL_MACHINE, K_SQM, "MachineId", buf, sizeof(buf))) printf("  SQM MachineId : %s\n", buf);
    DWORD inst = 0; BOOL ex = FALSE;
    if (RegReadDword(HKEY_LOCAL_MACHINE, K_CV, "InstallDate", &inst, &ex) && ex) printf("  InstallDate   : %lu\n", (unsigned long)inst);
    memset(buf, 0, sizeof(buf));
    if (RegReadString(HKEY_LOCAL_MACHINE, K_COMP, "ComputerName", buf, sizeof(buf))) printf("  ComputerName  : %s\n", buf);
    memset(buf, 0, sizeof(buf));
    if (RegReadString(HKEY_LOCAL_MACHINE, K_TCP, "Hostname", buf, sizeof(buf))) printf("  TCP Hostname  : %s\n", buf);

    return 0;
}

/* ------------------------------------------------------------------------- */
/* WMI helpers                                                               */
/* ------------------------------------------------------------------------- */

#pragma comment(lib, "wbemuuid.lib")

#include <wbemidl.h>
#include <comdef.h>

static void WmiInit(void)
{
    CoInitializeEx(0, COINIT_MULTITHREADED);
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
}

static BOOL WmiQueryFirst(const char* wql, const char* property, char* out, size_t outlen)
{
    out[0] = '\0';
    IWbemLocator* loc = NULL;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator, (LPVOID*)&loc);
    if (FAILED(hr) || !loc) return FALSE;

    IWbemServices* svc = NULL;
    BSTR ns = SysAllocString(L"ROOT\\CIMV2");
    hr = loc->ConnectServer(ns, NULL, NULL, NULL, 0, NULL, NULL, &svc);
    SysFreeString(ns);
    if (FAILED(hr) || !svc) { loc->Release(); return FALSE; }

    CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

    /* convert wql to BSTR */
    int wlen = MultiByteToWideChar(CP_ACP, 0, wql, -1, NULL, 0);
    wchar_t* wbuf = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_ACP, 0, wql, -1, wbuf, wlen);
    BSTR query = SysAllocString(wbuf);
    free(wbuf);

    IEnumWbemClassObject* en = NULL;
    hr = svc->ExecQuery(BSTR(L"WQL"), query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                        NULL, &en);
    SysFreeString(query);
    if (FAILED(hr) || !en) { if (svc) svc->Release(); loc->Release(); return FALSE; }

    IWbemClassObject* obj = NULL;
    ULONG ret = 0;
    if (en->Next(WBEM_INFINITE, 1, &obj, &ret) == S_OK && ret > 0) {
        /* property name to BSTR */
        int plen = MultiByteToWideChar(CP_ACP, 0, property, -1, NULL, 0);
        wchar_t* pbuf = (wchar_t*)malloc(plen * sizeof(wchar_t));
        MultiByteToWideChar(CP_ACP, 0, property, -1, pbuf, plen);
        BSTR pname = SysAllocString(pbuf);
        free(pbuf);

        VARIANT v;
        VariantInit(&v);
        CIMTYPE ct;
        if (SUCCEEDED(obj->Get(pname, 0, &v, &ct, NULL))) {
            if (v.vt == VT_BSTR && v.bstrVal) {
                WideCharToMultiByte(CP_ACP, 0, v.bstrVal, -1, out, (int)outlen, NULL, NULL);
            } else if (v.vt == VT_I4 || v.vt == VT_UI4) {
                _snprintf_s(out, outlen, _TRUNCATE, "%ld", v.lVal);
            }
        }
        VariantClear(&v);
        SysFreeString(pname);
        obj->Release();
    }
    en->Release();
    svc->Release();
    loc->Release();
    return out[0] != '\0';
}

static int CommandWmiShow(void)
{
    WmiInit();
    printf("=== WMI identity ===\n");

    char buf[512] = { 0 };
    if (WmiQueryFirst("SELECT UUID FROM Win32_ComputerSystemProduct", "UUID", buf, sizeof(buf)))
        printf("  ComputerSystemProduct.UUID : %s\n", buf);

    memset(buf, 0, sizeof(buf));
    if (WmiQueryFirst("SELECT IdentifyingNumber FROM Win32_ComputerSystemProduct", "IdentifyingNumber", buf, sizeof(buf)))
        printf("  IdentifyingNumber          : %s\n", buf);

    memset(buf, 0, sizeof(buf));
    if (WmiQueryFirst("SELECT Name FROM Win32_ComputerSystemProduct", "Name", buf, sizeof(buf)))
        printf("  Product Name               : %s\n", buf);

    memset(buf, 0, sizeof(buf));
    if (WmiQueryFirst("SELECT Vendor FROM Win32_ComputerSystemProduct", "Vendor", buf, sizeof(buf)))
        printf("  Vendor                     : %s\n", buf);

    memset(buf, 0, sizeof(buf));
    if (WmiQueryFirst("SELECT SerialNumber FROM Win32_BIOS", "SerialNumber", buf, sizeof(buf)))
        printf("  BIOS SerialNumber          : %s\n", buf);

    memset(buf, 0, sizeof(buf));
    if (WmiQueryFirst("SELECT SerialNumber FROM Win32_BaseBoard", "SerialNumber", buf, sizeof(buf)))
        printf("  BaseBoard SerialNumber     : %s\n", buf);

    memset(buf, 0, sizeof(buf));
    if (WmiQueryFirst("SELECT Product FROM Win32_BaseBoard", "Product", buf, sizeof(buf)))
        printf("  BaseBoard Product          : %s\n", buf);

    memset(buf, 0, sizeof(buf));
    if (WmiQueryFirst("SELECT SerialNumber FROM Win32_DiskDrive", "SerialNumber", buf, sizeof(buf)))
        printf("  Disk SerialNumber          : %s\n", buf);

    memset(buf, 0, sizeof(buf));
    if (WmiQueryFirst("SELECT MACAddress FROM Win32_NetworkAdapter WHERE MACAddress IS NOT NULL", "MACAddress", buf, sizeof(buf)))
        printf("  NetworkAdapter MAC         : %s\n", buf);

    printf("\n  Note: hardware values above come from firmware/drivers; WMI repository\n");
    printf("  editing only affects cached/stored instances.\n");
    return 0;
}

static void WmiStopService(void)
{
    system("net stop winmgmt /y >nul 2>&1");
    Sleep(2000);
}

static void WmiStartService(void)
{
    system("net start winmgmt >nul 2>&1");
    Sleep(2000);
}

static void BackupDirRecursive(const char* src, const char* dst)
{
    CreateDirectoryA(dst, NULL);
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", src);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char s[MAX_PATH], d[MAX_PATH];
        _snprintf_s(s, sizeof(s), _TRUNCATE, "%s\\%s", src, fd.cFileName);
        _snprintf_s(d, sizeof(d), _TRUNCATE, "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            BackupDirRecursive(s, d);
        } else {
            CopyFileA(s, d, FALSE);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int CommandWmiBackup(void)
{
    BackupEnsureDir();
    char dst[MAX_PATH];
    _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\wbem-repository-backup", BACKUP_DIR);
    BackupDirRecursive("C:\\Windows\\System32\\wbem\\Repository", dst);
    printf("[+] WMI repository backed up to %s\n", dst);
    return 0;
}

static void* memmem(const void* haystack, size_t haystacklen, const void* needle, size_t needlelen);

static int ReplaceInFile(const char* path, const char* from, const char* to)
{
    size_t flen = strlen(from);
    size_t tlen = strlen(to);
    if (flen == 0 || flen != tlen) {
        /* require same length to avoid corrupting the DB */
        return 0;
    }

    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(h); return 0; }

    char* buf = (char*)malloc(size);
    if (!buf) { CloseHandle(h); return 0; }

    DWORD read = 0;
    if (!ReadFile(h, buf, size, &read, NULL) || read != size) {
        free(buf); CloseHandle(h); return 0;
    }

    int count = 0;
    char* p = buf;
    while ((p = (char*)memmem(p, size - (p - buf), from, flen)) != NULL) {
        memcpy(p, to, flen);
        p += flen;
        count++;
    }

    if (count > 0) {
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        DWORD written = 0;
        WriteFile(h, buf, size, &written, NULL);
        FlushFileBuffers(h);
    }

    free(buf);
    CloseHandle(h);
    return count;
}

/* memmem polyfill for MSVC */
static void* memmem(const void* haystack, size_t haystacklen, const void* needle, size_t needlelen)
{
    if (needlelen == 0) return (void*)haystack;
    if (haystacklen < needlelen) return NULL;
    const char* h = (const char*)haystack;
    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        if (memcmp(h + i, needle, needlelen) == 0) return (void*)(h + i);
    }
    return NULL;
}

static int CommandWmiSpoof(int argc, char** argv)
{
    const char* fromText = FindOption(argc, argv, "--from");
    const char* toText   = FindOption(argc, argv, "--to");
    int doReset = HasFlag(argc, argv, "--reset-repository");

    if (doReset) {
        WmiStopService();
        system("winmgmt /resetrepository >nul 2>&1");
        Sleep(2000);
        WmiStartService();
        printf("[+] WMI repository reset.\n");
        return 0;
    }

    if (fromText == NULL || toText == NULL) {
        fprintf(stderr, "usage: FilterClient wmi spoof --from <old> --to <new> [--reset-repository]\n");
        fprintf(stderr, "       (strings must have equal length)\n");
        return 2;
    }

    if (strlen(fromText) != strlen(toText)) {
        fprintf(stderr, "[-] --from and --to must have the same length (binary DB safety).\n");
        return 2;
    }

    CommandWmiBackup();

    WmiStopService();

    char files[][MAX_PATH] = {
        "C:\\Windows\\System32\\wbem\\Repository\\OBJECTS.DATA",
        "C:\\Windows\\System32\\wbem\\Repository\\INDEX.BTR",
        "C:\\Windows\\System32\\wbem\\Repository\\MAPPING1.MAP",
        "C:\\Windows\\System32\\wbem\\Repository\\MAPPING2.MAP",
        "C:\\Windows\\System32\\wbem\\Repository\\MAPPING3.MAP",
    };

    int total = 0;
    for (int i = 0; i < 5; i++) {
        int c = ReplaceInFile(files[i], fromText, toText);
        if (c > 0) {
            printf("[+] %s : %d replacement(s)\n", files[i], c);
            total += c;
        }
    }

    WmiStartService();

    printf("\n    total replacements : %d\n", total);
    printf("    reboot recommended.\n");
    return (total > 0) ? 0 : 1;
}

static int CommandWmi(int argc, char** argv)
{
    if (argc < 3 || argv[2][0] == '-') {
        /* default show */
        return CommandWmiShow();
    }
    const char* sub = argv[2];
    if (_stricmp(sub, "show") == 0) return CommandWmiShow();
    if (_stricmp(sub, "backup") == 0) return CommandWmiBackup();
    if (_stricmp(sub, "spoof") == 0) return CommandWmiSpoof(argc - 1, argv + 1);
    fprintf(stderr, "[-] unknown wmi subcommand: %s\n", sub);
    return 2;
}

static int CommandSpoofAll(void)
{
    printf("=== HWID spoof (volatile disk + registry) ===\n\n");

    srand((unsigned int)GetTickCount());

    /* Disk: all drives, randomize all fields */
    {
        char* argvv[] = { (char*)"x", (char*)"disk", (char*)"--all-drives", (char*)"--all-fields", (char*)"--random" };
        CommandSpoofDisk(5, argvv);
    }
    printf("\n");

    /* MAC: all adapters, random */
    {
        char* argvv[] = { (char*)"x", (char*)"mac", (char*)"--all", (char*)"--random" };
        CommandSpoofMac(4, argvv);
    }
    printf("\n");

    /* Registry */
    {
        char* argvv[] = { (char*)"x", (char*)"guid" };
        CommandSpoofGuid(2, argvv);
    }
    {
        char* argvv[] = { (char*)"x", (char*)"productid" };
        CommandSpoofProductId(2, argvv);
    }
    {
        char hname[32];
        _snprintf_s(hname, sizeof(hname), _TRUNCATE, "PC-%04X", rand() & 0xFFFF);
        char* argvv[] = { (char*)"x", (char*)"hostname", (char*)"--value", hname };
        CommandSpoofHostname(4, argvv);
    }
    {
        char* argvv[] = { (char*)"x", (char*)"hwprofile" };
        CommandSpoofHwProfile(2, argvv);
    }
    {
        char* argvv[] = { (char*)"x", (char*)"sqm" };
        CommandSpoofSqm(2, argvv);
    }
    {
        char* argvv[] = { (char*)"x", (char*)"installdate" };
        CommandSpoofInstallDate(2, argvv);
    }

    printf("\n[+] all steps done.\n");
    printf("    Disk changes are live now (filter armed). Registry/MAC changes require reboot.\n");
    printf("    Run 'restore' to revert registry changes and clear the filter.\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Restore                                                                   */
/* ------------------------------------------------------------------------- */

static int CommandRestore(void)
{
    CommandClear();
    RestoreBackup();
    printf("[+] restore complete. Reboot to fully revert.\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Simulate                                                                  */
/* ------------------------------------------------------------------------- */

static ULONG SimulateRedact(BYTE* buffer, size_t length, const TEMPMOD_POLICY& policy)
{
    ULONG total = 0;
    if (!policy.Enabled) return 0;

    for (ULONG r = 0; r < policy.RuleCount && r < TEMPMOD_MAX_RULES; r++) {
        const TEMPMOD_RULE& rule = policy.Rules[r];
        size_t plen = rule.PatternLength;
        if (plen == 0 || plen > TEMPMOD_PATTERN_MAX || plen > length) continue;

        size_t replLen = rule.ReplacementLength;
        if (replLen == 0) replLen = plen;
        if (replLen > plen) replLen = plen;

        ULONG limit = policy.MaxReplacementsPerRequest;
        if (limit == 0) limit = TEMPMOD_DEFAULT_MAX_REPLACEMENTS;

        ULONG replacements = 0;
        for (size_t index = 0; index + plen <= length && replacements < limit; index++) {
            if (buffer[index] != rule.Pattern[0]) continue;
            if (memcmp(buffer + index, rule.Pattern, plen) != 0) continue;
            for (size_t k = 0; k < plen; k++) {
                if (k < replLen) buffer[index + k] = rule.Replacement[k];
                else buffer[index + k] = ' ';
            }
            replacements++;
            index += (plen - 1);
            total++;
        }
    }
    return total;
}

static int CommandSimulate(int argc, char** argv)
{
    const char* patternText      = FindOption(argc, argv, "--pattern");
    const char* replaceText      = FindOption(argc, argv, "--replace");
    const char* replacementText  = FindOption(argc, argv, "--replacement");
    const char* maxText          = FindOption(argc, argv, "--max");
    const char* sampleText       = FindOption(argc, argv, "--sample");

    TEMPMOD_POLICY policy;
    memset(&policy, 0, sizeof(policy));
    policy.Version = TEMPMOD_POLICY_VERSION;
    policy.Enabled = 1;
    policy.RuleCount = 1;

    TEMPMOD_RULE& rule = policy.Rules[0];
    memset(&rule, 0, sizeof(rule));

    if (replacementText != NULL) {
        size_t rlen = strlen(replacementText);
        if (rlen > TEMPMOD_PATTERN_MAX) rlen = TEMPMOD_PATTERN_MAX;
        memcpy(rule.Replacement, replacementText, rlen);
        rule.ReplacementLength = (ULONG)rlen;
    } else if (replaceText != NULL) {
        UCHAR byte = (UCHAR)strtoul(replaceText, NULL, 0);
        for (ULONG i = 0; i < TEMPMOD_PATTERN_MAX; i++) rule.Replacement[i] = byte;
        rule.ReplacementLength = 0;
    } else {
        for (ULONG i = 0; i < TEMPMOD_PATTERN_MAX; i++) rule.Replacement[i] = (UCHAR)'*';
        rule.ReplacementLength = 0;
    }

    policy.MaxReplacementsPerRequest = (maxText != NULL) ? (ULONG)strtoul(maxText, NULL, 0) : 0;

    if (!ParsePattern((patternText != NULL) ? patternText : "WD-WCC4E1234567", rule)) return 2;

    if (rule.ReplacementLength > rule.PatternLength) rule.ReplacementLength = rule.PatternLength;

    const char* sample = (sampleText != NULL) ? sampleText :
        "serial: WD-WCC4E1234567\r\n"
        "model : ST2000DM001\r\n"
        "serial: WD-WCC4E1234567\r\n"
        "flag  : keep-me-intact\r\n";

    size_t length = strlen(sample);
    if (length == 0) { fprintf(stderr, "[-] empty sample\n"); return 2; }

    BYTE* buffer = (BYTE*)malloc(length);
    if (buffer == NULL) { fprintf(stderr, "[-] out of memory\n"); return 1; }
    memcpy(buffer, sample, length);

    printf("[*] user-mode simulation - no driver, no disk access\n\n");
    PrintPolicy(policy);
    printf("\n");

    /* dump before */
    printf("  before\n");
    for (size_t off = 0; off < length; off += 16) {
        printf("    %04zX  ", off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < length) printf("%02X ", buffer[off + i]);
            else printf("   ");
        }
        printf(" |");
        for (size_t i = 0; i < 16 && off + i < length; i++) {
            BYTE c = buffer[off + i];
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("|\n");
    }

    ULONG replacements = SimulateRedact(buffer, length, policy);

    printf("\n  after\n");
    for (size_t off = 0; off < length; off += 16) {
        printf("    %04zX  ", off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < length) printf("%02X ", buffer[off + i]);
            else printf("   ");
        }
        printf(" |");
        for (size_t i = 0; i < 16 && off + i < length; i++) {
            BYTE c = buffer[off + i];
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("|\n");
    }

    printf("\n    replacements   : %lu\n", (unsigned long)replacements);
    printf("    scope          : only the copy above, nothing was written\n");

    free(buffer);
    return (replacements > 0) ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* Usage                                                                     */
/* ------------------------------------------------------------------------- */

static int CommandWmiBackup(int argc, char** argv)
{
    const char* dirText = FindOption(argc, argv, "--dir");
    char dir[MAX_PATH];
    if (dirText != NULL) {
        strncpy(dir, dirText, sizeof(dir) - 1);
    } else {
        GetTempPathA(MAX_PATH, dir);
        strcat_s(dir, MAX_PATH, "wmi_repository_backup");
    }

    char cmd[1024];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                "robocopy \"C:\\Windows\\System32\\wbem\\Repository\" \"%s\" /E /COPY:DAT /R:1 /W:1 >nul 2>&1",
                dir);
    printf("[*] backing up WMI repository to %s ...\n", dir);
    int rc = system(cmd);
    if (rc > 7) {
        fprintf(stderr, "[-] backup failed (robocopy rc=%d)\n", rc);
        fprintf(stderr, "    Run from an elevated console.\n");
        return 1;
    }
    printf("[+] backup complete: %s\n", dir);
    return 0;
}

static int CommandWmiRestore(int argc, char** argv)
{
    const char* dirText = FindOption(argc, argv, "--dir");
    char dir[MAX_PATH];
    if (dirText != NULL) {
        strncpy(dir, dirText, sizeof(dir) - 1);
    } else {
        GetTempPathA(MAX_PATH, dir);
        strcat_s(dir, MAX_PATH, "wmi_repository_backup");
    }

    printf("[*] stopping WMI service...\n");
    system("net stop winmgmt /y >nul 2>&1");

    char cmd[1024];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                "robocopy \"%s\" \"C:\\Windows\\System32\\wbem\\Repository\" /E /COPY:DAT /R:1 /W:1 >nul 2>&1",
                dir);
    printf("[*] restoring WMI repository from %s ...\n", dir);
    int rc = system(cmd);
    if (rc > 7) {
        fprintf(stderr, "[-] restore failed (robocopy rc=%d)\n", rc);
    }

    printf("[*] starting WMI service...\n");
    system("net start winmgmt >nul 2>&1");
    printf("[+] restore complete. Reboot recommended.\n");
    return 0;
}

static int CommandWmiSwap(int argc, char** argv)
{
    const char* dirText = FindOption(argc, argv, "--dir");
    if (dirText == NULL) {
        fprintf(stderr, "usage: FilterClient wmi-swap --dir <repository_dir>\n");
        return 2;
    }

    char backup[MAX_PATH];
    GetTempPathA(MAX_PATH, backup);
    strcat_s(backup, MAX_PATH, "wmi_repository_backup_preswap");
    char bcmd[1024];
    _snprintf_s(bcmd, sizeof(bcmd), _TRUNCATE,
                "robocopy \"C:\\Windows\\System32\\wbem\\Repository\" \"%s\" /E /COPY:DAT /R:1 /W:1 >nul 2>&1",
                backup);
    system(bcmd);

    printf("[*] stopping WMI service...\n");
    system("net stop winmgmt /y >nul 2>&1");

    char cmd[1024];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                "robocopy \"%s\" \"C:\\Windows\\System32\\wbem\\Repository\" /E /COPY:DAT /R:1 /W:1 >nul 2>&1",
                dirText);
    printf("[*] swapping WMI repository from %s ...\n", dirText);
    int rc = system(cmd);
    if (rc > 7) {
        fprintf(stderr, "[-] swap failed (robocopy rc=%d)\n", rc);
    }

    printf("[*] starting WMI service...\n");
    system("net start winmgmt >nul 2>&1");
    printf("[+] swap complete. Reboot recommended. Previous repo backed up to %s\n", backup);
    return 0;
}

static int CommandWmiQuery(int argc, char** argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[-] CoInitializeEx failed 0x%08X\n", (unsigned)hr);
        return 1;
    }

    IWbemLocator* pLoc = NULL;
    hr = CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, (LPVOID*)&pLoc);
    if (FAILED(hr)) {
        fprintf(stderr, "[-] CoCreateInstance failed 0x%08X\n", (unsigned)hr);
        CoUninitialize();
        return 1;
    }

    IWbemServices* pSvc = NULL;
    hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, NULL, 0, NULL, NULL, &pSvc);
    if (FAILED(hr)) {
        fprintf(stderr, "[-] ConnectServer failed 0x%08X\n", (unsigned)hr);
        pLoc->Release();
        CoUninitialize();
        return 1;
    }

    CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

    const wchar_t* queries[] = {
        L"SELECT Name FROM Win32_Processor",
        L"SELECT SerialNumber FROM Win32_BIOS",
        L"SELECT SerialNumber,Model FROM Win32_DiskDrive",
        L"SELECT UUID FROM Win32_ComputerSystemProduct",
    };
    const char* labels[] = { "CPU", "BIOS", "Disk", "UUID" };

    for (int q = 0; q < 4; q++) {
        IEnumWbemClassObject* pEnum = NULL;
        hr = pSvc->ExecQuery(_bstr_t(L"WQL"), _bstr_t(queries[q]),
                             WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                             NULL, &pEnum);
        if (FAILED(hr)) {
            printf("[*] %s query failed 0x%08X\n", labels[q], (unsigned)hr);
            continue;
        }
        IWbemClassObject* pObj = NULL;
        ULONG ret = 0;
        while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret) == S_OK && ret > 0) {
            VARIANT v;
            VariantInit(&v);
            const wchar_t* props[] = { L"Name", L"SerialNumber", L"Model", L"UUID" };
            for (int p = 0; p < 4; p++) {
                VariantClear(&v);
                hr = pObj->Get(props[p], 0, &v, NULL, NULL);
                if (SUCCEEDED(hr) && v.vt == VT_BSTR && v.bstrVal && wcslen(v.bstrVal) > 0) {
                    printf("[*] %s %ls : %ls\n", labels[q], props[p], v.bstrVal);
                }
            }
            VariantClear(&v);
            pObj->Release();
        }
        pEnum->Release();
    }

    pSvc->Release();
    pLoc->Release();
    CoUninitialize();
    return 0;
}

static int CommandWmiReset(int argc, char** argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);
    printf("[*] resetting WMI repository (may take a while)...\n");
    int rc = system("winmgmt /resetrepository");
    if (rc != 0) {
        fprintf(stderr, "[-] winmgmt /resetrepository failed (%d)\n", rc);
        fprintf(stderr, "    Run from an elevated console.\n");
        return 1;
    }
    printf("[+] WMI repository reset. Reboot recommended.\n");
    return 0;
}

static void Usage(void)
{
    printf(
        "HWID Spoofer client (TempModFilter)\n"
        "\n"
        "  status                      show the armed policy\n"
        "  stats [--reset]             show (or reset) counters\n"
        "  set --pattern <p> [opts]    arm a single redaction rule\n"
        "        --replace 0x2A        byte written over each match ('*')\n"
        "        --replacement <s>     replacement string\n"
        "        --ioctl   0x2D1400    target IOCTL, 0 = all\n"
        "        --max     8           replacements per request\n"
        "  clear                       disarm (pass-through)\n"
        "  demo [--drive 0]            before/after round-trip on a disk\n"
        "  show                        print current machine identity\n"
        "  spoof disk ...             spoof disk identity in RAM (volatile)\n"
        "        [--drive 0] [--all-drives] [--random]\n"
        "        [--serial <s>] [--model <m>] [--firmware <f>] [--vendor <v>]\n"
        "        [--all-fields]\n"
        "  spoof mac ...              set adapter MAC via registry\n"
        "        [--address <mac>] [--index N|--all] [--random]\n"
        "  spoof guid [--value <g>]    set MachineGuid (random if omitted)\n"
        "  spoof productid [--value <p>]  set Windows ProductId\n"
        "  spoof hostname --value <n>  set computer hostname\n"
        "  spoof hwprofile [--value <g>]  set HwProfileGuid\n"
        "  spoof sqm                   set SQM MachineId\n"
        "  spoof installdate [--value <n>]  set InstallDate\n"
        "  spoof all                  run every spoof step (random)\n"
        "  restore                    clear filter + restore registry backup\n"
        "  simulate ...               run the redaction algorithm in user mode\n"
        "  wmi show                    query WMI for HWID values\n"
        "  wmi backup                  backup WMI repository\n"
        "  wmi spoof --from <a> --to <b>  binary replace in repository (equal length)\n"
        "  wmi spoof --reset-repository  reset WMI repository\n"
        "  help                        this text\n"
        "\n"
        "  pattern syntax:  \"ABC123\"  or  \"hex:41 42 43\"\n");
}

/* ------------------------------------------------------------------------- */

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    srand((unsigned int)GetTickCount());

    if (argc < 2) {
        Usage();
        return 2;
    }

    const char* command = argv[1];

    /* spoof subcommand dispatch */
    if (_stricmp(command, "spoof") == 0) {
        if (argc >= 3 && argv[2][0] != '-') {
            const char* sub = argv[2];
            /* shift argv so subcommand handlers see their own args: we pass original argc/argv; handlers use FindOption starting at 2, but sub is at 2. Adjust by treating sub as command. Simpler: call with argc-1, argv+1. */
            if (_stricmp(sub, "disk") == 0) return CommandSpoofDisk(argc - 1, argv + 1);
            if (_stricmp(sub, "mac") == 0) return CommandSpoofMac(argc - 1, argv + 1);
            if (_stricmp(sub, "guid") == 0) return CommandSpoofGuid(argc - 1, argv + 1);
            if (_stricmp(sub, "productid") == 0) return CommandSpoofProductId(argc - 1, argv + 1);
            if (_stricmp(sub, "hostname") == 0) return CommandSpoofHostname(argc - 1, argv + 1);
            if (_stricmp(sub, "hwprofile") == 0) return CommandSpoofHwProfile(argc - 1, argv + 1);
            if (_stricmp(sub, "sqm") == 0) return CommandSpoofSqm(argc - 1, argv + 1);
            if (_stricmp(sub, "installdate") == 0) return CommandSpoofInstallDate(argc - 1, argv + 1);
            if (_stricmp(sub, "all") == 0) return CommandSpoofAll();
            fprintf(stderr, "[-] unknown spoof subcommand: %s\n\n", sub);
            Usage();
            return 2;
        }
        /* legacy: spoof --serial ... (disk) */
        return CommandSpoofDisk(argc, argv);
    }

    /* top-level legacy aliases */
    if (_stricmp(command, "set") == 0) return CommandSet(argc, argv);
    if (_stricmp(command, "clear") == 0) return CommandClear();
    if (_stricmp(command, "status") == 0 || _stricmp(command, "get") == 0) return CommandStatus();
    if (_stricmp(command, "stats") == 0) return CommandStats(HasFlag(argc, argv, "--reset"));
    if (_stricmp(command, "demo") == 0) return CommandDemo(argc, argv);
    if (_stricmp(command, "show") == 0) return CommandShow();
    if (_stricmp(command, "restore") == 0) return CommandRestore();
    if (_stricmp(command, "wmi-query") == 0) return CommandWmiQuery(argc, argv);
    if (_stricmp(command, "wmi-reset") == 0) return CommandWmiReset(argc, argv);
    if (_stricmp(command, "wmi-backup") == 0) return CommandWmiBackup(argc, argv);
    if (_stricmp(command, "wmi-restore") == 0) return CommandWmiRestore(argc, argv);
    if (_stricmp(command, "wmi-swap") == 0) return CommandWmiSwap(argc, argv);    if (_stricmp(command, "mac") == 0) return CommandSpoofMac(argc, argv);
    if (_stricmp(command, "guid") == 0) return CommandSpoofGuid(argc, argv);
    if (_stricmp(command, "productid") == 0) return CommandSpoofProductId(argc, argv);
    if (_stricmp(command, "hostname") == 0) return CommandSpoofHostname(argc, argv);
    if (_stricmp(command, "hwprofile") == 0) return CommandSpoofHwProfile(argc, argv);
    if (_stricmp(command, "sqm") == 0) return CommandSpoofSqm(argc, argv);
    if (_stricmp(command, "installdate") == 0) return CommandSpoofInstallDate(argc, argv);
    if (_stricmp(command, "all") == 0) return CommandSpoofAll();
    if (_stricmp(command, "simulate") == 0) return CommandSimulate(argc, argv);
    /* forward decl */
    /* handled below */
    if (_stricmp(command, "wmi") == 0) return CommandWmi(argc, argv);

    fprintf(stderr, "[-] unknown command: %s\n\n", command);
    Usage();
    return 2;
}