/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "compat.h"
#include "winfile.h"

#include <errno.h>
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
    BOUND(KERNEL, GetCurrentDirectoryA);
    BOUND(KERNEL, SetCurrentDirectoryA);
    BOUND(KERNEL, GetFullPathNameA);
    BOUND(KERNEL, GetFileAttributesA);
    BOUND(KERNEL, GetFileAttributesExA);
    BOUND(KERNEL, GetTempPathA);
    CRT_BOUND("stdio", "_set_fmode", nt_set_fmode);
    CRT_BOUND("stdio", "fopen", nt_fopen);
    CRT_BOUND("stdio", "fclose", nt_fclose);
    CRT_BOUND("stdio", "fread", nt_fread);
    CRT_BOUND("stdio", "fwrite", nt_fwrite);

    CHECK(nt_CreateDirectoryA("data", NULL) == 0 && last_error() == NT_ERROR_PATH_NOT_FOUND);
    char root[] = "/tmp/nt-mines-drive.XXXXXX";
    CHECK(mkdtemp(root) != NULL && nt_runtime_set_file_root(root));
    CHECK(nt_CreateDirectoryA("C:\\data", NULL) != 0);
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
    CHECK(nt_CopyFileA("C:\\DATA\\SCORES.TXT", "C:\\data\\Backup.TXT", 1) != 0);
    CHECK(nt_CopyFileA("data/scores.txt", "DATA/backup.txt", 1) == 0 &&
          last_error() == NT_ERROR_FILE_EXISTS);
    CHECK(nt_CopyFileA("data/SCORES.TXT", "DATA/BACKUP.txt", 0) != 0);
    CHECK(nt_CopyFileA("data/scores.txt", "DATA/SCORES.TXT", 0) == 0);
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
    CHECK(nt_MoveFileExA("data/target.txt", "DATA/TARGET.txt", 0) != 0);
    char renamed[512];
    CHECK(snprintf(renamed, sizeof(renamed), "%s/data/TARGET.txt", root) > 0);
    CHECK(access(renamed, F_OK) == 0);
    folded = nt_fopen("data/target.txt", "rb");
    CHECK(folded != NULL && nt_fclose(folded) == 0);
    CHECK(nt_RemoveDirectoryA("data") == 0 && last_error() == NT_ERROR_DIR_NOT_EMPTY);
    CHECK(nt_DeleteFileA("DATA/TARGET.TXT") != 0 && nt_RemoveDirectoryA("DATA") != 0);
    CHECK(nt_runtime_set_file_root(NULL) && rmdir(root) == 0);
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
