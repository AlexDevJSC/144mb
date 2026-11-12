/* SPDX-License-Identifier: MIT */
/* Freestanding PE32+ tests a process-local Windows event object namespace. */
#include <windows.h>
#include <stdint.h>

#define NT_OK ((LONG)0)
#define NT_OBJECT_NAME_EXISTS ((LONG)0x40000000u)
#define NT_OBJECT_NAME_NOT_FOUND ((LONG)0xc0000034u)
#define NT_ACCESS_DENIED ((LONG)0xc0000022u)
#define NT_INVALID_INFO_CLASS ((LONG)0xc0000003u)

struct guest_unicode_string {
    uint16_t length, capacity;
    const uint16_t *buffer;
};
struct guest_object_attributes {
    uint32_t size;
    uintptr_t root;
    const struct guest_unicode_string *name;
    uint32_t attributes;
    const void *security, *qos;
};
struct event_basic_info {
    uint32_t type;
    int32_t state;
};

__declspec(dllimport) LONG NTAPI NtCreateEvent(HANDLE *, DWORD,
                                                 const struct guest_object_attributes *,
                                                 ULONG, BOOLEAN);
__declspec(dllimport) LONG NTAPI NtOpenEvent(HANDLE *, DWORD,
                                               const struct guest_object_attributes *);
__declspec(dllimport) LONG NTAPI NtQueryEvent(HANDLE, ULONG, void *, ULONG, ULONG *);

int import_fixture_entry(void)
{
    const wchar_t absolute[] = L"\\BaseNamedObjects\\shaRedFixture";
    const struct guest_unicode_string name = {
        (uint16_t)(sizeof(absolute) - sizeof(wchar_t)),
        (uint16_t)sizeof(absolute), (const uint16_t *)absolute
    };
    const struct guest_object_attributes attr = {
        sizeof(attr), 0, &name, 0x40, NULL, NULL
    };
    if (sizeof(attr) != 48)
        return 5;

    SetLastError(0x777);
    HANDLE first = CreateEventW(NULL, TRUE, FALSE, L"Local\\SharedFixture");
    if (first == NULL || GetLastError() != ERROR_SUCCESS)
        return 10;
    HANDLE duplicate = CreateEventW(NULL, FALSE, TRUE, L"local\\SHAREDFIXTURE");
    if (duplicate == NULL || duplicate == first || GetLastError() != ERROR_ALREADY_EXISTS)
        return 11;
    if (WaitForSingleObject(duplicate, 0) != WAIT_TIMEOUT)
        return 12;

    struct event_basic_info info = {0};
    ULONG written = 0;
    if (NtQueryEvent(duplicate, 0, &info, sizeof(info), &written) != NT_OK ||
        written != sizeof(info) || info.type != 0 || info.state != 0)
        return 13;
    if (NtQueryEvent(first, 111, &info, sizeof(info), NULL) != NT_INVALID_INFO_CLASS)
        return 14;

    HANDLE native = NULL;
    if (NtOpenEvent(&native, EVENT_MODIFY_STATE | SYNCHRONIZE, &attr) != NT_OK ||
        native == NULL || native == first)
        return 15;
    if (NtQueryEvent(native, 0, &info, sizeof(info), NULL) != NT_ACCESS_DENIED)
        return 16;
    if (!SetEvent(native) || WaitForSingleObject(first, 0) != WAIT_OBJECT_0 ||
        NtQueryEvent(first, 0, &info, sizeof(info), NULL) != NT_OK ||
        info.state != 1)
        return 17;

    if (!CloseHandle(first) || !CloseHandle(duplicate))
        return 18;
    HANDLE reopened = OpenEventW(EVENT_ALL_ACCESS, FALSE, L"LOCAL\\SHAREDFIXTURE");
    if (reopened == NULL || WaitForSingleObject(reopened, 0) != WAIT_OBJECT_0)
        return 19;
    if (!CloseHandle(native) || !CloseHandle(reopened))
        return 20;
    if (OpenEventW(EVENT_ALL_ACCESS, FALSE, L"Local\\SharedFixture") != NULL ||
        GetLastError() != ERROR_FILE_NOT_FOUND ||
        NtOpenEvent(&native, EVENT_ALL_ACCESS, &attr) != NT_OBJECT_NAME_NOT_FOUND)
        return 21;

    HANDLE again = NULL;
    if (NtCreateEvent(&again, EVENT_ALL_ACCESS, &attr, 1, TRUE) != NT_OK ||
        again == NULL || WaitForSingleObject(again, 0) != WAIT_OBJECT_0)
        return 22;
    HANDLE existing = NULL;
    if (NtCreateEvent(&existing, EVENT_ALL_ACCESS, &attr, 0, FALSE) != NT_OBJECT_NAME_EXISTS ||
        existing == NULL || existing == again)
        return 23;
    if (!CloseHandle(existing) || !CloseHandle(again))
        return 24;
    if (CreateEventW(NULL, TRUE, FALSE, L"Global\\CrossSession") != NULL ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 25;
    return 0;
}
