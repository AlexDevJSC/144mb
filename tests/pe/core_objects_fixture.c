/* SPDX-License-Identifier: MIT */
/* Freestanding PE32+ regression for local NT semaphore and thread objects. */
#include <windows.h>
#include <stdint.h>

#define ST_OK ((LONG)0)
#define ST_OBJECT_EXISTS ((LONG)0x40000000u)
#define ST_TYPE_MISMATCH ((LONG)0xc0000024u)
#define ST_ACCESS_DENIED ((LONG)0xc0000022u)
#define ST_INFO_LENGTH_MISMATCH ((LONG)0xc0000004u)
#define ST_INVALID_INFO_CLASS ((LONG)0xc0000003u)
#define ST_SEMAPHORE_LIMIT ((LONG)0xc0000047u)
#define ST_INVALID_HANDLE ((LONG)0xc0000008u)
#define QUERY_LIMITED_INFORMATION 0x800u
#define QUERY_INFORMATION 0x40u
#define THREAD_SUSPEND_RESUME_ACCESS 2u

struct win_unicode_string {
    USHORT length, maximum;
    WCHAR *buffer;
};
struct win_object_attributes {
    ULONG length;
    uintptr_t root;
    const struct win_unicode_string *name;
    ULONG flags;
    const void *security, *qos;
};
struct win_thread_basic {
    LONG status;
    void *teb;
    struct { uintptr_t process, thread; } id;
    uintptr_t affinity;
    LONG priority, base_priority;
};
struct win_semaphore_basic { LONG count, maximum; };
__declspec(dllimport) LONG NTAPI NtCreateSemaphore(HANDLE *, ACCESS_MASK,
    const struct win_object_attributes *, LONG, LONG);
__declspec(dllimport) LONG NTAPI NtOpenSemaphore(HANDLE *, ACCESS_MASK,
    const struct win_object_attributes *);
__declspec(dllimport) LONG NTAPI NtReleaseSemaphore(HANDLE, LONG, LONG *);
__declspec(dllimport) LONG NTAPI NtQuerySemaphore(HANDLE, ULONG, void *, ULONG, ULONG *);
__declspec(dllimport) LONG NTAPI NtQueryInformationThread(HANDLE, ULONG, void *, ULONG, ULONG *);
struct win_client_id { uintptr_t process, thread; };
__declspec(dllimport) LONG NTAPI NtOpenThread(HANDLE *, ACCESS_MASK,
    const struct win_object_attributes *, const struct win_client_id *);

static DWORD WINAPI stopped_thread(void *context)
{
    volatile DWORD *flag = context;
    *flag = 1;
    return 0x144u;
}

static int test_semaphores(void)
{
    static WCHAR local_name[] = L"Local\\NtSemFixture";
    const struct win_unicode_string unicode = {
        sizeof(local_name) - sizeof(WCHAR), sizeof(local_name), local_name
    };
    const struct win_object_attributes attrs = {sizeof(attrs), 0, &unicode, 0x40, NULL, NULL};
    HANDLE sem = CreateSemaphoreW(NULL, 2, 3, local_name);
    if (sem == NULL || GetLastError() != ERROR_SUCCESS)
        return 10;

    struct win_semaphore_basic info = {-1, -1};
    ULONG returned = 0;
    SetLastError(0x744u);
    if (NtQuerySemaphore(sem, 0, &info, sizeof(info), &returned) != ST_OK ||
        returned != sizeof(info) || info.count != 2 || info.maximum != 3 ||
        GetLastError() != 0x744u)
        return 11;
    HANDLE duplicate = CreateSemaphoreW(NULL, 0, 10, L"local\\ntsemfixture");
    if (duplicate == NULL || duplicate == sem || GetLastError() != ERROR_ALREADY_EXISTS)
        return 12;

    if (WaitForSingleObject(sem, 0) != WAIT_OBJECT_0 ||
        WaitForSingleObject(duplicate, 0) != WAIT_OBJECT_0 ||
        WaitForSingleObject(sem, 0) != WAIT_TIMEOUT)
        return 13;

    LONG previous = -1;
    if (NtReleaseSemaphore(duplicate, 3, &previous) != ST_OK ||
        previous != 0 ||
        NtQuerySemaphore(sem, 0, &info, sizeof(info), &returned) != ST_OK ||
        info.count != 3)
        return 14;
    if (NtReleaseSemaphore(sem, 1, &previous) != ST_SEMAPHORE_LIMIT ||
        previous != 0 || NtQuerySemaphore(sem, 0, &info, sizeof(info), NULL) != ST_OK ||
        info.count != 3)
        return 15;

    HANDLE opened = OpenSemaphoreW(SYNCHRONIZE, FALSE, L"LOCAL\\NtSemFixture");
    if (opened == NULL || WaitForSingleObject(opened, 0) != WAIT_OBJECT_0 ||
        NtReleaseSemaphore(opened, 1, NULL) != ST_ACCESS_DENIED ||
        NtQuerySemaphore(opened, 0, &info, sizeof(info), NULL) != ST_ACCESS_DENIED)
        return 16;
    if (NtQuerySemaphore(sem, 0, &info, 4, &returned) != ST_INFO_LENGTH_MISMATCH ||
        returned != 8 || NtQuerySemaphore(sem, 21, &info, sizeof(info), NULL) != ST_INVALID_INFO_CLASS)
        return 17;

    HANDLE native = NULL;
    if (NtOpenSemaphore(&native, SYNCHRONIZE, &attrs) != ST_OK || native == NULL ||
        NtReleaseSemaphore(native, 1, NULL) != ST_ACCESS_DENIED)
        return 18;

    HANDLE wrong = CreateEventW(NULL, TRUE, FALSE, L"Local\\NtSemFixture");
    if (wrong != NULL || GetLastError() != ERROR_INVALID_HANDLE)
        return 19;
    if (!CloseHandle(sem) || !CloseHandle(duplicate) ||
        !CloseHandle(opened) || !CloseHandle(native))
        return 20;

    if (OpenSemaphoreW(SYNCHRONIZE, FALSE, local_name) != NULL ||
        GetLastError() != ERROR_FILE_NOT_FOUND)
        return 21;

    HANDLE one = CreateSemaphoreW(NULL, 1, 2, NULL);
    HANDLE two = CreateSemaphoreW(NULL, 1, 2, NULL);
    HANDLE both[] = {one, two};
    if (one == NULL || two == NULL ||
        WaitForMultipleObjects(2, both, TRUE, 0) != WAIT_OBJECT_0 ||
        WaitForSingleObject(one, 0) != WAIT_TIMEOUT ||
        WaitForSingleObject(two, 0) != WAIT_TIMEOUT)
        return 22;
    if (!CloseHandle(one) || !CloseHandle(two))
        return 23;
    /* Successful Win32 semaphore calls may reset LastError. Native
     * NTSTATUS calls must preserve its value independently of history. */
    SetLastError(0x746u);
    if (NtCreateSemaphore(&native, 0x1f0003, &attrs, -1, 2) != (LONG)0xc000000du ||
        GetLastError() != 0x746u)
        return 24;
    return 0;
}

static int test_thread_handles(void)
{
    volatile DWORD ran = 0;
    DWORD created_id = 0;
    HANDLE original = CreateThread(NULL, 0, stopped_thread, (void *)&ran,
                                   CREATE_SUSPENDED, &created_id);
    if (original == NULL || created_id == 0 || ran != 0 ||
        GetThreadId(original) != created_id ||
        GetProcessId(GetCurrentProcess()) != GetCurrentProcessId() ||
        GetProcessIdOfThread(GetCurrentThread()) != GetCurrentProcessId() ||
        GetProcessIdOfThread(original) != GetCurrentProcessId())
        return 30;
    HANDLE alias = OpenThread(QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                              FALSE, created_id);
    if (alias == NULL || alias == original || GetThreadId(alias) != created_id ||
        GetProcessIdOfThread(alias) != GetCurrentProcessId())
        return 31;
    const struct win_client_id ids = {GetCurrentProcessId(), created_id};
    const struct win_object_attributes attributes = {sizeof(attributes), 0, NULL, 0, NULL, NULL};
    HANDLE native = NULL;
    SetLastError(0x7abcu);
    if (NtOpenThread(&native, QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                     &attributes, &ids) != ST_OK || native == NULL ||
        native == original || native == alias || GetThreadId(native) != created_id ||
        GetLastError() != 0x7abcu)
        return 39;
    if (NtOpenThread(NULL, QUERY_LIMITED_INFORMATION, &attributes, &ids) !=
        (LONG)0xc0000005u || GetLastError() != 0x7abcu)
        return 40;
    struct win_client_id remote = {GetCurrentProcessId() + 123u, created_id};
    if (NtOpenThread(&native, QUERY_LIMITED_INFORMATION, &attributes, &remote) !=
        (LONG)0xc00000bbu || GetLastError() != 0x7abcu)
        return 41;
    HANDLE no_query = OpenThread(THREAD_SUSPEND_RESUME_ACCESS, FALSE, created_id);
    SetLastError(0);
    if (no_query == NULL || GetProcessIdOfThread(no_query) != 0 ||
        GetLastError() != ERROR_ACCESS_DENIED || !CloseHandle(no_query))
        return 42;

    typedef HRESULT (WINAPI *set_thread_description_fn)(HANDLE, PCWSTR);
    typedef HRESULT (WINAPI *get_thread_description_fn)(HANDLE, PWSTR *);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    set_thread_description_fn set_description =
        (set_thread_description_fn)GetProcAddress(kernel, "SetThreadDescription");
    get_thread_description_fn get_description =
        (get_thread_description_fn)GetProcAddress(kernel, "GetThreadDescription");
    static const WCHAR full_name[] = L"Nombre-\\u00e4-01234567890123456789";
    PWSTR name_copy = NULL;
    if (set_description == NULL || get_description == NULL ||
        set_description(alias, full_name) != (HRESULT)0x80070005u ||
        set_description(original, full_name) != S_OK ||
        get_description(alias, &name_copy) != S_OK || name_copy == NULL)
        return 43;
    for (unsigned int i = 0; i < sizeof(full_name) / sizeof(full_name[0]); ++i)
        if (name_copy[i] != full_name[i])
            return 44;
    if (LocalFree(name_copy) != NULL)
        return 45;

    struct win_thread_basic basic = {0};
    ULONG returned = 0;
    SetLastError(0x755);
    if (NtQueryInformationThread(alias, 0, &basic, sizeof(basic), &returned) != ST_OK ||
        returned != sizeof(basic) || basic.teb == NULL ||
        basic.id.thread != created_id || basic.id.process != GetCurrentProcessId() ||
        basic.status != 259 || GetLastError() != 0x755)
        return 32;

    if (NtQueryInformationThread(alias, 0, &basic, 2, &returned) != ST_INFO_LENGTH_MISMATCH ||
        returned != sizeof(basic) ||
        NtQueryInformationThread(alias, 19, &basic, sizeof(basic), NULL) != ST_INVALID_INFO_CLASS)
        return 33;

    if (ResumeThread(alias) != 0xffffffffu || GetLastError() != ERROR_ACCESS_DENIED)
        return 34;
    if (ResumeThread(original) != 1 ||
        WaitForSingleObject(alias, 5000) != WAIT_OBJECT_0 || ran != 1)
        return 35;
    if (NtQueryInformationThread(alias, 0, &basic, sizeof(basic), NULL) != ST_OK ||
        basic.status != 0x144 || basic.teb != NULL)
        return 36;
    name_copy = NULL;
    if (get_description(native, &name_copy) != S_OK || name_copy == NULL)
        return 46;
    for (unsigned int i = 0; i < sizeof(full_name) / sizeof(full_name[0]); ++i)
        if (name_copy[i] != full_name[i])
            return 47;
    if (LocalFree(name_copy) != NULL)
        return 48;
    if (!CloseHandle(original) || !CloseHandle(alias) || !CloseHandle(native))
        return 37;

    if (GetThreadId((HANDLE)(uintptr_t)15) != 0 ||
        GetLastError() != ERROR_INVALID_HANDLE)
        return 38;
    return 0;
}

int import_fixture_entry(void)
{
    int result = test_semaphores();
    return result ? result : test_thread_handles();
}
