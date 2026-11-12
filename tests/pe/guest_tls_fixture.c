/* SPDX-License-Identifier: MIT */
/* Static imports must initialize DLL TLS before process callbacks and before
 * each newly created guest thread receives DLL_THREAD_ATTACH. */
#include <windows.h>
#include <stdint.h>
__declspec(dllimport) uint32_t WINAPI GuestTlsRead(void);
__declspec(dllimport) uint32_t WINAPI GuestTlsWrite(uint32_t value);
__declspec(dllimport) uint32_t WINAPI GuestTlsEvents(void);

static DWORD WINAPI tls_worker(void *ignored)
{
    (void)ignored;
    if (GuestTlsRead() != 17 || GuestTlsWrite(99) != 99 || GuestTlsRead() != 99)
        return 11;
    return 0x144;
}

int import_fixture_entry(void)
{
    if (GuestTlsRead() != 17 || GuestTlsEvents() != 0)
        return 10;
    if (GuestTlsWrite(42) != 42 || GuestTlsRead() != 42)
        return 11;
    HANDLE worker = CreateThread(NULL, 0, tls_worker, NULL, 0, NULL);
    DWORD exit_code = 0;
    if (worker == NULL || WaitForSingleObject(worker, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(worker, &exit_code) || exit_code != 0x144 ||
        !CloseHandle(worker))
        return 12;
    if (GuestTlsRead() != 42)
        return 13;
    /* One attached and detached child, with a callback before DllMain. */
    if (GuestTlsEvents() != 0x01010101u)
        return 14;
    return 0;
}
