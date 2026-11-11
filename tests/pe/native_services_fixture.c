/* SPDX-License-Identifier: MIT */
#define _WIN32_WINNT 0x0a00
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <commdlg.h>
#include <commctrl.h>
#include <imm.h>
#include <objbase.h>
#include <shellapi.h>

static const unsigned char seed[8] __attribute__((section(".tls"), used)) = {41, 29};
volatile DWORD _tls_index = 0xffffffffu;
static volatile LONG process_tls, thread_tls;

static unsigned char *fixture_tls(void)
{
    ULONG_PTR *vector;
    __asm__ volatile("movq %%gs:0x58, %0" : "=r"(vector));
    return _tls_index == 0 ? (unsigned char *)vector[_tls_index] : NULL;
}

static void WINAPI fixture_tls_callback(void *module, DWORD reason, void *reserved)
{
    (void)module;
    (void)reserved;
    unsigned char *tls = fixture_tls();
    if (reason == DLL_PROCESS_ATTACH)
        process_tls = tls != NULL && tls[0] == 41 && tls[8] == 0;
    else if (reason == DLL_THREAD_ATTACH)
        thread_tls = tls != NULL && tls[0] == 41 && tls[11] == 0;
    else if (reason == DLL_THREAD_DETACH)
        thread_tls |= 2;
}

static const PIMAGE_TLS_CALLBACK fixture_callbacks[] = {fixture_tls_callback, NULL};
const IMAGE_TLS_DIRECTORY64 _tls_used __attribute__((section(".rdata$T"), used)) = {
    (ULONG_PTR)seed, (ULONG_PTR)(seed + sizeof(seed)), (ULONG_PTR)&_tls_index,
    (ULONG_PTR)fixture_callbacks, 4, 0x00500000
};

static BOOL equals(const WCHAR *a, const WCHAR *b)
{
    while (*a != 0 && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static BOOL bytes_equal(const void *a, const void *b, SIZE_T size)
{
    const unsigned char *left = a, *right = b;
    for (SIZE_T i = 0; i < size; ++i) {
        if (left[i] != right[i])
            return FALSE;
    }
    return TRUE;
}

static int check_files(void)
{
    if (!CreateDirectoryW(L"C:\\fixtures", NULL) || !SetCurrentDirectoryW(L"C:\\fixtures"))
        return 11;
    WCHAR path[260];
    WCHAR *leaf = NULL;
    if (!GetCurrentDirectoryW(260, path) || !equals(path, L"C:\\fixtures") ||
        !GetFullPathNameW(L"avión.bin", 260, path, &leaf) || !equals(leaf, L"avión.bin"))
        return 12;
    HANDLE file = CreateFileW(L"avión.bin", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                              NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 13;
    DWORD done;
    LARGE_INTEGER size, position;
    const LARGE_INTEGER zero = {.QuadPart = 0}, two = {.QuadPart = 2};
    char bytes[4];
    FILE_STANDARD_INFO info;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!WriteFile(file, "data", 4, &done, NULL) || done != 4 ||
        !FlushFileBuffers(file) || GetFileType(file) != FILE_TYPE_DISK ||
        !GetFileSizeEx(file, &size) || size.QuadPart != 4 ||
        !SetFilePointerEx(file, zero, &position, FILE_BEGIN) || position.QuadPart != 0 ||
        !ReadFile(file, bytes, 4, &done, NULL) || done != 4 || !bytes_equal(bytes, "data", 4) ||
        !GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof(info)) ||
        info.EndOfFile.QuadPart != 4 ||
        !GetFileAttributesExW(L"avión.bin", GetFileExInfoStandard, &attributes) ||
        attributes.nFileSizeLow != 4 ||
        !GetFinalPathNameByHandleW(file, path, 260, VOLUME_NAME_DOS) ||
        !equals(path, L"\\\\?\\C:\\fixtures\\avión.bin"))
        return 14;
    if (!SetFilePointerEx(file, two, NULL, FILE_BEGIN) || !SetEndOfFile(file) ||
        !GetFileSizeEx(file, &size) || size.QuadPart != 2)
        return 15;
    FILE_END_OF_FILE_INFO truncation = {.EndOfFile.QuadPart = 1};
    if (!SetFileInformationByHandle(file, FileEndOfFileInfo, &truncation, sizeof(truncation)) ||
        !CloseHandle(file))
        return 16;
    CREATEFILE2_EXTENDED_PARAMETERS params = {.dwSize = sizeof(params), .dwFileAttributes = FILE_ATTRIBUTE_NORMAL};
    file = CreateFile2(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &params);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) || size.QuadPart != 1 ||
        !CloseHandle(file) || !MoveFileExW(L"avión.bin", L"avión.dat", 0))
        return 17;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileExW(L"*.DAT", FindExInfoBasic, &entry, FindExSearchNameMatch, NULL, 0);
    if (find == INVALID_HANDLE_VALUE || !equals(entry.cFileName, L"avión.dat") ||
        FindNextFileW(find, &entry) || GetLastError() != ERROR_NO_MORE_FILES || !FindClose(find))
        return 18;
    if (GetFileAttributesW(L"avión.dat") == INVALID_FILE_ATTRIBUTES || !DeleteFileW(L"avión.dat") ||
        !SetCurrentDirectoryW(L"C:\\") || !RemoveDirectoryW(L"C:\\fixtures"))
        return 19;
    return 0;
}

static int check_memory_environment(void)
{
    HANDLE heap = GetProcessHeap();
    unsigned char *memory = HeapAlloc(heap, HEAP_ZERO_MEMORY, 8);
    if (memory == NULL || memory[3] != 0 || HeapSize(heap, 0, memory) != 8)
        return 21;
    memory[3] = 19;
    memory = HeapReAlloc(heap, HEAP_ZERO_MEMORY, memory, 12);
    if (memory == NULL || memory[3] != 19 || memory[10] != 0 || !HeapFree(heap, 0, memory))
        return 22;
    if (!SetEnvironmentVariableW(L"NT_FIXTURE_VAR", L"í") ||
        GetEnvironmentVariableW(L"nt_fixture_var", NULL, 0) != 2)
        return 23;
    WCHAR wide[8];
    char ansi[8];
    if (GetEnvironmentVariableW(L"NT_FIXTURE_VAR", wide, 8) != 1 || wide[0] != 0xed ||
        GetEnvironmentVariableA("NT_FIXTURE_VAR", ansi, 8) != 1 || (unsigned char)ansi[0] != 0xed ||
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, ansi, 8, NULL, NULL) != 3 ||
        (unsigned char)ansi[0] != 0xc3 || (unsigned char)ansi[1] != 0xad)
        return 24;
    WCHAR *environment = GetEnvironmentStringsW();
    if (environment == NULL || !FreeEnvironmentStringsW(environment) ||
        !SetEnvironmentVariableW(L"NT_FIXTURE_VAR", NULL))
        return 25;
    int count;
    WCHAR **argv = CommandLineToArgvW(L"\"C:\\My Game.exe\" \"two words\" a\\\"b", &count);
    if (argv == NULL || count != 3 || !equals(argv[0], L"C:\\My Game.exe") ||
        !equals(argv[1], L"two words") || !equals(argv[2], L"a\"b") || LocalFree(argv) != NULL)
        return 26;
    if (CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) != S_OK ||
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) != S_FALSE ||
        CoInitializeEx(NULL, COINIT_MULTITHREADED) != RPC_E_CHANGED_MODE)
        return 27;
    CoUninitialize();
    CoUninitialize();
    return 0;
}

struct thread_fixture {
    HANDLE event;
    DWORD fls;
    LONG cleanup;
};
static VOID CALLBACK fls_exit(void *context)
{
    struct thread_fixture *fixture = context;
    fixture->cleanup = 1;
}

static DWORD WINAPI worker(void *context)
{
    struct thread_fixture *fixture = context;
    if (thread_tls != 1 || !FlsSetValue(fixture->fls, fixture) || FlsGetValue(fixture->fls) != fixture ||
        !SetEvent(fixture->event))
        return 99;
    ExitThread(37);
}

static int check_threads(void)
{
    struct thread_fixture fixture = {.event = CreateEventW(NULL, FALSE, FALSE, NULL),
                                     .fls = FlsAlloc(fls_exit)};
    if (fixture.event == NULL || fixture.fls == FLS_OUT_OF_INDEXES ||
        WaitForSingleObject(fixture.event, 0) != WAIT_TIMEOUT)
        return 31;
    DWORD id, code;
    HANDLE thread = CreateThread(NULL, 0, worker, &fixture, 0, &id);
    if (thread == NULL || id == 0 || WaitForSingleObject(fixture.event, 1000) != WAIT_OBJECT_0 ||
        WaitForSingleObjectEx(thread, 1000, FALSE) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &code) || code != 37 || fixture.cleanup != 1 || thread_tls != 3 ||
        WaitForSingleObject(fixture.event, 0) != WAIT_TIMEOUT ||
        !FlsFree(fixture.fls) || !CloseHandle(thread) || !CloseHandle(fixture.event))
        return 32;

    CRITICAL_SECTION section;
    if (!InitializeCriticalSectionEx(&section, 2000, 0))
        return 33;
    EnterCriticalSection(&section);
    EnterCriticalSection(&section);
    LeaveCriticalSection(&section);
    LeaveCriticalSection(&section);
    DeleteCriticalSection(&section);
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE condition = CONDITION_VARIABLE_INIT;
    AcquireSRWLockExclusive(&lock);
    if (SleepConditionVariableSRW(&condition, &lock, 1, 0) || GetLastError() != ERROR_TIMEOUT)
        return 34;
    ReleaseSRWLockExclusive(&lock);
    if (!TryAcquireSRWLockExclusive(&lock))
        return 35;
    ReleaseSRWLockExclusive(&lock);
    INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    void *value = NULL;
    if (!InitOnceBeginInitialize(&once, 0, &pending, NULL) || !pending ||
        !InitOnceComplete(&once, 0, (void *)(ULONG_PTR)16) ||
        !InitOnceBeginInitialize(&once, INIT_ONCE_CHECK_ONLY, &pending, &value) ||
        pending || value != (void *)(ULONG_PTR)16)
        return 36;
    return 0;
}

static int check_hash(void)
{
    static const unsigned char sha256[] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    BCRYPT_ALG_HANDLE provider;
    BCRYPT_HASH_HANDLE hash;
    unsigned char digest[sizeof(sha256)], input[] = "abc";
    DWORD length, bytes;
    if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0 ||
        BCryptGetProperty(provider, BCRYPT_HASH_LENGTH, (PUCHAR)&length, sizeof(length), &bytes, 0) != 0 ||
        length != sizeof(digest) || bytes != sizeof(length))
        return 41;
    if (BCryptCreateHash(provider, &hash, NULL, 0, NULL, 0, 0) != 0 ||
        BCryptHashData(hash, input, 1, 0) != 0 || BCryptHashData(hash, input + 1, 2, 0) != 0 ||
        BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0 || !bytes_equal(digest, sha256, sizeof(digest)) ||
        BCryptDestroyHash(hash) != 0 || BCryptCloseAlgorithmProvider(provider, 0) != 0)
        return 42;
    return 0;
}

static int check_sockets(void)
{
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0 || data.wVersion != MAKEWORD(2, 2))
        return 51;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    int length = sizeof(address), on = 1;
    if (listener == INVALID_SOCKET || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on)) ||
        bind(listener, (const struct sockaddr *)&address, length) != 0 ||
        getsockname(listener, (struct sockaddr *)&address, &length) != 0 ||
        listen(listener, 1) != 0)
        return 52;
    SOCKET sender = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sender == INVALID_SOCKET || connect(sender, (const struct sockaddr *)&address, length) != 0)
        return 53;
    SOCKET receiver = accept(listener, NULL, NULL);
    ULONG nonblocking = 1;
    char bytes[4];
    if (receiver == INVALID_SOCKET || ioctlsocket(receiver, FIONBIO, &nonblocking) != 0 ||
        recv(receiver, bytes, sizeof(bytes), 0) != SOCKET_ERROR || WSAGetLastError() != WSAEWOULDBLOCK ||
        send(sender, "PING", 4, 0) != 4)
        return 54;
    fd_set readable;
    readable.fd_count = 1;
    readable.fd_array[0] = receiver;
    const struct timeval timeout = {1, 0};
    if (select(0, &readable, NULL, NULL, &timeout) != 1 || readable.fd_count != 1 ||
        recv(receiver, bytes, sizeof(bytes), 0) != 4 || !bytes_equal(bytes, "PING", 4) ||
        closesocket(receiver) != 0 || closesocket(sender) != 0 || closesocket(listener) != 0 ||
        WSACleanup() != 0)
        return 55;
    return 0;
}

static BOOL created, delivered;
static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CREATE) {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
        created = equals(create->lpszName, L"Vuelo ñ");
    }
    if (message == WM_APP + 7)
        delivered = wparam == 42 && lparam == 17;
    return DefWindowProcW(window, message, wparam, lparam);
}

_Static_assert(sizeof(TASKDIALOGCONFIG) == 160, "Windows TASKDIALOGCONFIG layout");

static HRESULT CALLBACK task_callback(HWND window, UINT event, WPARAM wparam, LPARAM lparam, LONG_PTR data)
{
    (void)wparam;
    (void)lparam;
    unsigned int *seen = (unsigned int *)data;
    if (event == TDN_CREATED)
        SendMessageA(window, TDM_CLICK_BUTTON, IDOK, 0);
    else if (event == TDN_BUTTON_CLICKED)
        *seen |= 1;
    else if (event == TDN_DESTROYED)
        *seen |= 2;
    return S_OK;
}

static int check_desktop(void)
{
    const WNDCLASSEXW type = {.cbSize = sizeof(type), .lpfnWndProc = procedure,
                             .lpszClassName = L"Fixture avión"};
    if (!RegisterClassExW(&type))
        return 61;
    HWND window = CreateWindowExW(0, type.lpszClassName, L"Vuelo ñ", WS_VISIBLE, 0, 0,
                                  200, 100, NULL, NULL, NULL, NULL);
    if (window == NULL || !created || !IsWindowUnicode(window) || !IsWindowVisible(window) ||
        !SetWindowTextW(window, L"Ruta ✓"))
        return 62;
    WCHAR text[16];
    if (DefWindowProcW(window, WM_GETTEXT, 16, (LPARAM)text) != 6 || !equals(text, L"Ruta ✓"))
        return 63;
    if (SetMessageExtraInfo(27) != 0 || GetMessageExtraInfo() != 27 ||
        SetMessageExtraInfo(0) != 27 || !PostMessageW(window, WM_APP + 7, 42, 17))
        return 64;
    MSG message;
    if (!PeekMessageW(&message, window, WM_APP + 7, WM_APP + 7, PM_NOREMOVE) ||
        !PeekMessageW(&message, window, WM_APP + 7, WM_APP + 7, PM_REMOVE))
        return 65;
    DispatchMessageW(&message);
    if (!delivered || PeekMessageW(&message, window, WM_APP + 7, WM_APP + 7, PM_REMOVE))
        return 66;
    HIMC ime = ImmGetContext(window);
    COMPOSITIONFORM composition = {.dwStyle = CFS_POINT, .ptCurrentPos = {8, 16}};
    CANDIDATEFORM candidate = {.dwStyle = CFS_CANDIDATEPOS, .ptCurrentPos = {8, 24}};
    if (ime == NULL || !ImmSetCompositionWindow(ime, &composition) ||
        !ImmSetCandidateWindow(ime, &candidate) || !ImmReleaseContext(window, ime))
        return 68;
    unsigned int seen = 0;
    TASKDIALOGCONFIG task = {.cbSize = sizeof(task), .hwndParent = window, .dwCommonButtons = TDCBF_OK_BUTTON,
        .pszWindowTitle = L"Fixture", .pszContent = L"SDL dialog", .pfCallback = task_callback,
        .lpCallbackData = (LONG_PTR)&seen};
    int chosen = 0;
    if (TaskDialogIndirect(&task, &chosen, NULL, NULL) != S_OK || chosen != IDOK || seen != 3)
        return 69;
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {.cbSize = sizeof(info)};
    if (monitor == NULL || !GetMonitorInfoW(monitor, &info) || info.rcMonitor.right <= info.rcMonitor.left ||
        GetSystemMetrics(SM_CXSCREEN) <= 0 || !DestroyWindow(window) ||
        !UnregisterClassW(type.lpszClassName, NULL))
        return 67;
    return 0;
}

static int check_nls(void)
{
    WCHAR buffer[64];
    const SYSTEMTIME date = {.wYear = 2026, .wMonth = 11, .wDay = 11, .wHour = 13, .wMinute = 7};
    if (GetLocaleInfoEx(L"en-US", LOCALE_SNAME, buffer, 64) != 6 || !equals(buffer, L"en-US") ||
        !GetDateFormatW(LOCALE_USER_DEFAULT, 0, &date, L"yyyy-MM-dd", buffer, 64) ||
        !equals(buffer, L"2026-11-11") ||
        !GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_FORCE24HOURFORMAT | TIME_NOTIMEMARKER | TIME_NOSECONDS,
                        &date, NULL, buffer, 64) || !equals(buffer, L"13:07"))
        return 71;
    CPINFO info;
    if (!GetCPInfo(CP_UTF8, &info) || info.MaxCharSize != 4 ||
        CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, L"Árbol", -1, L"árbol", -1) != CSTR_EQUAL ||
        !LCMapStringEx(L"en-US", LCMAP_UPPERCASE, L"ñ", -1, buffer, 64, NULL, NULL, 0) ||
        !equals(buffer, L"Ñ"))
        return 72;
    OSVERSIONINFOEXW version = {.dwOSVersionInfoSize = sizeof(version), .dwMajorVersion = 10};
    const DWORD fields = VER_MAJORVERSION | VER_MINORVERSION;
    ULONGLONG condition = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
    condition = VerSetConditionMask(condition, VER_MINORVERSION, VER_GREATER_EQUAL);
    if (!VerifyVersionInfoW(&version, fields, condition))
        return 73;
    return 0;
}

static int check_gdi(void)
{
    HDC source = CreateCompatibleDC(NULL), target = CreateCompatibleDC(NULL);
    BITMAPINFO info;
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = 2;
    info.bmiHeader.biHeight = -2;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *source_bits = NULL, *target_bits = NULL;
    HBITMAP from = CreateDIBSection(source, &info, DIB_RGB_COLORS, &source_bits, NULL, 0);
    HBITMAP to = CreateDIBSection(target, &info, DIB_RGB_COLORS, &target_bits, NULL, 0);
    if (source == NULL || target == NULL || from == NULL || to == NULL || !source_bits || !target_bits)
        return 81;
    HGDIOBJ initial_source = SelectObject(source, from), initial_target = SelectObject(target, to);
    HBRUSH brush = CreateSolidBrush(RGB(0, 0, 255));
    RECT rect = {0, 0, 2, 2};
    if (brush == NULL || !FillRect(target, &rect, brush) ||
        ((DWORD *)target_bits)[0] != 0x000000ff)
        return 82;
    ((DWORD *)source_bits)[0] = 0x80800000; /* Premultiplied red at alpha 128. */
    const BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (!AlphaBlend(target, 0, 0, 1, 1, source, 0, 0, 1, 1, blend) || !GdiFlush() ||
        (((DWORD *)target_bits)[0] & 0x00ffffff) != 0x0080007f)
        return 83;
    HFONT font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, 0, 0, L"DejaVu Sans");
    HGDIOBJ initial_font = font != NULL ? SelectObject(target, font) : NULL;
    WCHAR label[] = L"Texto";
    if (initial_font == NULL || !DrawTextW(target, label, -1, &rect, DT_CALCRECT | DT_SINGLELINE) ||
        rect.right <= rect.left || rect.bottom <= rect.top)
        return 84;
    if (!SelectObject(target, initial_font) || !SelectObject(source, initial_source) ||
        !SelectObject(target, initial_target) || !DeleteObject(font) || !DeleteObject(brush) ||
        !DeleteObject(from) || !DeleteObject(to) || !DeleteDC(source) || !DeleteDC(target))
        return 85;
    return 0;
}

static int check_registry(void)
{
    if (!CreateDirectoryW(L"C:\\Registry", NULL) || !CreateDirectoryW(L"C:\\Registry\\HKCU", NULL) ||
        !CreateDirectoryW(L"C:\\Registry\\HKCU\\Test", NULL))
        return 91;
    const WCHAR path[] = L"C:\\Registry\\HKCU\\Test\\Label.sz";
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    const char text[] = "árbol";
    DWORD done;
    if (file == INVALID_HANDLE_VALUE || !WriteFile(file, text, sizeof(text) - 1, &done, NULL) ||
        done != sizeof(text) - 1 || !CloseHandle(file))
        return 92;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Test", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 93;
    DWORD type, bytes = 0;
    WCHAR value[8];
    if (RegQueryValueExW(key, L"Label", NULL, &type, NULL, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes != 12 ||
        RegQueryValueExW(key, L"Label", NULL, NULL, (BYTE *)value, &bytes) != ERROR_SUCCESS ||
        !equals(value, L"árbol") || RegCloseKey(key) != ERROR_SUCCESS)
        return 94;
    if (!DeleteFileW(path) || !RemoveDirectoryW(L"C:\\Registry\\HKCU\\Test") ||
        !RemoveDirectoryW(L"C:\\Registry\\HKCU") || !RemoveDirectoryW(L"C:\\Registry"))
        return 95;
    return 0;
}

int import_fixture_entry(void)
{
    unsigned char *tls = fixture_tls();
    if (process_tls != 1 || tls == NULL || tls[0] != 41)
        return 3;
    tls[0] = 92;
    int result = check_files();
    if (result == 0)
        result = check_memory_environment();
    if (result == 0)
        result = check_threads();
    if (result == 0)
        result = check_hash();
    if (result == 0)
        result = check_sockets();
    if (result == 0)
        result = check_desktop();
    if (result == 0)
        result = check_nls();
    if (result == 0)
        result = check_gdi();
    if (result == 0)
        result = check_registry();
    return result;
}
