/* SPDX-License-Identifier: MIT */
#include <windows.h>
#include <stdint.h>

#ifndef NT_CHILD_FIXTURE
__declspec(dllimport) LONG NTAPI NtTerminateProcess(HANDLE, LONG);
__declspec(dllimport) LONG NTAPI NtSuspendThread(HANDLE, ULONG *);
#endif

#ifdef NT_CHILD_FIXTURE
static BOOL contains_command_marker(const WCHAR *command, const WCHAR *marker)
{
    for (const WCHAR *p = command; *p != 0; ++p) {
        const WCHAR *a = p, *b = marker;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == 0)
            return TRUE;
    }
    return FALSE;
}
static BOOL has_hold_argument(const WCHAR *command)
{
    static const WCHAR marker[] = L"/nt-hold";
    for (const WCHAR *p = command; *p != 0; ++p) {
        const WCHAR *a = p, *b = marker;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == 0)
            return TRUE;
    }
    return FALSE;
}

int import_fixture_entry(void)
{
    const WCHAR *command = GetCommandLineW();
    if (has_hold_argument(command)) {
        Sleep(30000);
        return 0x766u;
    }
    if (contains_command_marker(command, L"/nt-std-pipe")) {
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD flags = 0, sent = 0;
        if (output == NULL || output == INVALID_HANDLE_VALUE ||
            !GetHandleInformation(output, &flags) ||
            (flags & HANDLE_FLAG_INHERIT) == 0 ||
            GetFileType(output) != FILE_TYPE_PIPE ||
            !WriteFile(output, "PIPE_OK", 7, &sent, NULL) || sent != 7)
            return 118;
        return 0x12345678u;
    }
    /* The HANDLE is the guest's numeric token, not a host FD or PID.
     * The child inherits its real Linux open-file description. */
    static const WCHAR prefix[] = L"/nt-file-handle=";
    if (contains_command_marker(command, prefix)) {
        const WCHAR *p = command;
        for (; *p != 0; ++p) {
            const WCHAR *q = p, *name = prefix;
            while (*q && *name && *q == *name) {
                ++q;
                ++name;
            }
            if (*name == 0) {
                uintptr_t handle = 0;
                for (unsigned int digit = 0; digit < 16; ++digit) {
                    WCHAR c = q[digit];
                    unsigned int value = c >= L'0' && c <= L'9' ? (unsigned int)(c - L'0') :
                                         c >= L'A' && c <= L'F' ? (unsigned int)(c - L'A') + 10u : 16u;
                    if (value >= 16u)
                        return 103;
                    handle = (handle << 4) | value;
                }
                DWORD flags = 0;
                if (contains_command_marker(command, L"/nt-file-no-inherit")) {
                    SetLastError(0);
                    return GetHandleInformation((HANDLE)handle, &flags) ||
                           GetLastError() != ERROR_INVALID_HANDLE ? 104 : 0x12345678u;
                }
                if (!GetHandleInformation((HANDLE)handle, &flags) ||
                    (flags & HANDLE_FLAG_INHERIT) == 0)
                    return 105;
                char bytes[6] = {0};
                DWORD read_bytes = 0;
                if (!ReadFile((HANDLE)handle, bytes, 5, &read_bytes, NULL) ||
                    read_bytes != 5 || __builtin_memcmp(bytes, "HELLO", 5) != 0 ||
                    !CloseHandle((HANDLE)handle))
                    return 106;
                return 0x12345678u;
            }
        }
        return 107;
    }
    WCHAR env[80];
    if (contains_command_marker(command, L"/nt-env-custom")) {
        if (GetEnvironmentVariableW(L"NT_CHILD_CUSTOM", env, 80) != 5 ||
            lstrcmpW(env, L"árbol") != 0 ||
            GetEnvironmentVariableW(L"NT_CHILD_INHERITED", env, 80) != 0 ||
            GetLastError() != ERROR_ENVVAR_NOT_FOUND)
            return 96;
    } else {
        if (GetEnvironmentVariableW(L"NT_CHILD_INHERITED", env, 80) != 5 ||
            lstrcmpW(env, L"Grüße") != 0)
            return 97;
    }
    BOOL found = FALSE;
    for (const WCHAR *p = command; *p; ++p)
        if (*p == 0xe4)
            found = TRUE;
    WCHAR directory[32];
    if (!found || GetCurrentDirectoryW(32, directory) != 11 ||
        directory[0] != L'C' || directory[3] != L'C')
        return 91;
    return 0x12345678;
}
#else
int import_fixture_entry(void)
{
    WCHAR command[] = L"\"C:\\Test child.exe\" \x00e4";
    const WCHAR directory[] = L"C:\\ChildDir";
    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process = {0};
    if (!SetEnvironmentVariableW(L"NT_CHILD_INHERITED", L"Grüße"))
        return 98;
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process))
        return 20;
    DWORD code = 0;
    if (GetProcessId(process.hProcess) != process.dwProcessId ||
        GetProcessIdOfThread(process.hThread) != process.dwProcessId ||
        WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(process.hThread, 0) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x12345678)
        return 21;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (CreateProcessW(L"C:\\ChildDir", NULL, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process) ||
        GetLastError() != ERROR_FILE_NOT_FOUND)
        return 22;

    /* The child receives a private startup gate: not even its PE entry
     * executes until the initial thread's suspension count reaches zero. */
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_SUSPENDED,
                        NULL, directory, &startup, &process))
        return 31;
    if (WaitForSingleObject(process.hProcess, 0) != WAIT_TIMEOUT ||
        GetProcessIdOfThread(process.hThread) != process.dwProcessId)
        return 32;
    ULONG prior = 0;
    if (NtSuspendThread(process.hThread, &prior) != 0 || prior != 1 ||
        ResumeThread(process.hThread) != 2 ||
        WaitForSingleObject(process.hThread, 0) != WAIT_TIMEOUT ||
        ResumeThread(process.hThread) != 1 ||
        WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x12345678u)
        return 33;
    if (!CloseHandle(process.hThread) || !CloseHandle(process.hProcess))
        return 34;

    WCHAR hold[] = L"\"C:\\Test child.exe\" /nt-hold";
    if (!CreateProcessW(NULL, hold, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process))
        return 23;
    if (GetProcessId(process.hProcess) != process.dwProcessId ||
        GetProcessIdOfThread(process.hThread) != process.dwProcessId ||
        GetProcessId(GetCurrentProcess()) != GetCurrentProcessId())
        return 24;
    SetLastError(0x770u);
    if (NtTerminateProcess(process.hProcess, (LONG)0x12345678u) != 0 ||
        GetLastError() != 0x770u ||
        WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(process.hThread, 0) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x12345678)
        return 25;
    if (!CloseHandle(process.hThread) || !CloseHandle(process.hProcess))
        return 26;

    if (!CreateProcessW(NULL, hold, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process))
        return 27;
    if (!TerminateProcess(process.hProcess, 0x98765432u) ||
        WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x98765432u)
        return 28;
    if (!CloseHandle(process.hThread) || !CloseHandle(process.hProcess))
        return 29;

    /* The custom environment replaces, rather than extends, the inherited
     * guest snapshot. The Linux runner's host envp remains untouched. */
    WCHAR custom_command[] = L"\"C:\\Test child.exe\" /nt-env-custom ä";
    static const WCHAR environment[] =
        L"NT_CHILD_CUSTOM=árbol\0NT_CHILD_OTHER=ok\0";
    if (!CreateProcessW(NULL, custom_command, NULL, NULL, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, (void *)environment,
                        directory, &startup, &process))
        return 99;
    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x12345678u ||
        !CloseHandle(process.hThread) || !CloseHandle(process.hProcess))
        return 100;
    SetLastError(0);
    if (CreateProcessW(NULL, custom_command, NULL, NULL, FALSE, 0,
                       (void *)environment, directory, &startup, &process) ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 101;
    /* A regular file can be inherited with its Windows HANDLE identity,
     * rights and shared offset. Inheritance disabled must not leak it. */
    static const WCHAR file_path[] = L"C:\\ChildDir\\nt-stage4-inherit.bin";
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    HANDLE shared = CreateFileW(file_path, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (shared == INVALID_HANDLE_VALUE)
        return 108;
    DWORD written = 0;
    if (!WriteFile(shared, "HELLO", 5, &written, NULL) || written != 5 ||
        !SetFilePointerEx(shared, (LARGE_INTEGER){.QuadPart = 0}, NULL, FILE_BEGIN))
        return 109;
    WCHAR command_with_handle[128] = L"\"C:\\Test child.exe\" /nt-file-handle=";
    size_t at = 0;
    while (command_with_handle[at])
        ++at;
    static const WCHAR hex[] = L"0123456789ABCDEF";
    uintptr_t token = (uintptr_t)shared;
    for (int digit = 15; digit >= 0; --digit)
        command_with_handle[at++] = hex[(token >> (unsigned)(digit * 4)) & 15u];
    command_with_handle[at] = 0;
    if (!CreateProcessW(NULL, command_with_handle, NULL, NULL, TRUE, 0, NULL,
                        directory, &startup, &process))
        return 110;
    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x12345678u ||
        !CloseHandle(process.hThread) || !CloseHandle(process.hProcess))
        return 111;
    LARGE_INTEGER position;
    if (!SetFilePointerEx(shared, (LARGE_INTEGER){.QuadPart = 0}, &position, FILE_CURRENT) ||
        position.QuadPart != 5)
        return 112;
    static const WCHAR no_suffix[] = L" /nt-file-no-inherit";
    for (size_t i = 0; no_suffix[i]; ++i)
        command_with_handle[at++] = no_suffix[i];
    command_with_handle[at] = 0;
    if (!CreateProcessW(NULL, command_with_handle, NULL, NULL, FALSE, 0, NULL,
                        directory, &startup, &process))
        return 113;
    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x12345678u ||
        !CloseHandle(process.hThread) || !CloseHandle(process.hProcess))
        return 114;
    if (!CloseHandle(shared) || !DeleteFileW(file_path))
        return 115;
    HANDLE unshareable = CreateEventW(&security, TRUE, FALSE, NULL);
    if (unshareable == NULL)
        return 116;
    SetLastError(0);
    if (CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL,
                       directory, &startup, &process) ||
        GetLastError() != ERROR_NOT_SUPPORTED || !CloseHandle(unshareable))
        return 117;
    /* STARTF_USESTDHANDLES must point to truly inherited Win32 pipe
     * handles, not to invented Linux stdout/stderr pseudo values. */
    HANDLE read_pipe = NULL, write_pipe = NULL;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
        GetFileType(read_pipe) != FILE_TYPE_PIPE)
        return 119;
    STARTUPINFOW redirected = startup;
    redirected.dwFlags = STARTF_USESTDHANDLES;
    redirected.hStdInput = read_pipe;
    redirected.hStdOutput = write_pipe;
    redirected.hStdError = write_pipe;
    WCHAR pipe_command[] = L"\"C:\\Test child.exe\" /nt-std-pipe ä";
    if (!CreateProcessW(NULL, pipe_command, NULL, NULL, TRUE, 0, NULL,
                        directory, &redirected, &process))
        return 120;
    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x12345678u ||
        !CloseHandle(process.hThread) || !CloseHandle(process.hProcess))
        return 121;
    if (!CloseHandle(write_pipe))
        return 122;
    char output[8] = {0};
    DWORD output_size = 0;
    if (!ReadFile(read_pipe, output, 7, &output_size, NULL) || output_size != 7 ||
        __builtin_memcmp(output, "PIPE_OK", 7) != 0)
        return 123;
    SetLastError(0);
    if (CreateProcessW(NULL, pipe_command, NULL, NULL, FALSE, 0, NULL,
                       directory, &redirected, &process) ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 124;
    if (!CloseHandle(read_pipe))
        return 125;
    if (!SetEnvironmentVariableW(L"NT_CHILD_INHERITED", NULL))
        return 102;
    return 0;
}
#endif
