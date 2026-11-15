/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "compat.h"
#include "winfile.h"
#include "file_share_cleanup.h"
#include "sync.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void require(bool passed, const char *expression, int line)
{
    if (!passed) {
        fprintf(stderr, "FAIL line %d %s\n", line, expression);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) require((expression), #expression, __LINE__)
#define KERNEL "KERNEL32.dll"
#define USER "USER32.dll"
#define GDI "GDI32.dll"
#define CRT(group) "api-ms-win-crt-" group "-l1-1-0.dll"
#define BOUND(module, name) CHECK(nt_runtime_resolve(module, #name) == (nt_guest_proc)nt_##name)
#define CRT_BOUND(group, name, function) \
    CHECK(nt_runtime_resolve(CRT(group), name) == (nt_guest_proc)function)

static uint32_t last_error(void)
{
    typedef uint32_t (NT_MS_ABI *get_error)(void);
    return ((get_error)nt_runtime_resolve(KERNEL, "GetLastError"))();
}

static struct timespec realtime = {0, 123456789};
static struct timespec boottime = {10, 123400000};
static bool clock_fails;

/* Freeze one real worker pread so queued cancellation can be tested without
 * relying on the OS scheduler or on an artificial delay. */
static pthread_mutex_t async_test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t async_test_changed = PTHREAD_COND_INITIALIZER;
static bool async_hold_enabled, async_hold_entered, async_hold_release;

ssize_t __real_pread(int fd, void *buffer, size_t count, off_t offset);

ssize_t __wrap_pread(int fd, void *buffer, size_t count, off_t offset)
{
    pthread_mutex_lock(&async_test_lock);
    if (async_hold_enabled && offset == 4096) {
        async_hold_entered = true;
        pthread_cond_broadcast(&async_test_changed);
        while (!async_hold_release)
            pthread_cond_wait(&async_test_changed, &async_test_lock);
    }
    pthread_mutex_unlock(&async_test_lock);
    return __real_pread(fd, buffer, count, offset);
}

int __wrap_clock_gettime(clockid_t clock, struct timespec *value)
{
    if (clock_fails) {
        errno = EIO;
        return -1;
    }
    *value = clock == CLOCK_REALTIME ? realtime : boottime;
    return 0;
}

static void check_system(void)
{
    BOUND(KERNEL, GetCurrentProcess);
    BOUND(KERNEL, GetLocalTime);
    BOUND(KERNEL, GetSystemTimeAsFileTime);
    BOUND(KERNEL, GetTickCount);
    BOUND(KERNEL, GetStartupInfoW);
    BOUND(KERNEL, InitializeSListHead);
    BOUND(KERNEL, IsDebuggerPresent);
    BOUND(KERNEL, IsProcessorFeaturePresent);

    CHECK(nt_GetCurrentProcess() == UINTPTR_MAX);
    CHECK(nt_GetTickCount() == 10123);
    boottime = (struct timespec){4294967, 296000000};
    CHECK(nt_GetTickCount() == 0);

    /* This Win32 API leaves the output unchanged when CLOCK_REALTIME fails.
     * Initialize it explicitly so GCC+LTO can prove every read is defined. */
    struct nt_file_time file_time = {0};
    nt_GetSystemTimeAsFileTime(&file_time);
    const uint64_t ticks = (uint64_t)file_time.high << 32 | file_time.low;
    CHECK(ticks == UINT64_C(116444736001234567));
    struct nt_system_time local = {0};
    struct tm expected;
    CHECK(localtime_r(&realtime.tv_sec, &expected) == &expected);
    nt_GetLocalTime(&local);
    CHECK(local.year == expected.tm_year + 1900 && local.month == expected.tm_mon + 1 &&
          local.day == expected.tm_mday && local.day_of_week == expected.tm_wday &&
          local.hour == expected.tm_hour && local.minute == expected.tm_min &&
          local.second == expected.tm_sec && local.milliseconds == 123);
    clock_fails = true;
    nt_GetSystemTimeAsFileTime(&file_time);
    CHECK(((uint64_t)file_time.high << 32 | file_time.low) == ticks);
    CHECK(last_error() == NT_ERROR_GEN_FAILURE);
    clock_fails = false;

    struct nt_startup_info_w startup;
    memset(&startup, 0xa5, sizeof(startup));
    nt_GetStartupInfoW(&startup);
    CHECK(startup.size == 104 && startup.flags == 0 &&
          startup.title == NULL && startup.output == 0);
    _Alignas(16) unsigned char list[32];
    memset(list, 0xa5, sizeof(list));
    nt_InitializeSListHead(list);
    CHECK(memcmp(list, (const unsigned char[16]){0}, 16) == 0 && list[16] == 0xa5);
    nt_InitializeSListHead(list + 1);
    CHECK(last_error() == NT_ERROR_INVALID_PARAMETER && list[16] == 0xa5);
    const int32_t debugged = nt_IsDebuggerPresent();
    CHECK(debugged == 0 || debugged == 1);
    CHECK(nt_IsProcessorFeaturePresent(10) == 1);
    CHECK(nt_IsProcessorFeaturePresent(UINT32_MAX) == 0);
    puts("PASS Win32 process, clock, startup and CPU contracts");
}

static void check_modules(void)
{
    BOUND(KERNEL, GetModuleHandleW);
    BOUND(KERNEL, GetProcAddress);
    BOUND(KERNEL, LoadLibraryA);
    BOUND(KERNEL, FreeLibrary);

    const uintptr_t first = nt_LoadLibraryA("KERNEL32");
    const uintptr_t second = nt_LoadLibraryA("kernel32.dll");
    const uint16_t wide[] = {'K','e','r','n','e','l','3','2','.','D','L','L',0};
    CHECK(first != 0 && first == second && first == nt_GetModuleHandleW(wide));
    CHECK(nt_GetProcAddress(first, "QueryPerformanceCounter") ==
          nt_runtime_resolve(KERNEL, "QueryPerformanceCounter"));
    CHECK(nt_GetProcAddress(first, "queryperformancecounter") == NULL &&
          last_error() == NT_ERROR_PROC_NOT_FOUND);
    CHECK(nt_GetProcAddress(first, (const char *)(uintptr_t)17) == NULL &&
          last_error() == NT_ERROR_PROC_NOT_FOUND);
    CHECK(nt_GetProcAddress(0, "GetLastError") == NULL && last_error() == NT_ERROR_INVALID_HANDLE);
    CHECK(nt_FreeLibrary(first) != 0 && nt_FreeLibrary(second) != 0);
    CHECK(nt_FreeLibrary(second) == 0 && last_error() == NT_ERROR_INVALID_HANDLE);
    CHECK(nt_LoadLibraryA("C:\\host\\other.dll") == 0 && last_error() == NT_ERROR_MOD_NOT_FOUND);
    CHECK(nt_GetModuleHandleW(NULL) == 0 && last_error() == NT_ERROR_MOD_NOT_FOUND);
    puts("PASS built-in module names, references and missing procedure errors");
}

static void check_memory(void)
{
    BOUND(KERNEL, GlobalAlloc);
    BOUND(KERNEL, GlobalFree);
    BOUND(KERNEL, GlobalLock);
    BOUND(KERNEL, GlobalUnlock);
    BOUND(KERNEL, LocalFree);
    BOUND(KERNEL, FormatMessageA);

    const uintptr_t fixed = nt_GlobalAlloc(0x40, 16);
    CHECK(fixed != 0 && nt_GlobalLock(fixed) == (void *)fixed);
    CHECK(memcmp((const void *)fixed, (const unsigned char[16]){0}, 16) == 0);
    CHECK(nt_GlobalUnlock(fixed) == 1 && nt_GlobalFree(fixed) == 0);
    const uintptr_t movable = nt_GlobalAlloc(0x2042, 16);
    CHECK(movable != 0);
    void *bytes = nt_GlobalLock(movable);
    CHECK(bytes != NULL && (uintptr_t)bytes != movable && nt_GlobalLock(movable) == bytes);
    CHECK(nt_GlobalUnlock(movable) == 1);
    nt_SetLastError(55);
    CHECK(nt_GlobalUnlock(movable) == 0 && last_error() == NT_ERROR_SUCCESS);
    CHECK(nt_GlobalUnlock(movable) == 0 && last_error() == NT_ERROR_NOT_LOCKED);
    CHECK(nt_GlobalFree(movable) == 0 && nt_GlobalLock(movable) == NULL &&
          last_error() == NT_ERROR_INVALID_HANDLE);
    const uintptr_t discarded = nt_GlobalAlloc(2, 0);
    CHECK(discarded != 0 && nt_GlobalLock(discarded) == NULL && last_error() == NT_ERROR_DISCARDED);
    CHECK(nt_GlobalFree(discarded) == 0);
    CHECK(nt_GlobalAlloc(0x80000000u, 1) == 0 && last_error() == NT_ERROR_INVALID_PARAMETER);

    char small[] = "keep";
    CHECK(nt_FormatMessageA(0x1200, NULL, 87, 0, small, sizeof(small), NULL) == 0 &&
          last_error() == NT_ERROR_INSUFFICIENT_BUFFER && strcmp(small, "keep") == 0);
    char *message = NULL;
    CHECK(nt_FormatMessageA(0x1300, NULL, 87, 0x0409, (char *)&message, 0, NULL) ==
          strlen("The parameter is incorrect.\r\n"));
    CHECK(message != NULL && strcmp(message, "The parameter is incorrect.\r\n") == 0);
    CHECK(nt_GlobalFree((uintptr_t)message) == (uintptr_t)message &&
          last_error() == NT_ERROR_INVALID_HANDLE);
    CHECK(nt_LocalFree((uintptr_t)message) == 0);
    CHECK(nt_FormatMessageA(0x1000, NULL, UINT32_MAX, 0, small, sizeof(small), NULL) == 0 &&
          last_error() == NT_ERROR_MR_MID_NOT_FOUND);
    CHECK(nt_FormatMessageA(0x600, "ok", 0, 0, small, sizeof(small), NULL) == 2 &&
          strcmp(small, "ok") == 0);
    CHECK(nt_FormatMessageA(0x600, "%%", 0, 0, small, sizeof(small), NULL) == 0 &&
          last_error() == NT_ERROR_NOT_SUPPORTED && strcmp(small, "ok") == 0);
    puts("PASS global and local memory, lock counts and message buffer errors");
}

static void check_unicode_and_rects(void)
{
    BOUND(KERNEL, MultiByteToWideChar);
    BOUND(USER, SetRect);
    BOUND(USER, OffsetRect);
    BOUND(USER, IntersectRect);
    BOUND(USER, UnionRect);

    const char utf8[] = "A\xf0\x9f\x98\x80";
    uint16_t wide[4] = {0x1111, 0x2222, 0x3333, 0x4444};
    CHECK(nt_MultiByteToWideChar(65001, 8, utf8, -1, NULL, 0) == 4);
    CHECK(nt_MultiByteToWideChar(65001, 8, utf8, -1, wide, 3) == 0 &&
          last_error() == NT_ERROR_INSUFFICIENT_BUFFER && wide[0] == 0x1111);
    CHECK(nt_MultiByteToWideChar(65001, 8, utf8, -1, wide, 4) == 4);
    CHECK(wide[0] == 'A' && wide[1] == 0xd83d && wide[2] == 0xde00 && wide[3] == 0);
    CHECK(nt_MultiByteToWideChar(0, 0, "\x80", 1, wide, 4) == 1 && wide[0] == 0x20ac);
    CHECK(nt_MultiByteToWideChar(65001, 8, "\xf0\x28\x8c\x28", 4, wide, 4) == 0 &&
          last_error() == NT_ERROR_NO_UNICODE_TRANSLATION && wide[0] == 0x20ac);
    CHECK(nt_MultiByteToWideChar(65001, 0, "\xf0\x28\x8c\x28", 4, wide, 4) == 4);
    CHECK(wide[0] == 0xfffd && wide[1] == '(' && wide[2] == 0xfffd && wide[3] == '(');
    CHECK(nt_MultiByteToWideChar(65001, 1, utf8, -1, wide, 4) == 0 &&
          last_error() == NT_ERROR_INVALID_FLAGS);

    /* ICU-backed page conversion remains deterministic without the Linux
     * locale. Verify SBCS Cyrillic and two Windows DBCS families. */
    uint16_t international[16] = {0};
    const char russian_cp1251[] = "\xcf\xf0\xe8\xe2\xe5\xf2";
    CHECK(nt_IsValidCodePage(1251) && nt_IsValidCodePage(932) &&
          nt_IsValidCodePage(936) && nt_IsValidCodePage(850));
    CHECK(nt_MultiByteToWideChar(1251, 8, russian_cp1251, 6, international, 16) == 6 &&
          memcmp(international, u"Привет", 6 * sizeof(uint16_t)) == 0);
    char reverse_cp1251[32] = {0};
    CHECK(nt_WideCharToMultiByte(1251, 0, u"Привет", 6, reverse_cp1251,
                                 sizeof(reverse_cp1251), NULL, NULL) == 6 &&
          memcmp(reverse_cp1251, russian_cp1251, 6) == 0);
    CHECK(nt_MultiByteToWideChar(936, 8, "\xd6\xd0\xce\xc4", 4,
                                 international, 16) == 2 &&
          international[0] == 0x4e2d && international[1] == 0x6587);
    CHECK(nt_MultiByteToWideChar(932, 8, "\x82\xa0", 2,
                                 international, 16) == 1 && international[0] == 0x3042);
    CHECK(nt_MultiByteToWideChar(932, 8, "\x82", 1, international, 16) == 0 &&
          last_error() == NT_ERROR_NO_UNICODE_TRANSLATION);
    CHECK(nt_IsDBCSLeadByteEx(932, 0x82) && !nt_IsDBCSLeadByteEx(932, 0x40));
    CHECK(nt_IsDBCSLeadByteEx(936, 0x81) && !nt_IsDBCSLeadByteEx(1251, 0x81));

    struct nt_rect first, second, result;
    CHECK(nt_SetRect(&first, -2, 1, 8, 9) && nt_SetRect(&second, 4, 2, 10, 7));
    CHECK(nt_IntersectRect(&first, &first, &second) && first.left == 4 && first.right == 8 &&
          first.top == 2 && first.bottom == 7);
    CHECK(nt_OffsetRect(&second, -6, 8) && second.left == -2 && second.top == 10);
    CHECK(nt_IntersectRect(&result, &first, &second) == 0 &&
          result.left == 0 && result.top == 0 && result.right == 0 && result.bottom == 0);
    CHECK(nt_UnionRect(&second, &first, &second) && second.left == -2 &&
          second.top == 2 && second.right == 8 && second.bottom == 15);
    CHECK(nt_UnionRect(&result, &result, &result) == 0);
    puts("PASS UTF-16 sizing, replacement, strict errors and aliased rectangles");
}

static unsigned int callbacks;
static void NT_MS_ABI append_one(void) { callbacks = callbacks * 10 + 1; }
static void NT_MS_ABI append_two(void) { callbacks = callbacks * 10 + 2; }
static int NT_MS_ABI init_one(void) { append_one(); return 0; }
static int NT_MS_ABI init_fail(void) { append_two(); return 23; }

static void check_crt(void)
{
    CRT_BOUND("convert", "atoi", nt_atoi);
    CRT_BOUND("convert", "atof", nt_atof);
    CRT_BOUND("string", "isspace", nt_isspace);
    CRT_BOUND("string", "tolower", nt_tolower);
    CRT_BOUND("string", "toupper", nt_toupper);
    CRT_BOUND("locale", "_configthreadlocale", nt_configthreadlocale);
    CRT_BOUND("heap", "_set_new_mode", nt_set_new_mode);
    CRT_BOUND("environment", "getenv", nt_getenv);
    CRT_BOUND("runtime", "_initterm", nt_initterm);
    CRT_BOUND("runtime", "_initterm_e", nt_initterm_e);
    CRT_BOUND("runtime", "_initialize_onexit_table", nt_initialize_onexit_table);
    CRT_BOUND("runtime", "_register_onexit_function", nt_register_onexit_function);
    CRT_BOUND("runtime", "_crt_atexit", nt_crt_atexit);
    CRT_BOUND("runtime", "_cexit", nt_cexit);

    CHECK(nt_atoi(" -123end") == -123 && nt_atoi("no") == 0);
    errno = 0;
    CHECK(nt_atoi("999999999999999999999999") == INT32_MAX && errno == ERANGE);
    CHECK(nt_atoi("-2147483649") == INT32_MIN && errno == ERANGE);
    CHECK(nt_atof(" 3.125rest") == 3.125 && nt_atof("not a number") == 0.0);
    CHECK(nt_isspace('\t') != 0 && nt_isspace(0xa0) == 0);
    CHECK(nt_tolower('M') == 'm' && nt_tolower(0xe1) == 0xe1 && nt_toupper('m') == 'M');
    CHECK(nt_configthreadlocale(1) == 2 && nt_configthreadlocale(0) == 1);
    CHECK(nt_configthreadlocale(3) == -1 && errno == EINVAL);
    CHECK(nt_configthreadlocale(2) == 1);
    CHECK(nt_set_new_mode(1) == 0 && nt_set_new_mode(0) == 1);
    CHECK(nt_set_new_mode(2) == -1 && errno == EINVAL);
    const char *const env[] = {"MINES_COLOR=blue", "MINES_COLORFUL=1", NULL};
    nt_runtime_set_guest_environment(env);
    const char *color = nt_getenv("mines_color");
    CHECK(color != NULL && strcmp(color, "blue") == 0 && nt_getenv("MINES") == NULL);
    nt_runtime_set_guest_environment(NULL);
    CHECK(nt_getenv("MINES_COLOR") == NULL);

    const nt_void_callback initializers[] = {append_one, NULL, append_two};
    nt_initterm(initializers, initializers + 3);
    CHECK(callbacks == 12);
    callbacks = 0;
    const nt_int_callback checked[] = {init_one, NULL, init_fail, init_one};
    CHECK(nt_initterm_e(checked, checked + 4) == 23 && callbacks == 12);
    struct nt_onexit_table table;
    CHECK(nt_initialize_onexit_table(&table) == 0 && table.first == NULL);
    for (size_t i = 0; i < 9; ++i)
        CHECK(nt_register_onexit_function(&table, init_one) == 0);
    CHECK(table.last - table.first == 9 && table.first[8] == (nt_guest_proc)init_one);
    nt_runtime_release_onexit_table(&table);
    CHECK(table.first == NULL && nt_register_onexit_function(&table, NULL) == -1 && errno == EINVAL);
    callbacks = 0;
    CHECK(nt_crt_atexit(append_one) == 0 && nt_crt_atexit(append_two) == 0);
    nt_cexit();
    nt_cexit();
    CHECK(callbacks == 21);
    puts("PASS C locale, numeric overflow, isolated environment and callback order");
}

static void check_files(void)
{
    BOUND(KERNEL, CreateDirectoryA);
    BOUND(KERNEL, DeleteFileA);
    BOUND(KERNEL, RemoveDirectoryA);
    BOUND(KERNEL, MoveFileExA);
    BOUND(KERNEL, MoveFileA);
    BOUND(KERNEL, MoveFileW);
    BOUND(KERNEL, CopyFileA);
    BOUND(KERNEL, CopyFileW);
    BOUND(KERNEL, SetFileAttributesA);
    BOUND(KERNEL, SetFileAttributesW);
    BOUND(KERNEL, GetFinalPathNameByHandleW);
    BOUND(KERNEL, GetCurrentDirectoryA);
    BOUND(KERNEL, SetCurrentDirectoryA);
    BOUND(KERNEL, GetFullPathNameA);
    BOUND(KERNEL, GetFileAttributesA);
    BOUND(KERNEL, GetFileAttributesExA);
    BOUND(KERNEL, FindFirstFileExA);
    BOUND(KERNEL, FindFirstFileExW);
    BOUND(KERNEL, FindFirstFileA);
    BOUND(KERNEL, FindFirstFileW);
    BOUND(KERNEL, FindNextFileA);
    BOUND(KERNEL, FindNextFileW);
    BOUND(KERNEL, FindClose);
    BOUND(KERNEL, GetFileInformationByHandle);
    BOUND(KERNEL, GetFileSize);
    BOUND(KERNEL, GetFileTime);
    BOUND(KERNEL, SetFileTime);
    BOUND(KERNEL, GetLogicalDrives);
    BOUND(KERNEL, GetDriveTypeW);
    BOUND(KERNEL, IsDBCSLeadByteEx);
    BOUND(KERNEL, LockFile);
    BOUND(KERNEL, UnlockFile);
    BOUND(KERNEL, LockFileEx);
    BOUND(KERNEL, UnlockFileEx);
    BOUND(KERNEL, GetOverlappedResult);
    BOUND(KERNEL, GetOverlappedResultEx);
    BOUND(KERNEL, CreateIoCompletionPort);
    BOUND(KERNEL, GetQueuedCompletionStatus);
    BOUND(KERNEL, GetQueuedCompletionStatusEx);
    BOUND(KERNEL, PostQueuedCompletionStatus);
    BOUND(KERNEL, CancelIoEx);
    BOUND(KERNEL, GetTempPathA);
    CRT_BOUND("stdio", "_set_fmode", nt_set_fmode);
    CRT_BOUND("stdio", "fopen", nt_fopen);
    CRT_BOUND("stdio", "fclose", nt_fclose);
    CRT_BOUND("stdio", "fread", nt_fread);
    CRT_BOUND("stdio", "fwrite", nt_fwrite);

    CHECK(nt_CreateDirectoryA("data", NULL) == 0 && last_error() == NT_ERROR_PATH_NOT_FOUND);
    char root[] = "/tmp/nt-mines-drive.XXXXXX";
    CHECK(mkdtemp(root) != NULL && nt_runtime_set_file_root(root));
    /* Win32 A codepage compatibility for raw CP1252 bytes, in addition to
     * the UTF-8 strings used by the host runtime. */
    const char cp1252_directory[] = {'C', ':', '\\', 'c', 'a', 'f', (char)0xe9, 0};
    CHECK(nt_CreateDirectoryA(cp1252_directory, NULL) != 0 &&
          (nt_GetFileAttributesA(cp1252_directory) & 0x10u) != 0 &&
          nt_RemoveDirectoryA(cp1252_directory) != 0);
    CHECK(nt_CreateDirectoryA("C:\\data", NULL) != 0);
    /* For direct A calls, try UTF-8 first and use CP1252 only when the
     * UTF-8 file is absent. A W call is UTF-16 and MUST NOT be reinterpreted
     * as CP1252 when the requested Unicode name is missing. */
    char utf8_native[512], acp_native[512];
    CHECK(snprintf(utf8_native, sizeof(utf8_native), "%s/data/caf\xc3\xa9.txt", root) > 0);
    CHECK(snprintf(acp_native, sizeof(acp_native), "%s/data/caf\xc3\x83\xc2\xa9.txt", root) > 0);
    int utf8_fd = open(utf8_native, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(utf8_fd >= 0 && write(utf8_fd, "U", 1) == 1 && close(utf8_fd) == 0);
    int acp_fd = open(acp_native, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(acp_fd >= 0 && write(acp_fd, "A", 1) == 1 && close(acp_fd) == 0);
    const char ambiguous_a[] = "C:\\data\\caf\xc3\xa9.txt";
    uintptr_t ansi_handle = nt_CreateFileA(ambiguous_a, UINT32_C(0x80000000),
                                            7u, NULL, 3u, 0x80u, 0);
    char selected = 0;
    uint32_t received = 0;
    CHECK(ansi_handle != UINTPTR_MAX &&
          nt_ReadFile(ansi_handle, &selected, 1, &received, NULL) &&
          received == 1 && selected == 'U' && nt_CloseHandle(ansi_handle));
    CHECK(unlink(utf8_native) == 0);
    ansi_handle = nt_CreateFileA(ambiguous_a, UINT32_C(0x80000000),
                                 7u, NULL, 3u, 0x80u, 0);
    CHECK(ansi_handle != UINTPTR_MAX &&
          nt_ReadFile(ansi_handle, &selected, 1, &received, NULL) &&
          received == 1 && selected == 'A' && nt_CloseHandle(ansi_handle));
    static const uint16_t unambiguous_w[] = u"C:\\data\\caf\u00e9.txt";
    CHECK(nt_CreateFileW(unambiguous_w, UINT32_C(0x80000000), 7u,
                         NULL, 3u, 0x80u, 0) == UINTPTR_MAX &&
          last_error() == NT_ERROR_FILE_NOT_FOUND);
    CHECK(unlink(acp_native) == 0);
    CHECK(nt_CreateDirectoryA("data", NULL) == 0 && last_error() == NT_ERROR_ALREADY_EXISTS);
    CHECK(nt_CreateDirectoryA("DATA", NULL) == 0 && last_error() == NT_ERROR_ALREADY_EXISTS);
    CHECK(nt_GetCurrentDirectoryA(0, NULL) == 4);
    char win_cwd[256], absolute_path[256], *base_name = NULL;
    CHECK(nt_GetCurrentDirectoryA(sizeof(win_cwd), win_cwd) == 3 &&
          strcmp(win_cwd, "C:\\") == 0);
    CHECK(nt_SetCurrentDirectoryA("C:\\DATA") != 0);
    CHECK(nt_GetCurrentDirectoryA(sizeof(win_cwd), win_cwd) == 7 &&
          strcmp(win_cwd, "C:\\DATA") == 0);
    CHECK(nt_GetFullPathNameA("..\\data\\scores.txt", sizeof(absolute_path),
                              absolute_path, &base_name) == strlen("C:\\data\\scores.txt") &&
          strcmp(absolute_path, "C:\\data\\scores.txt") == 0 &&
          base_name != NULL && strcmp(base_name, "scores.txt") == 0);
    CHECK(nt_GetFullPathNameA("C:drive-relative.txt", sizeof(absolute_path),
                              absolute_path, NULL) ==
          strlen("C:\\DATA\\drive-relative.txt") &&
          strcmp(absolute_path, "C:\\DATA\\drive-relative.txt") == 0);
    CHECK(nt_CreateDirectoryA("C:relative-child", NULL) != 0);
    CHECK(nt_RemoveDirectoryA("C:\\DATA\\RELATIVE-CHILD") != 0);
    CHECK(nt_GetFullPathNameA("\\\\?\\C:\\DATA\\hello.txt",
                              sizeof(absolute_path), absolute_path, &base_name) ==
          strlen("C:\\DATA\\hello.txt") &&
          strcmp(absolute_path, "C:\\DATA\\hello.txt") == 0 &&
          base_name != NULL && strcmp(base_name, "hello.txt") == 0);
    CHECK(nt_GetFullPathNameA("\\\\remote\\share", sizeof(absolute_path),
                              absolute_path, NULL) == 0 &&
          last_error() == NT_ERROR_ACCESS_DENIED);
    CHECK(nt_SetCurrentDirectoryA("C:\\") != 0);
    CHECK((nt_GetFileAttributesA("C:\\DATA") & 0x10u) != 0);
    struct nt_file_attribute_data folder_attributes;
    CHECK(nt_GetFileAttributesExA("data", 0, &folder_attributes) != 0 &&
          (folder_attributes.attributes & 0x10u) != 0);
    const uint32_t temp_required = nt_GetTempPathA(0, NULL);
    CHECK(temp_required > 3 && temp_required < sizeof(absolute_path));
    CHECK(nt_GetTempPathA(sizeof(absolute_path), absolute_path) == temp_required - 1 &&
          strncmp(absolute_path, "C:\\", 3) == 0);
    CHECK(nt_CreateDirectoryA("data\\nested", NULL) != 0);
    CHECK(nt_CreateDirectoryA("DATA\\Nested", NULL) == 0 && last_error() == NT_ERROR_ALREADY_EXISTS);
    CHECK(nt_CreateDirectoryA("data\\nested\\..\\subfolder", NULL) != 0);
    CHECK(nt_RemoveDirectoryA("DATA\\SUBFOLDER") != 0);
    CHECK(nt_RemoveDirectoryA("DATA\\NESTED") != 0);
    /* A Windows process keeps its current directory reserved. Renaming
     * that directory would leave every relative C: path pointing nowhere. */
    CHECK(nt_CreateDirectoryA("data\\current", NULL) != 0);
    CHECK(nt_SetCurrentDirectoryA("C:\\DATA\\CURRENT") != 0);
    CHECK(nt_MoveFileExA("C:\\DATA\\current", "C:\\data\\relocated", 0) == 0 &&
          last_error() == NT_ERROR_SHARING_VIOLATION);
    /* An empty active CWD must not be unlinked and leave all relative
     * guest paths dangling. Its case-insensitive DOS spelling is protected. */
    nt_SetLastError(0);
    CHECK(nt_RemoveDirectoryA("c:\\data\\CuRrEnT") == 0 &&
          last_error() == NT_ERROR_SHARING_VIOLATION);
    CHECK(nt_GetCurrentDirectoryA(sizeof(win_cwd), win_cwd) ==
          strlen("C:\\DATA\\CURRENT") &&
          strcmp(win_cwd, "C:\\DATA\\CURRENT") == 0);
    CHECK(nt_SetCurrentDirectoryA("C:\\") != 0);
    CHECK(nt_MoveFileExA("DATA\\CURRENT", "data\\relocated", 0) != 0);
    CHECK(nt_RemoveDirectoryA("data\\RELOCATED") != 0);
    /* The guest sees a DOS path, while Linux receives a normal directory
     * entry. The path is not a separately emulated disk or a copied file. */
    CHECK(nt_CreateDirectoryA("C:\\Program Files", NULL) != 0);
    CHECK(nt_CreateDirectoryA("C:\\Program Files\\Demo App", NULL) != 0);
    void *installed = nt_fopen("C:\\Program Files\\Demo App\\settings.ini", "wb");
    CHECK(installed != NULL && nt_fwrite("local", 1, 5, installed) == 5 &&
          nt_fclose(installed) == 0);
    char host_installed[512];
    CHECK(snprintf(host_installed, sizeof(host_installed),
                   "%s/Program Files/Demo App/settings.ini", root) > 0);
    FILE *native = fopen(host_installed, "rb");
    char installed_bytes[5] = {0};
    CHECK(native != NULL && fread(installed_bytes, 1, 5, native) == 5 &&
          memcmp(installed_bytes, "local", 5) == 0 && fclose(native) == 0);
    void *folded = nt_fopen("c:\\pROGRAM fILES\\dEMO aPP\\SETTINGS.INI", "rb");
    char folded_bytes[5] = {0};
    CHECK(folded != NULL && nt_fread(folded_bytes, 1, 5, folded) == 5 &&
          memcmp(folded_bytes, "local", 5) == 0 && nt_fclose(folded) == 0);
    folded = nt_fopen("C:\\PROGRAM FILES\\DEMO APP\\Settings.ini", "ab");
    CHECK(folded != NULL && nt_fwrite("!", 1, 1, folded) == 1 &&
          nt_fclose(folded) == 0);
    native = fopen(host_installed, "rb");
    char appended[6] = {0};
    CHECK(native != NULL && fread(appended, 1, 6, native) == 6 &&
          memcmp(appended, "local!", 6) == 0 && fclose(native) == 0);
    CHECK(nt_DeleteFileA("C:\\PROGRAM FILES\\DEMO APP\\SETTINGS.ini") != 0);
    CHECK(nt_RemoveDirectoryA("C:\\Program Files\\Demo App") != 0);
    CHECK(nt_RemoveDirectoryA("C:\\Program Files") != 0);

    CHECK(nt_CreateDirectoryA("missing\\nested", NULL) == 0 && last_error() == NT_ERROR_PATH_NOT_FOUND);
    CHECK(nt_CreateDirectoryA("..\\escape", NULL) == 0 && last_error() == NT_ERROR_ACCESS_DENIED);
    char link[128];
    snprintf(link, sizeof(link), "%s/link", root);
    CHECK(symlink("data", link) == 0);
    CHECK(nt_fopen("link\\outside", "wb") == NULL && errno == ELOOP);
    CHECK(nt_fopen("LINK\\outside", "wb") == NULL && errno == ELOOP);
    CHECK(nt_MoveFileExA("link", "link-renamed", 0) == 0 &&
          last_error() == NT_ERROR_ACCESS_DENIED);
    CHECK(unlink(link) == 0);

    /* Exact spelling wins; two non-exact ASCII-folded host names are
     * ambiguous, never candidates for an arbitrary selection. */
    char upper_name[512], lower_name[512];
    CHECK(snprintf(upper_name, sizeof(upper_name), "%s/Ambiguous", root) > 0);
    CHECK(snprintf(lower_name, sizeof(lower_name), "%s/ambiguous", root) > 0);
    native = fopen(upper_name, "wb");
    CHECK(native != NULL && fclose(native) == 0);
    native = fopen(lower_name, "wb");
    CHECK(native != NULL && fclose(native) == 0);
    CHECK(nt_fopen("AMBIGUOUS", "rb") == NULL && errno == EEXIST);
    folded = nt_fopen("Ambiguous", "rb");
    CHECK(folded != NULL && nt_fclose(folded) == 0);
    CHECK(unlink(upper_name) == 0 && unlink(lower_name) == 0);

    /* Unicode case folding is locale-independent, including every parent
     * component and Windows wildcard matching on Unicode characters. */
    CHECK(nt_CreateDirectoryA("C:\\Étage", NULL) != 0);
    CHECK(nt_CreateDirectoryA("c:\\étage", NULL) == 0 &&
          last_error() == NT_ERROR_ALREADY_EXISTS);
    CHECK(nt_SetFileAttributesA("C:\\éTAGE", 1u | 2u) != 0 &&
          (nt_GetFileAttributesW(u"C:\\Étage") & (1u | 2u | 0x10u)) ==
          (1u | 2u | 0x10u));
    void *international = nt_fopen("C:\\Étage\\Résumé.TXT", "wb");
    CHECK(international != NULL && nt_fwrite("ok", 1, 2, international) == 2 &&
          nt_fclose(international) == 0);
    international = nt_fopen("c:\\étage\\résumé.txt", "rb");
    char international_bytes[2];
    CHECK(international != NULL &&
          nt_fread(international_bytes, 1, 2, international) == 2 &&
          memcmp(international_bytes, "ok", 2) == 0 &&
          nt_fclose(international) == 0);
    struct nt_find_data_w found_w;
    uintptr_t search = nt_FindFirstFileW(u"C:\\étage\\RÉSU?É.t?t", &found_w);
    CHECK(search != UINTPTR_MAX &&
          memcmp(found_w.name, u"Résumé.TXT", sizeof(u"Résumé.TXT")) == 0);
    CHECK(!nt_FindNextFileW(search, &found_w) && last_error() == NT_ERROR_NO_MORE_FILES);
    CHECK(nt_FindClose(search) != 0);

    struct nt_find_data_a found_a;
    search = nt_FindFirstFileA("C:\\\xE9tage\\*.txt", &found_a);
    static const unsigned char expected_ansi[] = {
        'R', 0xe9, 's', 'u', 'm', 0xe9, '.', 'T', 'X', 'T', 0
    };
    CHECK(search != UINTPTR_MAX &&
          memcmp(found_a.name, expected_ansi, sizeof(expected_ansi)) == 0 &&
          nt_FindClose(search) != 0);
    search = nt_FindFirstFileExW(u"C:\\*", 0, &found_w, 1, NULL, 0);
    CHECK(search != UINTPTR_MAX && (found_w.info.attributes & 0x10u) != 0 &&
          nt_FindClose(search) != 0);

    /* HIDDEN, SYSTEM, ARCHIVE and a mutable Windows creation timestamp
     * are persisted as Linux user xattrs on the original file. */
    CHECK(nt_SetFileAttributesA("c:\\étage\\RÉSUMÉ.TXT", 2u | 4u | 32u) != 0);
    CHECK((nt_GetFileAttributesW(u"C:\\ÉTAGE\\résumé.txt") & (2u | 4u | 32u)) ==
          (2u | 4u | 32u));
    uintptr_t info_handle = nt_CreateFileW(u"C:\\étage\\résumé.txt",
        UINT32_C(0xc0000000), 7u, NULL, 3u, 0x80u, 0);
    CHECK(info_handle != UINTPTR_MAX);
    uint32_t high = 123u;
    CHECK(nt_GetFileSize(info_handle, &high) == 2 && high == 0);
    struct nt_handle_file_info handle_info;
    CHECK(nt_GetFileInformationByHandle(info_handle, &handle_info) != 0 &&
          handle_info.size_low == 2 && handle_info.size_high == 0 &&
          (handle_info.attributes & 2u) != 0);
    const struct nt_file_time birth = {0x12345678u, 0x01dbeeeeu};
    const struct nt_file_time modified = {0x87654321u, 0x01dbeeedu};
    CHECK(nt_SetFileTime(info_handle, &birth, NULL, &modified) != 0);
    struct nt_file_time actual_birth = {0}, actual_modified = {0};
    CHECK(nt_GetFileTime(info_handle, &actual_birth, NULL, &actual_modified) != 0 &&
          memcmp(&actual_birth, &birth, sizeof(birth)) == 0 &&
          memcmp(&actual_modified, &modified, sizeof(modified)) == 0);
    CHECK(nt_CloseHandle(info_handle) != 0);
    CHECK(nt_CopyFileA("C:\\étage\\RÉSUMÉ.TXT", "C:\\Étage\\Clone.bin", 1) != 0);
    CHECK((nt_GetFileAttributesA("C:\\étage\\clone.bin") & (2u | 4u | 32u)) ==
          (2u | 4u | 32u));
    CHECK(nt_DeleteFileA("c:\\étage\\CLONE.BIN") != 0 &&
          nt_DeleteFileA("c:\\étage\\RÉSUMÉ.TXT") != 0 &&
          nt_RemoveDirectoryA("C:\\ÉTAGE") != 0);

    void *text = nt_fopen("C:\\data\\scores.txt", "w");
    CHECK(text != NULL && nt_fwrite("a\nb\n", 2, 2, text) == 2);
    nt_cexit();
    void *raw = nt_fopen("data/scores.txt", "rb");
    unsigned char buffer[12] = {0};
    CHECK(raw != NULL && nt_fread(buffer, 1, sizeof(buffer), raw) == 6 &&
          memcmp(buffer, "a\r\nb\r\n", 6) == 0);
    CHECK(nt_fclose(raw) == 0 && nt_fclose(text) == 0);
    CHECK(nt_set_fmode(0x8000) == 0);
    raw = nt_fopen("data/scores.txt", "r");
    CHECK(raw != NULL && nt_fread(buffer, 2, 6, raw) == 3 && nt_fclose(raw) == 0);
    CHECK(nt_set_fmode(0x4000) == 0 && nt_set_fmode(0) == EINVAL);
    text = nt_fopen("data/scores.txt", "r");
    CHECK(text != NULL && nt_fread(buffer, 2, 6, text) == 2 &&
          memcmp(buffer, "a\nb\n", 4) == 0 && nt_fclose(text) == 0);
    CHECK(nt_fclose(NULL) == EOF && errno == EINVAL);

    raw = nt_fopen("data/target.txt", "wb");
    CHECK(raw != NULL && nt_fclose(raw) == 0);
    /* Native copy does not invoke cp(1), create an alternate drive, or
     * duplicate files merely because the Windows caller changes case. */
    /* An open handle without delete sharing blocks the operation; an
     * explicitly shared-delete handle keeps the original Linux inode alive. */
    void *delete_stream = nt_fopen("data/delete-me.bin", "wb");
    CHECK(delete_stream != NULL && nt_fwrite("X", 1, 1, delete_stream) == 1 &&
          nt_fclose(delete_stream) == 0);
    uintptr_t delete_handle = nt_CreateFileA("DATA/DELETE-ME.BIN",
        UINT32_C(0x80000000), 1u, NULL, 3u, 0x80u, 0);
    CHECK(delete_handle != UINTPTR_MAX);
    CHECK(nt_DeleteFileA("data/delete-me.bin") == 0 &&
          last_error() == NT_ERROR_SHARING_VIOLATION);
    CHECK(nt_CloseHandle(delete_handle) != 0);
    CHECK(nt_SetFileAttributesA("data/delete-me.bin", 1u) != 0);
    CHECK(nt_DeleteFileA("DATA/DELETE-ME.BIN") == 0 &&
          last_error() == NT_ERROR_ACCESS_DENIED);
    CHECK(nt_SetFileAttributesA("data/delete-me.bin", 0x80u) != 0);
    delete_handle = nt_CreateFileA("data/delete-me.bin",
        UINT32_C(0x80000000), 1u | 4u, NULL, 3u, 0x80u, 0);
    CHECK(delete_handle != UINTPTR_MAX);
    CHECK(nt_DeleteFileA("DATA/DELETE-ME.BIN") != 0);
    char still_open = 0;
    uint32_t got_delete = 0;
    CHECK(nt_ReadFile(delete_handle, &still_open, 1, &got_delete, NULL) != 0 &&
          got_delete == 1 && still_open == 'X');
    CHECK(nt_CloseHandle(delete_handle) != 0);
    CHECK(nt_GetFileAttributesA("data/delete-me.bin") == UINT32_MAX);

    /* Linux OFD advisory locks are backed by the actual inode and survive
     * temporary fd duplicates. Distinct Windows opens may conflict. */
    const uintptr_t first_lock = nt_CreateFileA("data/locked.bin", UINT32_C(0xc0000000),
                                                  3u, NULL, 2u, 0x80u, 0);
    CHECK(first_lock != UINTPTR_MAX);
    const uintptr_t second_lock = nt_CreateFileA("DATA/LOCKED.bin", UINT32_C(0xc0000000),
                                                   3u, NULL, 3u, 0x80u, 0);
    CHECK(second_lock != UINTPTR_MAX);
    CHECK(nt_LockFile(first_lock, 0, 0, 8, 0) != 0);
    CHECK(nt_LockFile(second_lock, 0, 0, 8, 0) == 0 &&
          last_error() == NT_ERROR_LOCK_VIOLATION);
    int64_t start_locked = -1;
    CHECK(nt_SetFilePointerEx(second_lock, 0, &start_locked, 0) && start_locked == 0);
    char check_locked[4];
    uint32_t checked_bytes = 0;
    CHECK(!nt_ReadFile(second_lock, check_locked, 4, &checked_bytes, NULL) &&
          last_error() == NT_ERROR_LOCK_VIOLATION);
    CHECK(nt_UnlockFile(first_lock, 0, 0, 8, 0) != 0 &&
          nt_LockFile(second_lock, 0, 0, 8, 0) != 0 &&
          nt_UnlockFile(second_lock, 0, 0, 8, 0) != 0);

    const uintptr_t completed = nt_CreateEventW(NULL, 1, 0, NULL);
    CHECK(completed != 0 && completed != UINTPTR_MAX);
    struct nt_overlapped overlapped = {.offset = 16u, .event = completed};
    CHECK(nt_LockFileEx(first_lock, 1u, 0, 4, 0, &overlapped) != 0);
    CHECK(nt_WaitForSingleObject(completed, 0) == 0);
    CHECK(nt_LockFileEx(second_lock, 3u, 0, 4, 0, &overlapped) == 0 &&
          last_error() == NT_ERROR_LOCK_VIOLATION);
    CHECK(nt_UnlockFileEx(first_lock, 0, 4, 0, &overlapped) != 0);

    uint32_t written_bytes = 0;
    CHECK(nt_WriteFile(first_lock, "abc", 3, &written_bytes, NULL) != 0 &&
          written_bytes == 3);
    overlapped.offset = 1;
    CHECK(nt_WriteFile(first_lock, "Z", 1, NULL, &overlapped) != 0);
    CHECK(nt_GetOverlappedResult(first_lock, &overlapped, &written_bytes, 1) != 0 &&
          written_bytes == 1);
    int64_t file_position = -1;
    CHECK(nt_SetFilePointerEx(first_lock, 0, &file_position, 1) != 0 &&
          file_position == 3);
    overlapped.offset = 0;
    char positional[4] = {0};
    CHECK(nt_ReadFile(first_lock, positional, 3, NULL, &overlapped) != 0 &&
          nt_GetOverlappedResult(first_lock, &overlapped, &written_bytes, 1) != 0 &&
          written_bytes == 3 && memcmp(positional, "aZc", 3) == 0);
    CHECK(nt_CloseHandle(completed) != 0 && nt_CloseHandle(second_lock) != 0 &&
          nt_CloseHandle(first_lock) != 0 && nt_DeleteFileA("data/LOCKED.bin") != 0);

    /* FILE_FLAG_OVERLAPPED takes the real worker path, rather than the
     * immediate pread/pwrite completion preserved for legacy handles. */
    const uintptr_t background = nt_CreateFileA("data/background.bin",
        UINT32_C(0xc0000000), 7u, NULL, 2u, 0x40000080u, 0);
    CHECK(background != UINTPTR_MAX);
    const uintptr_t background_event = nt_CreateEventW(NULL, 1, 0, NULL);
    CHECK(background_event != 0 && background_event != UINTPTR_MAX);
    CHECK((background_event & (uintptr_t)3) == 0);
    struct nt_overlapped async_write = {.offset = 5, .event = background_event};
    CHECK(!nt_WriteFile(background, "async", 5, NULL, &async_write) &&
          last_error() == NT_ERROR_IO_PENDING);
    uint32_t async_completed = 0;
    CHECK(nt_GetOverlappedResult(background, &async_write, &async_completed, 1) != 0 &&
          async_completed == 5);
    CHECK(nt_WaitForSingleObject(background_event, 1000) == 0);
    struct nt_overlapped async_read = {.offset = 5, .event = background_event};
    char background_data[6] = {0};
    CHECK(!nt_ReadFile(background, background_data, 5, NULL, &async_read) &&
          last_error() == NT_ERROR_IO_PENDING);
    CHECK(nt_GetOverlappedResult(background, &async_read, &async_completed, 1) != 0 &&
          async_completed == 5 && memcmp(background_data, "async", 5) == 0);

    /* The first read is already running, while two later reads remain in
     * the queue. Cancellation must target the originating Windows HANDLE,
     * publish ERROR_OPERATION_ABORTED, signal its event, and leave the
     * active request running until Linux completes it. */
    pthread_mutex_lock(&async_test_lock);
    async_hold_enabled = true;
    async_hold_entered = false;
    async_hold_release = false;
    pthread_mutex_unlock(&async_test_lock);
    const uintptr_t running_event = nt_CreateEventW(NULL, 1, 0, NULL);
    const uintptr_t cancelled_event = nt_CreateEventW(NULL, 1, 0, NULL);
    CHECK(running_event && cancelled_event);
    CHECK((running_event & (uintptr_t)3) == 0 &&
          (cancelled_event & (uintptr_t)3) == 0);
    struct nt_overlapped running = {.offset = 4096, .event = running_event};
    struct nt_overlapped victim = {.offset = 0, .event = cancelled_event};
    char pending_byte = 0, unused_byte = 0;
    CHECK(!nt_ReadFile(background, &pending_byte, 1, NULL, &running) &&
          last_error() == NT_ERROR_IO_PENDING);
    pthread_mutex_lock(&async_test_lock);
    while (!async_hold_entered)
        pthread_cond_wait(&async_test_changed, &async_test_lock);
    pthread_mutex_unlock(&async_test_lock);
    CHECK(!nt_ReadFile(background, &unused_byte, 1, NULL, &victim) &&
          last_error() == NT_ERROR_IO_PENDING);
    const uintptr_t alternate = nt_CreateFileA("data/background.bin", UINT32_C(0x80000000),
        7u, NULL, 3u, 0x40000080u, 0);
    CHECK(alternate != UINTPTR_MAX);
    /* While background's first read is blocked, a second HANDLE must be
     * served by another Linux worker rather than waiting for the entire
     * worker queue to drain. */
    struct nt_overlapped concurrent = {.offset = 5};
    char concurrent_buffer[6] = {0};
    CHECK(!nt_ReadFile(alternate, concurrent_buffer, 5, NULL, &concurrent) &&
          last_error() == NT_ERROR_IO_PENDING);
    CHECK(nt_GetOverlappedResultEx(alternate, &concurrent, &async_completed, 1000, 0) &&
          async_completed == 5 && memcmp(concurrent_buffer, "async", 5) == 0);
    CHECK(!nt_GetOverlappedResultEx(background, &victim, &async_completed, 0, 0) &&
          last_error() == NT_ERROR_IO_INCOMPLETE);
    CHECK(!nt_GetOverlappedResultEx(background, &victim, &async_completed, 2, 0) &&
          last_error() == NT_ERROR_WAIT_TIMEOUT);
    CHECK(!nt_GetOverlappedResultEx(background, &victim, &async_completed, 2, 1) &&
          last_error() == NT_ERROR_NOT_SUPPORTED);
    CHECK(!nt_CancelIoEx(alternate, &victim) && last_error() == NT_ERROR_NOT_FOUND);
    CHECK(nt_CancelIoEx(background, &victim));
    CHECK(!nt_GetOverlappedResult(background, &victim, &async_completed, 1) &&
          last_error() == NT_ERROR_OPERATION_ABORTED);
    CHECK(nt_WaitForSingleObject(cancelled_event, 1000) == 0);
    /* NULL cancels all queued requests associated with the HANDLE, but
     * leaves the already-running read to complete normally. */
    struct nt_overlapped all_one = {.offset = 1};
    struct nt_overlapped all_two = {.offset = 2};
    char all_one_byte = 0, all_two_byte = 0;
    CHECK(!nt_ReadFile(background, &all_one_byte, 1, NULL, &all_one) &&
          last_error() == NT_ERROR_IO_PENDING);
    CHECK(!nt_ReadFile(background, &all_two_byte, 1, NULL, &all_two) &&
          last_error() == NT_ERROR_IO_PENDING);
    CHECK(nt_CancelIoEx(background, NULL));
    CHECK(!nt_GetOverlappedResult(background, &all_one, &async_completed, 1) &&
          last_error() == NT_ERROR_OPERATION_ABORTED);
    CHECK(!nt_GetOverlappedResult(background, &all_two, &async_completed, 1) &&
          last_error() == NT_ERROR_OPERATION_ABORTED);
    CHECK(!nt_CancelIoEx(background, &running) && last_error() == NT_ERROR_NOT_FOUND);
    pthread_mutex_lock(&async_test_lock);
    async_hold_release = true;
    async_hold_enabled = false;
    pthread_cond_broadcast(&async_test_changed);
    pthread_mutex_unlock(&async_test_lock);
    CHECK(nt_GetOverlappedResultEx(background, &running, &async_completed, 1000, 0) &&
          async_completed == 0);
    CHECK(nt_WaitForSingleObject(running_event, 1000) == 0);
    CHECK(nt_CloseHandle(alternate) && nt_CloseHandle(running_event) &&
          nt_CloseHandle(cancelled_event));

    /* Per-process IOCP supports manual packets and associates native file
     * completion notifications by file handle/key. Windows event low bit
     * suppresses posting but does not prevent GetOverlappedResult. */
    const uintptr_t port = nt_CreateIoCompletionPort(UINTPTR_MAX, 0, 0, 0);
    CHECK(port != 0);
    struct nt_overlapped posted = {0};
    CHECK(nt_PostQueuedCompletionStatus(port, 17, 0xBEEFu, &posted));
    uint32_t packet_bytes = 0;
    uintptr_t packet_key = 0;
    struct nt_overlapped *packet_overlapped = NULL;
    CHECK(nt_GetQueuedCompletionStatus(port, &packet_bytes, &packet_key,
                                        &packet_overlapped, 0) &&
          packet_bytes == 17 && packet_key == 0xBEEFu &&
          packet_overlapped == &posted);
    /* Native x64 OVERLAPPED_ENTRY ABI, ordered batch dequeue and mixed
     * consumers on the same IOCP. Reject invalid/alertable waits without
     * consuming packets or leaving a false success in the result count. */
    struct nt_overlapped_entry batch[3] = {0};
    uint32_t removed = 77;
    CHECK(!nt_GetQueuedCompletionStatusEx(port, batch, 0, &removed, 0, 0) &&
          last_error() == NT_ERROR_INVALID_PARAMETER);
    CHECK(!nt_GetQueuedCompletionStatusEx(port, NULL, 1, &removed, 0, 0) &&
          last_error() == NT_ERROR_INVALID_PARAMETER);
    CHECK(!nt_GetQueuedCompletionStatusEx(port, batch, 1, &removed, 0, 1) &&
          last_error() == NT_ERROR_NOT_SUPPORTED && removed == 0);
    CHECK(!nt_GetQueuedCompletionStatusEx(port, batch, 1, &removed, 0, 0) &&
          last_error() == NT_ERROR_WAIT_TIMEOUT && removed == 0);
    CHECK(nt_PostQueuedCompletionStatus(port, 11, 111, &posted));
    CHECK(nt_PostQueuedCompletionStatus(port, 22, 222, NULL));
    CHECK(nt_PostQueuedCompletionStatus(port, 33, 333, &posted));
    CHECK(nt_GetQueuedCompletionStatusEx(port, batch, 2, &removed, 0, 0) &&
          removed == 2 && batch[0].completion_key == 111 &&
          batch[0].overlap == &posted && batch[0].internal == 0 &&
          batch[0].bytes == 11 && batch[1].completion_key == 222 &&
          batch[1].overlap == NULL && batch[1].internal == 0 &&
          batch[1].bytes == 22);
    CHECK(nt_GetQueuedCompletionStatus(port, &packet_bytes, &packet_key,
                                        &packet_overlapped, 0) &&
          packet_bytes == 33 && packet_key == 333 &&
          packet_overlapped == &posted);

    packet_overlapped = &posted;
    CHECK(!nt_GetQueuedCompletionStatus(port, &packet_bytes, &packet_key,
                                         &packet_overlapped, 0) &&
          last_error() == NT_ERROR_WAIT_TIMEOUT && packet_overlapped == NULL);
    CHECK(nt_CreateIoCompletionPort(background, port, 0xCAFEu, 0) == port);
    CHECK(nt_CreateIoCompletionPort(background, port, 0xDEADu, 0) == 0 &&
          last_error() == NT_ERROR_INVALID_PARAMETER);
    char port_read_buffer[6] = {0};
    struct nt_overlapped port_read = {.offset = 5};
    CHECK(!nt_ReadFile(background, port_read_buffer, 5, NULL, &port_read) &&
          last_error() == NT_ERROR_IO_PENDING);
    CHECK(nt_GetQueuedCompletionStatus(port, &packet_bytes, &packet_key,
                                        &packet_overlapped, 1000) &&
          packet_bytes == 5 && packet_key == 0xCAFEu &&
          packet_overlapped == &port_read &&
          memcmp(port_read_buffer, "async", 5) == 0);
    struct nt_overlapped suppressed = {.offset = 5, .event = background_event | 1u};
    char suppressed_buffer[6] = {0};
    CHECK(!nt_ReadFile(background, suppressed_buffer, 5, NULL, &suppressed) &&
          last_error() == NT_ERROR_IO_PENDING);
    CHECK(nt_GetOverlappedResult(background, &suppressed, &async_completed, 1) &&
          async_completed == 5 && memcmp(suppressed_buffer, "async", 5) == 0);
    packet_overlapped = &posted;
    CHECK(!nt_GetQueuedCompletionStatus(port, &packet_bytes, &packet_key,
                                         &packet_overlapped, 0) &&
          last_error() == NT_ERROR_WAIT_TIMEOUT && packet_overlapped == NULL);
    /* Queue cancellation must also deliver an unsuccessful completion
     * packet with the original completion key and OVERLAPPED identity. */
    pthread_mutex_lock(&async_test_lock);
    async_hold_enabled = true;
    async_hold_entered = false;
    async_hold_release = false;
    pthread_mutex_unlock(&async_test_lock);
    struct nt_overlapped blocked_iocp = {.offset = 4096};
    struct nt_overlapped queued_iocp = {.offset = 0};
    char blocked_byte = 0, cancelled_byte = 0;
    CHECK(!nt_ReadFile(background, &blocked_byte, 1, NULL, &blocked_iocp) &&
          last_error() == NT_ERROR_IO_PENDING);
    pthread_mutex_lock(&async_test_lock);
    while (!async_hold_entered)
        pthread_cond_wait(&async_test_changed, &async_test_lock);
    pthread_mutex_unlock(&async_test_lock);
    CHECK(!nt_ReadFile(background, &cancelled_byte, 1, NULL, &queued_iocp) &&
          last_error() == NT_ERROR_IO_PENDING);
    CHECK(nt_CancelIoEx(background, &queued_iocp));
    packet_overlapped = NULL;
    CHECK(!nt_GetQueuedCompletionStatus(port, &packet_bytes, &packet_key,
                                         &packet_overlapped, 1000) &&
          last_error() == NT_ERROR_OPERATION_ABORTED &&
          packet_bytes == 0 && packet_key == 0xCAFEu &&
          packet_overlapped == &queued_iocp);
    /* A canceled OVERLAPPED reported by GQCSEx remains a valid dequeued
     * entry, with STATUS_CANCELLED in Internal rather than a Win32 error. */
    struct nt_overlapped cancelled_batch = {.offset = 0};
    char cancelled_batch_byte = 0;
    CHECK(!nt_ReadFile(background, &cancelled_batch_byte, 1, NULL, &cancelled_batch) &&
          last_error() == NT_ERROR_IO_PENDING);
    CHECK(nt_CancelIoEx(background, &cancelled_batch));
    CHECK(nt_GetQueuedCompletionStatusEx(port, batch, 3, &removed, 1000, 0) &&
          removed == 1 && batch[0].completion_key == 0xCAFEu &&
          batch[0].overlap == &cancelled_batch &&
          batch[0].bytes == 0 && batch[0].internal == UINT32_C(0xc0000120));
    pthread_mutex_lock(&async_test_lock);
    async_hold_release = true;
    async_hold_enabled = false;
    pthread_cond_broadcast(&async_test_changed);
    pthread_mutex_unlock(&async_test_lock);
    CHECK(nt_GetQueuedCompletionStatus(port, &packet_bytes, &packet_key,
                                        &packet_overlapped, 1000) &&
          packet_bytes == 0 && packet_key == 0xCAFEu &&
          packet_overlapped == &blocked_iocp);

    CHECK(nt_CloseHandle(port));
    CHECK(!nt_PostQueuedCompletionStatus(port, 1, 2, NULL) &&
          last_error() == NT_ERROR_INVALID_HANDLE);
    CHECK(!nt_GetQueuedCompletionStatusEx(port, batch, 1, &removed, 0, 0) &&
          last_error() == NT_ERROR_INVALID_HANDLE && removed == 0);
    CHECK(nt_CloseHandle(background) && nt_CloseHandle(background_event) &&
          nt_DeleteFileA("data/background.bin"));

    CHECK(nt_CopyFileA("C:\\DATA\\SCORES.TXT", "C:\\data\\Backup.TXT", 1) != 0);
    CHECK(nt_CopyFileA("data/scores.txt", "DATA/backup.txt", 1) == 0 &&
          last_error() == NT_ERROR_FILE_EXISTS);
    CHECK(nt_CopyFileA("data/SCORES.TXT", "DATA/BACKUP.txt", 0) != 0);
    CHECK(nt_CopyFileA("data/scores.txt", "DATA/SCORES.TXT", 0) == 0);
    /* Preserve the Linux inode's basic Windows-visible metadata when
     * copying: readonly mode and modification time. */
    char source_native[512], readonly_native[512];
    CHECK(snprintf(source_native, sizeof(source_native), "%s/data/scores.txt", root) > 0);
    CHECK(snprintf(readonly_native, sizeof(readonly_native), "%s/data/ReadonlyCopy.bin", root) > 0);
    const struct timespec fixed_times[2] = {{1700000000, 0}, {1700000042, 123456789}};
    CHECK(utimensat(AT_FDCWD, source_native, fixed_times, 0) == 0);
    CHECK(nt_SetFileAttributesA("DATA/SCORES.TXT", 1u) != 0);
    CHECK(nt_CopyFileA("data/SCORES.txt", "data/ReadonlyCopy.bin", 1) != 0);
    struct stat source_stat, copy_stat;
    CHECK(stat(source_native, &source_stat) == 0 &&
          stat(readonly_native, &copy_stat) == 0 &&
          (source_stat.st_mode & 0777) == (copy_stat.st_mode & 0777) &&
          copy_stat.st_mtim.tv_sec == source_stat.st_mtim.tv_sec &&
          copy_stat.st_mtim.tv_nsec == source_stat.st_mtim.tv_nsec &&
          (nt_GetFileAttributesA("data/READONLYCOPY.bin") & 1u) != 0);
    static const uint16_t wide_readonly[] = {
        'C', ':', '\\', 'd', 'a', 't', 'a', '\\',
        'R', 'e', 'a', 'd', 'o', 'n', 'l', 'y', 'C', 'o', 'p', 'y', '.', 'b', 'i', 'n', 0
    };
    CHECK(nt_SetFileAttributesW(wide_readonly, 0x80u) != 0);
    CHECK((nt_GetFileAttributesA("data/readonlycopy.bin") & 1u) == 0);
    CHECK(nt_DeleteFileA("data/READONLYCOPY.BIN") != 0);
    CHECK(nt_SetFileAttributesA("data/scores.txt", 0x80u) != 0);
    CHECK(nt_SetFileAttributesA("data/scores.txt", 0x1000u) == 0 &&
          last_error() == NT_ERROR_NOT_SUPPORTED);
    CHECK(nt_MoveFileA("DATA/backup.txt", "data/Copied.TXT") != 0);
    folded = nt_fopen("data/cOPIED.txt", "rb");
    char copied[6] = {0};
    CHECK(folded != NULL && nt_fread(copied, 1, 6, folded) == 6 &&
          memcmp(copied, "a\r\nb\r\n", 6) == 0 && nt_fclose(folded) == 0);
    CHECK(nt_DeleteFileA("DATA/COPIED.txt") != 0);

    static const uint16_t wide_from[] = {
        'C', ':', '\\', 'd', 'a', 't', 'a', '\\',
        's', 'c', 'o', 'r', 'e', 's', '.', 't', 'x', 't', 0
    };
    static const uint16_t wide_copy[] = {
        'C', ':', '\\', 'D', 'A', 'T', 'A', '\\',
        'W', 'I', 'D', 'E', '.', 'T', 'X', 'T', 0
    };
    static const uint16_t wide_moved[] = {
        'C', ':', '\\', 'd', 'a', 't', 'a', '\\',
        'w', 'i', 'd', 'e', '-', 'f', 'i', 'n', 'a', 'l', '.', 't', 'x', 't', 0
    };
    CHECK(nt_CopyFileW(wide_from, wide_copy, 1) != 0);
    CHECK(nt_MoveFileW(wide_copy, wide_moved) != 0);
    CHECK(nt_DeleteFileA("C:\\DATA\\WIDE-FINAL.TXT") != 0);

    CHECK(nt_MoveFileExA("data/scores.txt", "data/target.txt", 0) == 0 &&
          last_error() == NT_ERROR_ALREADY_EXISTS);
    CHECK(nt_MoveFileExA("DATA/SCORES.TXT", "data/TARGET.TXT", 1) != 0);
    CHECK(nt_DeleteFileA("data/scores.txt") == 0 && last_error() == NT_ERROR_FILE_NOT_FOUND);
    uintptr_t path_handle = nt_CreateFileA("data/TARGET.txt", UINT32_C(0x80000000),
                                            1u | 4u, NULL, 3u, 0x80u, 0);
    CHECK(path_handle != UINTPTR_MAX);
    CHECK(nt_MoveFileExA("data/target.txt", "DATA/TARGET.txt", 0) != 0);
    uint16_t final_path[256] = {0};
    static const uint16_t expected_final[] = u"\\\\?\\C:\\DATA\\TARGET.txt";
    CHECK(nt_GetFinalPathNameByHandleW(path_handle, final_path, 256, 0) ==
          sizeof(expected_final) / sizeof(*expected_final) - 1 &&
          memcmp(final_path, expected_final, sizeof(expected_final)) == 0);
    CHECK(nt_CloseHandle(path_handle) != 0);

    /* Moving a directory also updates names reported by open file handles
     * beneath it without invalidating their original Linux descriptors. */
    CHECK(nt_CreateDirectoryA("data\\nested-move", NULL) != 0);
    void *nested_stream = nt_fopen("data\\nested-move\\file.bin", "wb");
    CHECK(nested_stream != NULL && nt_fwrite("Z", 1, 1, nested_stream) == 1 &&
          nt_fclose(nested_stream) == 0);
    path_handle = nt_CreateFileA("data\\nested-move\\file.bin",
                                 UINT32_C(0x80000000), 1u | 4u, NULL, 3u, 0x80u, 0);
    CHECK(path_handle != UINTPTR_MAX);
    CHECK(nt_MoveFileExA("data\\NESTED-MOVE", "DATA\\MovedDir", 0) != 0);
    static const uint16_t expected_nested[] = u"\\\\?\\C:\\DATA\\MovedDir\\file.bin";
    CHECK(nt_GetFinalPathNameByHandleW(path_handle, final_path, 256, 0) ==
          sizeof(expected_nested) / sizeof(*expected_nested) - 1 &&
          memcmp(final_path, expected_nested, sizeof(expected_nested)) == 0);
    CHECK(nt_CloseHandle(path_handle) != 0);
    CHECK(nt_DeleteFileA("data\\moveddir\\FILE.BIN") != 0 &&
          nt_RemoveDirectoryA("DATA\\MOVEDDIR") != 0);
    char renamed[512];
    CHECK(snprintf(renamed, sizeof(renamed), "%s/data/TARGET.txt", root) > 0);
    CHECK(access(renamed, F_OK) == 0);
    folded = nt_fopen("data/target.txt", "rb");
    CHECK(folded != NULL && nt_fclose(folded) == 0);
    CHECK(nt_RemoveDirectoryA("data") == 0 && last_error() == NT_ERROR_DIR_NOT_EMPTY);
    CHECK(nt_DeleteFileA("DATA/TARGET.TXT") != 0 && nt_RemoveDirectoryA("DATA") != 0);

    /* Windows remembers a separate current directory per DOS drive.
     * A drive-relative D:foo must use D:'s remembered directory even if
     * the active process directory has moved back to C:. */
    char second_drive[] = "/tmp/nt-mines-second-drive.XXXXXX";
    CHECK(mkdtemp(second_drive) != NULL);
    CHECK(nt_runtime_set_drive_root('D', second_drive));
    CHECK(nt_CreateDirectoryA("D:\\games", NULL));
    CHECK(nt_SetCurrentDirectoryA("D:\\games"));
    CHECK(nt_GetCurrentDirectoryA(sizeof(win_cwd), win_cwd) ==
          strlen("D:\\games") && strcmp(win_cwd, "D:\\games") == 0);
    CHECK(nt_GetFullPathNameA("D:save.bin", sizeof(absolute_path),
                              absolute_path, NULL) ==
          strlen("D:\\games\\save.bin") &&
          strcmp(absolute_path, "D:\\games\\save.bin") == 0);
    const uintptr_t drive_handle = nt_CreateFileA("D:save.bin",
        UINT32_C(0x40000000), 7u, NULL, 2u, 0x80u, 0);
    CHECK(drive_handle != UINTPTR_MAX && nt_CloseHandle(drive_handle));
    /* A rooted path without a drive prefix uses the active drive. */
    CHECK(nt_GetFullPathNameA("\\root.dat", sizeof(absolute_path),
                              absolute_path, NULL) ==
          strlen("D:\\root.dat") && strcmp(absolute_path, "D:\\root.dat") == 0);
    CHECK(nt_SetCurrentDirectoryA("C:\\"));
    CHECK(nt_GetFullPathNameA("D:save.bin", sizeof(absolute_path),
                              absolute_path, NULL) ==
          strlen("D:\\games\\save.bin") &&
          strcmp(absolute_path, "D:\\games\\save.bin") == 0);
    CHECK(nt_SetCurrentDirectoryA("D:"));
    CHECK(nt_GetCurrentDirectoryA(sizeof(win_cwd), win_cwd) ==
          strlen("D:\\games") && strcmp(win_cwd, "D:\\games") == 0);
    CHECK(!nt_RemoveDirectoryA("D:\\games") &&
          last_error() == NT_ERROR_SHARING_VIOLATION);
    CHECK(nt_SetCurrentDirectoryA("C:\\"));
    CHECK(nt_GetFullPathNameA("D:..\\..\\escape", sizeof(absolute_path),
                              absolute_path, NULL) == 0);
    CHECK(nt_SetCurrentDirectoryA("D:\\"));
    CHECK(nt_SetCurrentDirectoryA("C:\\"));
    CHECK(nt_DeleteFileA("D:\\games\\save.bin"));
    CHECK(nt_RemoveDirectoryA("D:\\games"));
    CHECK(nt_runtime_set_drive_root('D', NULL));
    CHECK(rmdir(second_drive) == 0);
    CHECK(nt_runtime_set_file_root(NULL) &&
          nt_test_cleanup_share_registry(root) && rmdir(root) == 0);
    puts("PASS scoped files, text and binary I/O, and denied paths");
}

static void *make_thread_context(void *output)
{
    *(uintptr_t *)output = nt_CreateCompatibleDC(0);
    return NULL;
}

static void check_gdi(void)
{
    BOUND(GDI, BitBlt);
    BOUND(GDI, CreateCompatibleBitmap);
    BOUND(GDI, CreateCompatibleDC);
    BOUND(GDI, CreateHatchBrush);
    BOUND(GDI, CreatePen);
    BOUND(GDI, CreateSolidBrush);
    BOUND(GDI, DeleteDC);
    BOUND(GDI, DeleteObject);
    BOUND(GDI, GetStockObject);
    BOUND(GDI, IntersectClipRect);
    BOUND(GDI, Rectangle);
    BOUND(GDI, SelectClipRgn);
    BOUND(GDI, SelectObject);
    BOUND(GDI, SetBkMode);
    BOUND(GDI, SetMapMode);
    BOUND(GDI, SetPixel);
    BOUND(GDI, SetTextColor);

    enum { RED = 0x0000ff, BLUE = 0xff0000, WHITE = 0xffffff, SRCCOPY = 0x00cc0020,
           PATCOPY = 0x00f00021, BLACKNESS = 0x00000042, WHITENESS = 0x00ff0062 };
    const uintptr_t canvas = nt_runtime_create_surface(10, 10);
    const uintptr_t memory = nt_CreateCompatibleDC(canvas);
    const uintptr_t copy = nt_CreateCompatibleDC(0);
    CHECK(canvas != 0 && memory != 0 && copy != 0);
    CHECK(nt_SetPixel(copy, 0, 0, WHITE) == WHITE &&
          nt_SetPixel(copy, 1, 0, WHITE) == UINT32_MAX);
    const uintptr_t bitmap = nt_CreateCompatibleBitmap(canvas, 8, 8);
    const uintptr_t old_bitmap = nt_SelectObject(memory, bitmap);
    CHECK(bitmap != 0 && old_bitmap != 0 && old_bitmap != bitmap);
    CHECK(nt_SelectObject(copy, bitmap) == 0 && last_error() == NT_ERROR_ACCESS_DENIED);
    CHECK(nt_SelectObject(copy, old_bitmap) == 0 && last_error() == NT_ERROR_ACCESS_DENIED);
    CHECK(nt_DeleteObject(bitmap) == 0 && last_error() == NT_ERROR_ACCESS_DENIED);
    CHECK(nt_CreateCompatibleBitmap(canvas, -1, 3) == 0 &&
          last_error() == NT_ERROR_INVALID_PARAMETER);
    CHECK(nt_CreateCompatibleBitmap(canvas, 4096, 4096) == 0 &&
          last_error() == NT_ERROR_NOT_ENOUGH_MEMORY);
    const uintptr_t zero = nt_CreateCompatibleBitmap(canvas, 0, 7);
    const uintptr_t mono_default = nt_SelectObject(copy, zero);
    CHECK(zero != 0 && mono_default != 0 && nt_runtime_read_pixel(copy, 1, 0) == UINT32_MAX);

    const uintptr_t pen = nt_CreatePen(0, 1, RED);
    const uintptr_t brush = nt_CreateSolidBrush(BLUE);
    const uintptr_t stock_pen = nt_GetStockObject(7);
    const uintptr_t stock_brush = nt_GetStockObject(0);
    CHECK(pen != 0 && brush != 0 && stock_pen != 0 && stock_brush != 0);
    CHECK(nt_SelectObject(memory, pen) == stock_pen &&
          nt_SelectObject(memory, brush) == stock_brush);
    CHECK(nt_DeleteObject(brush) == 0 && last_error() == NT_ERROR_ACCESS_DENIED);
    CHECK(nt_Rectangle(memory, 1, 1, 5, 5) != 0);
    CHECK(nt_runtime_read_pixel(memory, 1, 1) == RED &&
          nt_runtime_read_pixel(memory, 4, 4) == RED &&
          nt_runtime_read_pixel(memory, 2, 2) == BLUE &&
          nt_runtime_read_pixel(memory, 5, 5) == 0);
    CHECK(nt_SetMapMode(memory, 1) == 1 && nt_SetMapMode(memory, 8) == 0 &&
          last_error() == NT_ERROR_NOT_SUPPORTED);
    CHECK(nt_IntersectClipRect(memory, 2, 2, 4, 4) == 2 &&
          nt_SetPixel(memory, 1, 1, 0) == UINT32_MAX);
    CHECK(nt_SetPixel(memory, 3, 3, 0) == 0 && nt_runtime_read_pixel(memory, 3, 3) == 0);
    CHECK(nt_IntersectClipRect(memory, 7, 7, 8, 8) == 1 &&
          nt_SetPixel(memory, 3, 3, RED) == UINT32_MAX);
    CHECK(nt_SelectClipRgn(memory, stock_brush) == 0 && last_error() == NT_ERROR_NOT_SUPPORTED);
    CHECK(nt_SelectClipRgn(memory, 0) == 2 && nt_SetPixel(memory, 3, 3, RED) == RED);
    CHECK(nt_BitBlt(canvas, 2, 2, 6, 6, memory, 0, 0, SRCCOPY) != 0);
    CHECK(nt_runtime_read_pixel(canvas, 3, 3) == RED &&
          nt_runtime_read_pixel(canvas, 4, 4) == BLUE &&
          nt_runtime_read_pixel(canvas, 7, 7) == 0);
    CHECK(nt_BitBlt(canvas, 3, 3, 5, 5, canvas, 2, 2, SRCCOPY) != 0);
    CHECK(nt_runtime_read_pixel(canvas, 4, 4) == RED &&
          nt_runtime_read_pixel(canvas, 5, 5) == BLUE);
    CHECK(nt_BitBlt(canvas, 0, 0, 2, 2, memory, 7, 7, SRCCOPY) == 0 &&
          last_error() == NT_ERROR_INVALID_PARAMETER && nt_runtime_read_pixel(canvas, 0, 0) == 0);
    CHECK(nt_BitBlt(canvas, 0, 0, 1, 1, 0, 0, 0, 1) == 0 &&
          last_error() == NT_ERROR_NOT_SUPPORTED);
    CHECK(nt_SetTextColor(canvas, RED) == 0 && nt_SetTextColor(canvas, BLUE) == RED);
    CHECK(nt_BitBlt(canvas, 0, 0, 1, 1, copy, 0, 0, SRCCOPY) != 0 &&
          nt_runtime_read_pixel(canvas, 0, 0) == BLUE);
    CHECK(nt_BitBlt(canvas, 0, 0, 10, 10, 0, 0, 0, BLACKNESS) != 0);
    CHECK(nt_BitBlt(canvas, -1, -1, 2, 2, 0, 0, 0, WHITENESS) != 0 &&
          nt_runtime_read_pixel(canvas, 0, 0) == WHITE && nt_runtime_read_pixel(canvas, 1, 0) == 0);

    const uintptr_t hatch = nt_CreateHatchBrush(0, RED);
    CHECK(hatch != 0 && nt_CreateHatchBrush(6, RED) == 0 &&
          nt_SelectObject(canvas, hatch) == stock_brush);
    CHECK(nt_SetBkMode(canvas, 1) == 2);
    CHECK(nt_BitBlt(canvas, 0, 0, 3, 3, 0, 0, 0, PATCOPY) != 0);
    CHECK(nt_runtime_read_pixel(canvas, 1, 0) == RED && nt_runtime_read_pixel(canvas, 1, 1) == 0);
    CHECK(nt_SetBkMode(canvas, 2) == 1);
    CHECK(nt_BitBlt(canvas, 0, 0, 3, 3, 0, 0, 0, PATCOPY) != 0);
    CHECK(nt_runtime_read_pixel(canvas, 1, 0) == RED &&
          nt_runtime_read_pixel(canvas, 1, 1) == WHITE);
    CHECK(nt_SelectObject(canvas, stock_brush) == hatch && nt_DeleteObject(hatch) != 0);
    CHECK(nt_CreatePen(1, 1, RED) == 0 && last_error() == NT_ERROR_NOT_SUPPORTED);
    CHECK(nt_GetStockObject(19) == 0 && last_error() == NT_ERROR_NOT_SUPPORTED);
    CHECK(nt_DeleteDC(memory) != 0 && nt_DeleteObject(bitmap) != 0 &&
          nt_DeleteObject(pen) != 0 && nt_DeleteObject(brush) != 0);
    CHECK(nt_DeleteObject(old_bitmap) == 0 && last_error() == NT_ERROR_INVALID_HANDLE);
    CHECK(nt_DeleteDC(copy) != 0 && nt_DeleteObject(zero) != 0 &&
          nt_DeleteObject(canvas) != 0);

    pthread_t thread;
    uintptr_t thread_context = 0;
    CHECK(pthread_create(&thread, NULL, make_thread_context, &thread_context) == 0);
    CHECK(pthread_join(thread, NULL) == 0 && thread_context != 0);
    CHECK(nt_DeleteDC(thread_context) == 0 && last_error() == NT_ERROR_INVALID_HANDLE);
    puts("PASS GDI memory objects, clipped drawing, real pixels, overlap and lifetimes");
}

static void check_menus(void)
{
    BOUND(USER, AppendMenuA);
    BOUND(USER, CheckMenuItem);
    BOUND(USER, CreateMenu);
    BOUND(USER, DestroyMenu);
    BOUND(USER, EnableMenuItem);
    BOUND(USER, GetMenuItemCount);

    const uintptr_t main_menu = nt_CreateMenu();
    const uintptr_t game = nt_CreateMenu();
    CHECK(main_menu != 0 && game != 0 && game != main_menu);
    CHECK(nt_GetMenuItemCount(main_menu) == 0);
    CHECK(nt_AppendMenuA(game, 0x08, 20, "&Begin") != 0);
    CHECK(nt_AppendMenuA(game, 0x800, 0, NULL) != 0);
    CHECK(nt_AppendMenuA(game, 0x01, 21, "&Stop") != 0);
    CHECK(nt_AppendMenuA(main_menu, 0x10, game, "&Game") != 0);
    CHECK(nt_GetMenuItemCount(main_menu) == 1 && nt_GetMenuItemCount(game) == 3);
    CHECK(nt_CheckMenuItem(main_menu, 20, 0) == 0x08 &&
          nt_CheckMenuItem(game, 0, 0x408) == 0);
    CHECK(nt_EnableMenuItem(main_menu, 21, 0) == 0x01 &&
          nt_EnableMenuItem(game, 2, 0x402) == 0);
    CHECK(nt_EnableMenuItem(game, 21, 0) == 0x02);
    CHECK(nt_CheckMenuItem(game, 1, 0x408) == UINT32_MAX &&
          last_error() == NT_ERROR_INVALID_PARAMETER);
    CHECK(nt_EnableMenuItem(main_menu, 99, 0) == -1 &&
          last_error() == NT_ERROR_INVALID_PARAMETER);
    CHECK(nt_AppendMenuA(game, 0x10, main_menu, "cycle") == 0 &&
          last_error() == NT_ERROR_INVALID_PARAMETER);
    CHECK(nt_AppendMenuA(game, 0x100, 50, "owner draw") == 0 &&
          last_error() == NT_ERROR_NOT_SUPPORTED);
    CHECK(nt_GetMenuItemCount(game) == 3);
    CHECK(nt_DestroyMenu(game) == 0 && last_error() == NT_ERROR_ACCESS_DENIED);
    CHECK(nt_DestroyMenu(main_menu) != 0 && nt_GetMenuItemCount(game) == -1 &&
          last_error() == NT_ERROR_INVALID_HANDLE);
    CHECK(nt_DestroyMenu(main_menu) == 0 && last_error() == NT_ERROR_INVALID_HANDLE);
    puts("PASS menu state by position and command, ownership and recursive destruction");
}

static uintptr_t timer_callback_id;
static void NT_MS_ABI on_timer(uintptr_t window, uint32_t message, uintptr_t id, uint32_t ticks)
{
    CHECK(window == 0 && message == 0x113 && ticks == nt_GetTickCount());
    timer_callback_id = id;
    nt_PostQuitMessage(19);
}

static void check_messages(void)
{
    BOUND(USER, DispatchMessageA);
    BOUND(USER, GetMessageA);
    BOUND(USER, KillTimer);
    BOUND(USER, PostQuitMessage);
    BOUND(USER, SetTimer);

    struct nt_message message = {.wparam = 77};
    CHECK(nt_GetMessageA(&message, 1, 0, 0) == -1 &&
          last_error() == NT_ERROR_INVALID_HANDLE && message.wparam == 77);
    CHECK(nt_GetMessageA(&message, 0, 0, 0) == -1 &&
          last_error() == NT_ERROR_NOT_SUPPORTED && message.wparam == 77);
    CHECK(nt_SetTimer(1, 0, 10, on_timer) == 0 && last_error() == NT_ERROR_NOT_SUPPORTED);
    const uintptr_t timer = nt_SetTimer(0, 0, 1000, NULL);
    CHECK(timer != 0 && nt_SetTimer(0, timer, 1, on_timer) == timer);
    CHECK(nt_GetMessageA(&message, UINTPTR_MAX, 0x100, 0x115) == 1 &&
          message.window == 0 && message.message == 0x113 && message.wparam == timer &&
          message.lparam == (intptr_t)on_timer && message.time == nt_GetTickCount());
    CHECK(nt_KillTimer(0, timer) != 0);
    CHECK(nt_KillTimer(0, timer) == 0 && last_error() == NT_ERROR_INVALID_HANDLE);
    CHECK(nt_DispatchMessageA(&message) == 0 && timer_callback_id == timer);
    CHECK(nt_GetMessageA(&message, 0, 0x400, 0x500) == 0 &&
          message.message == 0x12 && message.wparam == 19 && message.lparam == 0);
    CHECK(nt_GetMessageA(&message, 0, 0x500, 0x400) == -1 &&
          last_error() == NT_ERROR_INVALID_PARAMETER);
    puts("PASS thread timers, MS ABI callbacks, WM_QUIT and message filters");
}

int main(void)
{
    check_system();
    check_modules();
    check_memory();
    check_unicode_and_rects();
    check_crt();
    check_files();
    check_gdi();
    check_menus();
    check_messages();
    return EXIT_SUCCESS;
}
