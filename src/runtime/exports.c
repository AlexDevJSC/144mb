/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "ntnative/counter.h"
#include "compat.h"
#include "desktop.h"
#include "desktop_resources.h"
#include "dialogs.h"
#include "nt_services.h"
#include "nt_core_information.h"
#include "exception_callbacks.h"
#include "user_folders.h"
#include "crypto.h"
#include "sync.h"
#include "dynamic_tls.h"
#include "winbase.h"
#include "winfile.h"
#include "child_process.h"
#include "console.h"
#include "shell.h"
#include "winsock.h"
#include "nls.h"
#include "gdi_ext.h"
#include "registry.h"
#include "raw_input.h"
#include "task_dialog.h"
#include "windesktop.h"
#ifdef NT_HAVE_WIC
#include "wic.h"
#endif
#ifdef NT_HAVE_VKD3D
#include "graphics.h"
#include "shader.h"
#include "streamline.h"
#endif
#include "display_config.h"

#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/random.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static _Thread_local uint32_t last_error;

static nt_status NT_MS_ABI nt_counter(int64_t *counter, int64_t *frequency)
{
    return nt_query_performance_counter(counter, frequency);
}

static int32_t NT_MS_ABI win_counter(int64_t *counter)
{
    return nt_query_performance_counter(counter, NULL) == NT_STATUS_SUCCESS;
}

static uint32_t NT_MS_ABI win_process_id(void)
{
    return (uint32_t)getpid();
}

static uint32_t NT_MS_ABI win_thread_id(void)
{
    return (uint32_t)gettid();
}

uint32_t NT_MS_ABI nt_GetLastError(void)
{
    const unsigned char *teb = nt_guest_thread_teb();
    if (!teb)
        return last_error;
    uint32_t error;
    memcpy(&error, teb + NT_TEB_LAST_ERROR, sizeof(error));
    return error;
}

void NT_MS_ABI nt_SetLastError(uint32_t error)
{
    unsigned char *teb = nt_guest_thread_teb();
    if (teb)
        memcpy(teb + NT_TEB_LAST_ERROR, &error, sizeof(error));
    else
        last_error = error;
}

static int32_t NT_MS_ABI win_mul_div(int32_t left, int32_t right, int32_t divisor)
{
    if (divisor == 0)
        return -1;

    const int64_t product = (int64_t)left * right;
    const bool negative = (product < 0) != (divisor < 0);
    const int64_t numerator = product < 0 ? -product : product;
    const int64_t denominator = divisor < 0 ? -(int64_t)divisor : divisor;
    int64_t result = (numerator + denominator / 2) / denominator;
    if (negative)
        result = -result;

    return result < INT32_MIN || result > INT32_MAX ? -1 : (int32_t)result;
}

static void *NT_MS_ABI crt_memchr(const void *data, int byte, size_t count)
{
    /* The guest CRT returns an unqualified pointer into the same buffer. */
    return (void *)memchr(data, byte, count);
}

static int NT_MS_ABI crt_memcmp(const void *left, const void *right, size_t count)
{
    return memcmp(left, right, count);
}

static void *NT_MS_ABI crt_memcpy(void *destination, const void *source, size_t count)
{
    return memcpy(destination, source, count);
}

static void *NT_MS_ABI crt_memmove(void *destination, const void *source, size_t count)
{
    return memmove(destination, source, count);
}

static void *NT_MS_ABI crt_memset(void *destination, int byte, size_t count)
{
    return memset(destination, byte, count);
}

static char *NT_MS_ABI crt_strchr(const char *string, int byte)
{
    return (char *)strchr(string, byte);
}

static char *NT_MS_ABI crt_strrchr(const char *string, int byte)
{
    return (char *)strrchr(string, byte);
}

static void *NT_MS_ABI crt_malloc(size_t size)
{
    return malloc(size == 0 ? 1 : size);
}

static void NT_MS_ABI crt_free(void *memory)
{
    free(memory);
}

static void *NT_MS_ABI crt_realloc(void *memory, size_t size)
{
    if (memory == NULL)
        return crt_malloc(size);
    if (size == 0) {
        free(memory);
        return NULL;
    }
    return realloc(memory, size);
}

static double NT_MS_ABI crt_ceil(double value)
{
    return ceil(value);
}

static double NT_MS_ABI crt_floor(double value)
{
    return floor(value);
}

static float NT_MS_ABI crt_roundf(float value)
{
    return roundf(value);
}

static double NT_MS_ABI crt_sqrt(double value)
{
    return nt_runtime_sqrt(value);
}

static float NT_MS_ABI crt_sqrtf(float value)
{
    return sqrtf(value);
}

static char *NT_MS_ABI crt_strcat(char *destination, const char *source)
{
    return strcat(destination, source);
}

static int NT_MS_ABI crt_strcmp(const char *left, const char *right)
{
    return strcmp(left, right);
}

static char *NT_MS_ABI crt_strcpy(char *destination, const char *source)
{
    return strcpy(destination, source);
}

static size_t NT_MS_ABI crt_strcspn(const char *string, const char *reject)
{
    return strcspn(string, reject);
}

static size_t NT_MS_ABI crt_strlen(const char *string)
{
    return strlen(string);
}

static char *NT_MS_ABI crt_strncpy(char *destination, const char *source, size_t count)
{
    return strncpy(destination, source, count);
}

static size_t NT_MS_ABI crt_strspn(const char *string, const char *accept)
{
    return strspn(string, accept);
}

static size_t NT_MS_ABI crt_wcslen(const uint16_t *string)
{
    const uint16_t *end = string;
    while (*end != 0)
        ++end;
    return (size_t)(end - string);
}

typedef int (NT_MS_ABI *guest_compare)(const void *, const void *);

static int compare_for_linux(const void *left, const void *right, void *context)
{
    const guest_compare *compare = context;
    return (*compare)(left, right);
}

static void NT_MS_ABI crt_qsort(void *base, size_t count, size_t width, guest_compare compare)
{
    if (count > 1)
        qsort_r(base, count, width, compare_for_linux, &compare);
}

struct runtime_export {
    const char *name;
    nt_guest_proc target;
    uint16_t ordinal;
};

#define EXPORT(name, function) {name, (nt_guest_proc)function, 0}
#define WIN_EXPORT(name) EXPORT(#name, nt_##name)
#define NT_EXPORT(name) EXPORT("Nt" #name, nt_Nt##name), EXPORT("Zw" #name, nt_Nt##name)

static const struct runtime_export ntdll_exports[] = {
    EXPORT("NtQueryPerformanceCounter", nt_counter),
    EXPORT("ZwQueryPerformanceCounter", nt_counter),
    NT_EXPORT(QuerySystemTime),
    NT_EXPORT(DelayExecution),
    NT_EXPORT(Close),
    NT_EXPORT(CreateFile),
    NT_EXPORT(OpenFile),
    NT_EXPORT(ReadFile),
    NT_EXPORT(WriteFile),
    NT_EXPORT(QueryInformationFile),
    NT_EXPORT(SetInformationFile),
    NT_EXPORT(FlushBuffersFile),
    NT_EXPORT(DuplicateObject),
    NT_EXPORT(CreateEvent),
    NT_EXPORT(OpenEvent),
    NT_EXPORT(QueryEvent),
    NT_EXPORT(CreateSemaphore),
    NT_EXPORT(OpenSemaphore),
    NT_EXPORT(QuerySemaphore),
    NT_EXPORT(ReleaseSemaphore),
    NT_EXPORT(ResumeThread),
    NT_EXPORT(SetEvent),
    NT_EXPORT(ResetEvent),
    NT_EXPORT(WaitForSingleObject),
    NT_EXPORT(WaitForMultipleObjects),
    NT_EXPORT(QueryInformationProcess),
    NT_EXPORT(QueryInformationThread),
    NT_EXPORT(AllocateVirtualMemory),
    NT_EXPORT(FreeVirtualMemory),
    NT_EXPORT(ProtectVirtualMemory),
    NT_EXPORT(QueryVirtualMemory),
    WIN_EXPORT(RtlGetCurrentPeb),
    EXPORT("RtlAddVectoredExceptionHandler", nt_AddVectoredExceptionHandler),
    EXPORT("RtlRemoveVectoredExceptionHandler", nt_RemoveVectoredExceptionHandler),
    WIN_EXPORT(RtlCaptureContext),
    WIN_EXPORT(RtlLookupFunctionEntry),
    WIN_EXPORT(RtlVirtualUnwind),
    WIN_EXPORT(RtlUnwind),
    WIN_EXPORT(RtlUnwindEx),
    WIN_EXPORT(RtlPcToFileHeader),
    EXPORT("RtlGetLastWin32Error", nt_GetLastError),
    EXPORT("RtlSetLastWin32Error", nt_SetLastError),
};
#undef NT_EXPORT

static uintptr_t NT_MS_ABI win_GetModuleHandleA(const char *name);
static int32_t NT_MS_ABI win_GetModuleHandleExW(uint32_t flags, const uint16_t *name, uintptr_t *handle);
static uintptr_t NT_MS_ABI win_LoadLibraryW(const uint16_t *name);
static uintptr_t NT_MS_ABI win_LoadLibraryExW(const uint16_t *name, uintptr_t file, uint32_t flags);
static int32_t NT_MS_ABI win_QueryPerformanceFrequency(int64_t *frequency);
static void NT_MS_ABI win_OutputDebugStringA(const char *text);
static void NT_MS_ABI win_OutputDebugStringW(const uint16_t *text);
static void *NT_MS_ABI win_EncodePointer(void *pointer);
static void *NT_MS_ABI win_DecodePointer(void *pointer);

bool nt_runtime_fluidsynth_available(void);
#define FLUID(type, name, signature, arguments) type NT_MS_ABI nt_##name signature;
#define FLUID_VOID(name, signature, arguments) FLUID(void, name, signature, arguments)
#include "fluid_api.h"
#undef FLUID_VOID
#undef FLUID
int NT_MS_ABI nt_fluid_synth_sfload(void *synth, const char *path, int reset);

static const struct runtime_export kernel32_exports[] = {
    WIN_EXPORT(AddVectoredExceptionHandler),
    WIN_EXPORT(RemoveVectoredExceptionHandler),
    WIN_EXPORT(CloseHandle),
    WIN_EXPORT(DuplicateHandle),
    WIN_EXPORT(CreateDirectoryA),
    WIN_EXPORT(CreateFileA),
    WIN_EXPORT(DeleteFileA),
    WIN_EXPORT(FormatMessageA),
    WIN_EXPORT(FreeLibrary),
    WIN_EXPORT(GetCurrentProcess),
    EXPORT("GetCurrentProcessId", win_process_id),
    EXPORT("GetCurrentThreadId", win_thread_id),
    WIN_EXPORT(GetLastError),
    WIN_EXPORT(GetLocalTime),
    WIN_EXPORT(GetModuleFileNameA),
    WIN_EXPORT(GetModuleHandleW),
    WIN_EXPORT(GetProcAddress),
    WIN_EXPORT(GetStartupInfoW),
    WIN_EXPORT(GetSystemTimeAsFileTime),
    WIN_EXPORT(GetTickCount),
    WIN_EXPORT(GlobalAlloc),
    WIN_EXPORT(GlobalFree),
    WIN_EXPORT(GlobalLock),
    WIN_EXPORT(GlobalUnlock),
    WIN_EXPORT(InitializeSListHead),
    WIN_EXPORT(IsDebuggerPresent),
    WIN_EXPORT(IsProcessorFeaturePresent),
    WIN_EXPORT(LoadLibraryA),
    WIN_EXPORT(LocalFree),
    WIN_EXPORT(MoveFileExA),
    EXPORT("MulDiv", win_mul_div),
    WIN_EXPORT(MultiByteToWideChar),
    EXPORT("QueryPerformanceCounter", win_counter),
    WIN_EXPORT(RemoveDirectoryA),
    WIN_EXPORT(RemoveDirectoryW),
    WIN_EXPORT(RtlCaptureContext),
    WIN_EXPORT(RaiseException),
    WIN_EXPORT(RtlUnwind),
    WIN_EXPORT(RtlUnwindEx),
    WIN_EXPORT(RtlLookupFunctionEntry),
    WIN_EXPORT(RtlVirtualUnwind),
    WIN_EXPORT(SetLastError),
    WIN_EXPORT(SetUnhandledExceptionFilter),
    WIN_EXPORT(TerminateProcess),
    WIN_EXPORT(UnhandledExceptionFilter),
    WIN_EXPORT(CompareStringW),
    WIN_EXPORT(EnumSystemLocalesW),
    WIN_EXPORT(GetCPInfo),
    WIN_EXPORT(GetDateFormatW),
    WIN_EXPORT(GetLocaleInfoA),
    WIN_EXPORT(GetLocaleInfoEx),
    WIN_EXPORT(GetLocaleInfoW),
    WIN_EXPORT(GetStringTypeW),
    WIN_EXPORT(GetTimeFormatW),
    WIN_EXPORT(GetTimeZoneInformation),
    WIN_EXPORT(GetUserDefaultLCID),
    WIN_EXPORT(IsValidLocale),
    WIN_EXPORT(LCMapStringEx),
    WIN_EXPORT(LCMapStringW),
    WIN_EXPORT(VirtualAlloc),
    WIN_EXPORT(VirtualFree),
    WIN_EXPORT(VirtualProtect),
    WIN_EXPORT(VirtualQuery),
    WIN_EXPORT(VerSetConditionMask),
    WIN_EXPORT(VerifyVersionInfoW),
    WIN_EXPORT(AcquireSRWLockExclusive),
    WIN_EXPORT(AreFileApisANSI),
    WIN_EXPORT(CreateDirectoryW),
    WIN_EXPORT(CreateEventW),
    WIN_EXPORT(OpenEventW),
    WIN_EXPORT(CreateSemaphoreW),
    WIN_EXPORT(OpenSemaphoreW),
    WIN_EXPORT(ReleaseSemaphore),
    WIN_EXPORT(CreateFile2),
    WIN_EXPORT(CreateFileW),
    WIN_EXPORT(CreateThread),
    WIN_EXPORT(ResumeThread),
    WIN_EXPORT(CreateProcessW),
    WIN_EXPORT(GetExitCodeProcess),
    WIN_EXPORT(DeleteCriticalSection),
    WIN_EXPORT(DeleteFileW),
    WIN_EXPORT(EnterCriticalSection),
    WIN_EXPORT(ExitProcess),
    WIN_EXPORT(ExitThread),
    WIN_EXPORT(FindClose),
    WIN_EXPORT(FindFirstFileExW),
    WIN_EXPORT(FindFirstFileW),
    WIN_EXPORT(FindNextFileW),
    WIN_EXPORT(FlsAlloc),
    WIN_EXPORT(FlsFree),
    WIN_EXPORT(FlsGetValue),
    WIN_EXPORT(FlsSetValue),
    WIN_EXPORT(TlsAlloc),
    WIN_EXPORT(TlsFree),
    WIN_EXPORT(TlsGetValue),
    WIN_EXPORT(TlsSetValue),
    WIN_EXPORT(FlushFileBuffers),
    WIN_EXPORT(FormatMessageW),
    WIN_EXPORT(FreeEnvironmentStringsW),
    WIN_EXPORT(FreeLibraryAndExitThread),
    WIN_EXPORT(GetACP),
    WIN_EXPORT(GetCommandLineA),
    WIN_EXPORT(GetCommandLineW),
    WIN_EXPORT(GetConsoleMode),
    WIN_EXPORT(AttachConsole),
    WIN_EXPORT(AllocConsole),
    WIN_EXPORT(FreeConsole),
    WIN_EXPORT(GetConsoleWindow),
    WIN_EXPORT(ReadConsoleW),
    WIN_EXPORT(SetConsoleTitleA),
    WIN_EXPORT(GetConsoleOutputCP),
    WIN_EXPORT(GetCurrentDirectoryW),
    WIN_EXPORT(GetCurrentThread),
    WIN_EXPORT(GetEnvironmentStringsW),
    WIN_EXPORT(GetEnvironmentVariableA),
    WIN_EXPORT(GetEnvironmentVariableW),
    WIN_EXPORT(GetExitCodeThread),
    WIN_EXPORT(GetThreadId),
    WIN_EXPORT(OpenThread),
    WIN_EXPORT(GetFileAttributesExW),
    WIN_EXPORT(GetFileAttributesW),
    WIN_EXPORT(GetFileInformationByHandleEx),
    WIN_EXPORT(GetFileSizeEx),
    WIN_EXPORT(GetHandleInformation),
    WIN_EXPORT(GetFileType),
    WIN_EXPORT(GetFinalPathNameByHandleW),
    WIN_EXPORT(GetFullPathNameW),
    WIN_EXPORT(GetModuleFileNameW),
    WIN_EXPORT(GetOEMCP),
    WIN_EXPORT(GetProcessHeap),
    WIN_EXPORT(GetStdHandle),
    WIN_EXPORT(GetSystemDirectoryW),
    WIN_EXPORT(GetSystemTime),
    WIN_EXPORT(GetTempPathW),
    WIN_EXPORT(GetTickCount64),
    WIN_EXPORT(HeapAlloc),
    WIN_EXPORT(HeapFree),
    WIN_EXPORT(HeapReAlloc),
    WIN_EXPORT(HeapSize),
    WIN_EXPORT(InitializeCriticalSection),
    WIN_EXPORT(InitializeCriticalSectionEx),
    WIN_EXPORT(InitOnceBeginInitialize),
    WIN_EXPORT(InitOnceComplete),
    WIN_EXPORT(IsDBCSLeadByte),
    WIN_EXPORT(IsValidCodePage),
    WIN_EXPORT(LeaveCriticalSection),
    WIN_EXPORT(LocalAlloc),
    WIN_EXPORT(MoveFileExW),
    WIN_EXPORT(ReadFile),
    WIN_EXPORT(ReleaseSRWLockExclusive),
    WIN_EXPORT(RtlPcToFileHeader),
    WIN_EXPORT(SetCurrentDirectoryW),
    WIN_EXPORT(SetEndOfFile),
    WIN_EXPORT(SetEnvironmentVariableW),
    WIN_EXPORT(SetEvent),
    WIN_EXPORT(SetFileInformationByHandle),
    WIN_EXPORT(SetFilePointerEx),
    WIN_EXPORT(SetStdHandle),
    WIN_EXPORT(SetThreadDescription),
    WIN_EXPORT(Sleep),
    WIN_EXPORT(SleepConditionVariableSRW),
    WIN_EXPORT(TryAcquireSRWLockExclusive),
    WIN_EXPORT(WaitForSingleObject),
    WIN_EXPORT(WaitForSingleObjectEx),
    WIN_EXPORT(WaitForMultipleObjects),
    WIN_EXPORT(WaitForMultipleObjectsEx),
    WIN_EXPORT(WakeAllConditionVariable),
    WIN_EXPORT(WakeConditionVariable),
    WIN_EXPORT(WideCharToMultiByte),
    WIN_EXPORT(WriteConsoleA),
    WIN_EXPORT(WriteConsoleW),
    WIN_EXPORT(WriteFile),
    EXPORT("DecodePointer", win_DecodePointer),
    EXPORT("EncodePointer", win_EncodePointer),
    EXPORT("GetModuleHandleA", win_GetModuleHandleA),
    EXPORT("GetModuleHandleExW", win_GetModuleHandleExW),
    EXPORT("LoadLibraryW", win_LoadLibraryW),
    EXPORT("LoadLibraryExW", win_LoadLibraryExW),
    EXPORT("OutputDebugStringA", win_OutputDebugStringA),
    EXPORT("OutputDebugStringW", win_OutputDebugStringW),
    EXPORT("QueryPerformanceFrequency", win_QueryPerformanceFrequency),
    EXPORT("GetSystemTimePreciseAsFileTime", nt_GetSystemTimeAsFileTime)
};

static const struct runtime_export user32_exports[] = {
    WIN_EXPORT(GetDisplayConfigBufferSizes),
    WIN_EXPORT(QueryDisplayConfig),
    WIN_EXPORT(DisplayConfigGetDeviceInfo),
    WIN_EXPORT(DrawTextW),
    WIN_EXPORT(FillRect),
    WIN_EXPORT(AdjustWindowRect),
    WIN_EXPORT(ClientToScreen),
    WIN_EXPORT(CreateWindowExW),
    WIN_EXPORT(DefWindowProcW),
    WIN_EXPORT(DispatchMessageW),
    WIN_EXPORT(GetActiveWindow),
    WIN_EXPORT(GetAsyncKeyState),
    WIN_EXPORT(GetCapture),
    WIN_EXPORT(GetClipboardData),
    WIN_EXPORT(GetCursorPos),
    WIN_EXPORT(GetForegroundWindow),
    WIN_EXPORT(GetKeyboardLayout),
    WIN_EXPORT(GetKeyState),
    WIN_EXPORT(GetMessageExtraInfo),
    WIN_EXPORT(SetMessageExtraInfo),
    WIN_EXPORT(GetMessageW),
    WIN_EXPORT(GetMonitorInfoW),
    WIN_EXPORT(GetSystemMetrics),
    WIN_EXPORT(GetWindowPlacement),
    WIN_EXPORT(GetWindowThreadProcessId),
    WIN_EXPORT(IsIconic),
    WIN_EXPORT(IsWindowUnicode),
    WIN_EXPORT(IsWindowVisible),
    WIN_EXPORT(LoadCursorW),
    WIN_EXPORT(LoadImageW),
    WIN_EXPORT(MonitorFromPoint),
    WIN_EXPORT(MonitorFromWindow),
    WIN_EXPORT(PeekMessageW),
    WIN_EXPORT(PostMessageW),
    WIN_EXPORT(RegisterClassExW),
    WIN_EXPORT(RegisterRawInputDevices),
    WIN_EXPORT(GetRawInputData),
    WIN_EXPORT(ScreenToClient),
    WIN_EXPORT(SetCursor),
    WIN_EXPORT(SetCursorPos),
    WIN_EXPORT(SetProcessDPIAware),
    WIN_EXPORT(SetWindowPlacement),
    WIN_EXPORT(SetWindowTextW),
    WIN_EXPORT(TrackMouseEvent),
    WIN_EXPORT(UnregisterClassW),
    WIN_EXPORT(UpdateWindow),
    EXPORT("GetWindowLongPtrW", nt_GetWindowLongPtrA),
    EXPORT("SetWindowLongPtrW", nt_SetWindowLongPtrA),
    WIN_EXPORT(AdjustWindowRectEx),
    WIN_EXPORT(AppendMenuA),
    WIN_EXPORT(BeginPaint),
    WIN_EXPORT(CheckDlgButton),
    WIN_EXPORT(CheckMenuItem),
    WIN_EXPORT(CloseClipboard),
    WIN_EXPORT(CreateMenu),
    WIN_EXPORT(CreateWindowExA),
    WIN_EXPORT(DefDlgProcA),
    WIN_EXPORT(DefWindowProcA),
    WIN_EXPORT(DestroyMenu),
    WIN_EXPORT(DestroyWindow),
    WIN_EXPORT(DispatchMessageA),
    WIN_EXPORT(DrawMenuBar),
    WIN_EXPORT(EmptyClipboard),
    WIN_EXPORT(EnableMenuItem),
    WIN_EXPORT(EnableWindow),
    WIN_EXPORT(EndPaint),
    WIN_EXPORT(GetClientRect),
    WIN_EXPORT(GetDC),
    WIN_EXPORT(GetDlgItemTextA),
    WIN_EXPORT(GetKeyboardState),
    WIN_EXPORT(GetMenu),
    WIN_EXPORT(GetMenuItemCount),
    WIN_EXPORT(GetMenuItemRect),
    WIN_EXPORT(GetMessageA),
    WIN_EXPORT(GetSysColor),
    WIN_EXPORT(GetWindowLongPtrA),
    WIN_EXPORT(GetWindowRect),
    WIN_EXPORT(IntersectRect),
    WIN_EXPORT(IsDialogMessageA),
    WIN_EXPORT(IsDlgButtonChecked),
    WIN_EXPORT(InvalidateRect),
    WIN_EXPORT(KillTimer),
    WIN_EXPORT(LoadCursorA),
    WIN_EXPORT(LoadIconA),
    WIN_EXPORT(MessageBeep),
    WIN_EXPORT(MessageBoxA),
    WIN_EXPORT(OffsetRect),
    WIN_EXPORT(OpenClipboard),
    WIN_EXPORT(PostQuitMessage),
    WIN_EXPORT(RegisterClassA),
    WIN_EXPORT(ReleaseCapture),
    WIN_EXPORT(ReleaseDC),
    WIN_EXPORT(SendDlgItemMessageA),
    WIN_EXPORT(SendMessageA),
    WIN_EXPORT(SetCapture),
    WIN_EXPORT(SetClipboardData),
    WIN_EXPORT(SetForegroundWindow),
    WIN_EXPORT(SetMenu),
    WIN_EXPORT(SetRect),
    WIN_EXPORT(SetTimer),
    WIN_EXPORT(SetWindowLongPtrA),
    WIN_EXPORT(SetWindowPos),
    WIN_EXPORT(SetWindowTextA),
    WIN_EXPORT(ShowWindow),
    WIN_EXPORT(SystemParametersInfoA),
    WIN_EXPORT(TranslateMessage),
    WIN_EXPORT(UnionRect),
    WIN_EXPORT(WinHelpA)
};

static const struct runtime_export win32u_exports[] = {
    EXPORT("NtUserGetAsyncKeyState", nt_GetAsyncKeyState),
    EXPORT("NtUserGetCursorPos", nt_GetCursorPos),
    EXPORT("NtUserGetForegroundWindow", nt_GetForegroundWindow),
    EXPORT("NtUserGetKeyState", nt_GetKeyState),
};

static const struct runtime_export gdi32_exports[] = {
    WIN_EXPORT(CreateDIBSection),
    WIN_EXPORT(CreateFontW),
    WIN_EXPORT(GdiFlush),
    WIN_EXPORT(BitBlt),
    WIN_EXPORT(CreateCompatibleBitmap),
    WIN_EXPORT(CreateCompatibleDC),
    WIN_EXPORT(CreateFontA),
    WIN_EXPORT(CreateFontIndirectA),
    WIN_EXPORT(CreateHatchBrush),
    WIN_EXPORT(CreatePen),
    WIN_EXPORT(CreateSolidBrush),
    WIN_EXPORT(DeleteDC),
    WIN_EXPORT(DeleteObject),
    WIN_EXPORT(Ellipse),
    WIN_EXPORT(EndDoc),
    WIN_EXPORT(EndPage),
    WIN_EXPORT(ExtTextOutW),
    WIN_EXPORT(GetDeviceCaps),
    WIN_EXPORT(GetStockObject),
    WIN_EXPORT(GetTextExtentPoint32A),
    WIN_EXPORT(GetTextExtentPoint32W),
    WIN_EXPORT(GetTextMetricsA),
    WIN_EXPORT(IntersectClipRect),
    WIN_EXPORT(Polygon),
    WIN_EXPORT(Polyline),
    WIN_EXPORT(Rectangle),
    WIN_EXPORT(SelectClipRgn),
    WIN_EXPORT(SelectObject),
    WIN_EXPORT(SetBkMode),
    WIN_EXPORT(SetMapMode),
    WIN_EXPORT(SetPixel),
    WIN_EXPORT(SetTextColor),
    WIN_EXPORT(StartDocA),
    WIN_EXPORT(StartPage)
};

static const struct runtime_export msimg32_exports[] = {
    WIN_EXPORT(AlphaBlend)
};

static const struct runtime_export comdlg32_exports[] = {
    WIN_EXPORT(CommDlgExtendedError),
    WIN_EXPORT(GetOpenFileNameA),
    WIN_EXPORT(GetOpenFileNameW),
    WIN_EXPORT(GetSaveFileNameA),
    WIN_EXPORT(GetSaveFileNameW),
    WIN_EXPORT(PrintDlgA)
};

static const struct runtime_export shell32_exports[] = {
    EXPORT("SHParseDisplayName", nt_SHParseDisplayName),
    EXPORT("SHOpenFolderAndSelectItems", nt_SHOpenFolderAndSelectItems),
    EXPORT("ShellExecuteW", nt_ShellExecuteW),
    WIN_EXPORT(SHGetKnownFolderPath),
    WIN_EXPORT(CommandLineToArgvW)
};

static const struct runtime_export ole32_exports[] = {
#ifdef NT_HAVE_WIC
    EXPORT("CoCreateInstance", nt_CoCreateInstance),
#endif
    EXPORT("CoTaskMemFree", crt_free),
    WIN_EXPORT(CoInitializeEx),
    WIN_EXPORT(CoUninitialize)
};

static const struct runtime_export imm32_exports[] = {
    WIN_EXPORT(ImmGetContext),
    WIN_EXPORT(ImmReleaseContext),
    WIN_EXPORT(ImmSetCompositionWindow),
    WIN_EXPORT(ImmSetCandidateWindow)
};

static const struct runtime_export advapi32_exports[] = {
    WIN_EXPORT(RegOpenKeyExW),
    WIN_EXPORT(RegQueryValueExW),
    WIN_EXPORT(RegCloseKey)
};

static const struct runtime_export bcrypt_exports[] = {
    WIN_EXPORT(BCryptOpenAlgorithmProvider),
    WIN_EXPORT(BCryptGetProperty),
    WIN_EXPORT(BCryptCloseAlgorithmProvider),
    WIN_EXPORT(BCryptCreateHash),
    WIN_EXPORT(BCryptHashData),
    WIN_EXPORT(BCryptFinishHash),
    WIN_EXPORT(BCryptDestroyHash)
};

static const struct runtime_export ws2_32_exports[] = {
    {"accept", (nt_guest_proc)nt_accept, 1},
    {"bind", (nt_guest_proc)nt_bind, 2},
    {"closesocket", (nt_guest_proc)nt_closesocket, 3},
    {"connect", (nt_guest_proc)nt_connect, 4},
    {"getsockname", (nt_guest_proc)nt_getsockname, 6},
    {"htonl", (nt_guest_proc)nt_htonl, 8},
    {"htons", (nt_guest_proc)nt_htons, 9},
    {"ioctlsocket", (nt_guest_proc)nt_ioctlsocket, 10},
    {"listen", (nt_guest_proc)nt_listen, 13},
    {"recv", (nt_guest_proc)nt_recv, 16},
    {"select", (nt_guest_proc)nt_select, 18},
    {"send", (nt_guest_proc)nt_send, 19},
    {"setsockopt", (nt_guest_proc)nt_setsockopt, 21},
    {"socket", (nt_guest_proc)nt_socket, 23},
    {"WSAGetLastError", (nt_guest_proc)nt_WSAGetLastError, 111},
    {"WSAStartup", (nt_guest_proc)nt_WSAStartup, 115},
    {"WSACleanup", (nt_guest_proc)nt_WSACleanup, 116},
};

static const struct runtime_export fluid_exports[] = {
#define FLUID(type, name, signature, arguments) WIN_EXPORT(name),
#define FLUID_VOID(name, signature, arguments) FLUID(void, name, signature, arguments)
#include "fluid_api.h"
#undef FLUID_VOID
#undef FLUID
    WIN_EXPORT(fluid_synth_sfload)
};

static const struct runtime_export comctl32_exports[] = {
    {"InitCommonControls", (nt_guest_proc)nt_InitCommonControls, 17},
    {"TaskDialogIndirect", (nt_guest_proc)nt_TaskDialogIndirect, 345}
};

static const struct runtime_export vcruntime_exports[] = {
    EXPORT("__C_specific_handler", nt_C_specific_handler),
    EXPORT("__current_exception", nt_current_exception),
    EXPORT("__current_exception_context", nt_current_exception_context),
    EXPORT("memchr", crt_memchr),
    EXPORT("memcmp", crt_memcmp),
    EXPORT("memcpy", crt_memcpy),
    EXPORT("memmove", crt_memmove),
    EXPORT("memset", crt_memset),
    EXPORT("strchr", crt_strchr),
    EXPORT("strrchr", crt_strrchr)
};

static const struct runtime_export heap_exports[] = {
    EXPORT("_set_new_mode", nt_set_new_mode),
    EXPORT("free", crt_free),
    EXPORT("malloc", crt_malloc),
    EXPORT("realloc", crt_realloc)
};

static const struct runtime_export math_exports[] = {
    EXPORT("__setusermatherr", nt_setusermatherr),
    EXPORT("_fdopen", nt_fdopen),
    EXPORT("ceil", crt_ceil),
    EXPORT("floor", crt_floor),
    EXPORT("roundf", crt_roundf),
    EXPORT("sqrt", crt_sqrt),
    EXPORT("sqrtf", crt_sqrtf)
};

static const struct runtime_export string_exports[] = {
    EXPORT("isspace", nt_isspace),
    EXPORT("strcat", crt_strcat),
    EXPORT("strcmp", crt_strcmp),
    EXPORT("strcpy", crt_strcpy),
    EXPORT("strcspn", crt_strcspn),
    EXPORT("strlen", crt_strlen),
    EXPORT("strncpy", crt_strncpy),
    EXPORT("strspn", crt_strspn),
    EXPORT("tolower", nt_tolower),
    EXPORT("toupper", nt_toupper),
    EXPORT("wcslen", crt_wcslen)
};

static const struct runtime_export convert_exports[] = {
    EXPORT("atof", nt_atof),
    EXPORT("atoi", nt_atoi)
};

static const struct runtime_export environment_exports[] = {
    EXPORT("getenv", nt_getenv)
};

static const struct runtime_export locale_exports[] = {
    EXPORT("_configthreadlocale", nt_configthreadlocale)
};

static const struct runtime_export crt_runtime_exports[] = {
    EXPORT("_c_exit", nt_c_exit),
    EXPORT("_cexit", nt_cexit),
    EXPORT("_configure_narrow_argv", nt_configure_narrow_argv),
    EXPORT("_crt_atexit", nt_crt_atexit),
    EXPORT("_exit", nt_quick_exit),
    EXPORT("_get_narrow_winmain_command_line", nt_get_narrow_winmain_command_line),
    EXPORT("_initialize_narrow_environment", nt_initialize_narrow_environment),
    EXPORT("_initialize_onexit_table", nt_initialize_onexit_table),
    EXPORT("_initterm", nt_initterm),
    EXPORT("_initterm_e", nt_initterm_e),
    EXPORT("_is_c_termination_complete", nt_is_c_termination_complete),
    EXPORT("_register_onexit_function", nt_register_onexit_function),
    EXPORT("_register_thread_local_exe_atexit_callback", nt_register_thread_local_exe_atexit_callback),
    EXPORT("_seh_filter_exe", nt_seh_filter_exe),
    EXPORT("_set_app_type", nt_set_app_type),
    EXPORT("_wassert", nt_wassert),
    EXPORT("exit", nt_exit),
    EXPORT("signal", nt_signal),
    EXPORT("terminate", nt_terminate)
};

static const struct runtime_export stdio_exports[] = {
    EXPORT("__acrt_iob_func", nt_acrt_iob_func),
    EXPORT("__p__commode", nt_p_commode),
    EXPORT("__stdio_common_vfprintf", nt_stdio_vfprintf),
    EXPORT("__stdio_common_vsprintf", nt_stdio_vsprintf),
    EXPORT("__stdio_common_vsscanf", nt_stdio_vsscanf),
    EXPORT("_close", nt_close),
    EXPORT("_fdopen", nt_fdopen),
    EXPORT("_open_osfhandle", nt_open_osfhandle),
    EXPORT("_set_fmode", nt_set_fmode),
    EXPORT("fclose", nt_fclose),
    EXPORT("fopen", nt_fopen),
    EXPORT("fread", nt_fread),
    EXPORT("fwrite", nt_fwrite)
};

static const struct runtime_export utility_exports[] = {
    EXPORT("qsort", crt_qsort)
};

#ifdef NT_HAVE_VKD3D
static const struct runtime_export d3d12_exports[] = {
    EXPORT("D3D12CreateDevice", nt_D3D12CreateDevice),
    EXPORT("D3D12SerializeRootSignature", nt_D3D12SerializeRootSignature),
    EXPORT("D3D12SerializeVersionedRootSignature", nt_D3D12SerializeVersionedRootSignature),
};
static const struct runtime_export dxgi_exports[] = {
    EXPORT("CreateDXGIFactory", nt_CreateDXGIFactory),
    EXPORT("CreateDXGIFactory1", nt_CreateDXGIFactory1),
    EXPORT("CreateDXGIFactory2", nt_CreateDXGIFactory2),
};
static const struct runtime_export d3dcompiler_exports[] = {
    EXPORT("D3DCompile", nt_D3DCompile),
};
static const struct runtime_export dxcompiler_exports[] = {
    EXPORT("DxcCreateInstance", nt_DxcCreateInstance),
};
static const struct runtime_export streamline_exports[] = {
    EXPORT("D3D12CreateDevice", nt_D3D12CreateDevice),
    EXPORT("D3D12SerializeRootSignature", nt_D3D12SerializeRootSignature),
    EXPORT("D3D12SerializeVersionedRootSignature", nt_D3D12SerializeVersionedRootSignature),
    EXPORT("CreateDXGIFactory1", nt_CreateDXGIFactory1),
    EXPORT("CreateDXGIFactory2", nt_CreateDXGIFactory2),
    EXPORT("slInit", nt_slInit),
    EXPORT("slShutdown", nt_slShutdown),
    EXPORT("slSetD3DDevice", nt_slSetD3DDevice),
    EXPORT("slGetNativeInterface", nt_slGetNativeInterface),
    EXPORT("slGetNewFrameToken", nt_slGetNewFrameToken),
    EXPORT("slSetConstants", nt_slSetConstants),
    EXPORT("slSetTagForFrame", nt_slSetTagForFrame),
    EXPORT("slGetFeatureRequirements", nt_slGetFeatureRequirements),
    EXPORT("slIsFeatureSupported", nt_slIsFeatureSupported),
    EXPORT("slSetFeatureLoaded", nt_slSetFeatureLoaded),
    EXPORT("slGetFeatureFunction", nt_slGetFeatureFunction),
    EXPORT("slFreeResources", nt_slFreeResources),
    EXPORT("slEvaluateFeature", nt_slEvaluateFeature),
};
#endif

static const struct {
    const char *name;
    const struct runtime_export *exports;
    size_t count;
} modules[] = {
#define MODULE(name, exports) {name, exports, sizeof(exports) / sizeof(exports[0])}
    MODULE("ntdll.dll", ntdll_exports),
    MODULE("kernel32.dll", kernel32_exports),
    MODULE("user32.dll", user32_exports),
    MODULE("gdi32.dll", gdi32_exports),
    MODULE("msimg32.dll", msimg32_exports),
    MODULE("comdlg32.dll", comdlg32_exports),
    MODULE("vcruntime140.dll", vcruntime_exports),
    MODULE("api-ms-win-crt-convert-l1-1-0.dll", convert_exports),
    MODULE("api-ms-win-crt-environment-l1-1-0.dll", environment_exports),
    MODULE("api-ms-win-crt-heap-l1-1-0.dll", heap_exports),
    MODULE("api-ms-win-crt-math-l1-1-0.dll", math_exports),
    MODULE("api-ms-win-crt-locale-l1-1-0.dll", locale_exports),
    MODULE("api-ms-win-crt-runtime-l1-1-0.dll", crt_runtime_exports),
    MODULE("api-ms-win-crt-stdio-l1-1-0.dll", stdio_exports),
    MODULE("api-ms-win-crt-string-l1-1-0.dll", string_exports),
    MODULE("api-ms-win-crt-utility-l1-1-0.dll", utility_exports),
    MODULE("comctl32.dll", comctl32_exports),
    MODULE("shell32.dll", shell32_exports),
    MODULE("ole32.dll", ole32_exports),
    MODULE("bcrypt.dll", bcrypt_exports),
    MODULE("advapi32.dll", advapi32_exports),
    MODULE("imm32.dll", imm32_exports),
    MODULE("ws2_32.dll", ws2_32_exports),
    MODULE("libfluidsynth-3.dll", fluid_exports),
    MODULE("kernelbase.dll", kernel32_exports),
    MODULE("win32u.dll", win32u_exports),
#ifdef NT_HAVE_VKD3D
    MODULE("d3d12.dll", d3d12_exports),
    MODULE("dxgi.dll", dxgi_exports),
    MODULE("d3dcompiler_47.dll", d3dcompiler_exports),
    MODULE("dxcompiler.dll", dxcompiler_exports),
    MODULE("sl.interposer.dll", streamline_exports),
#endif
#undef MODULE
};

enum { MODULE_COUNT = sizeof(modules) / sizeof(modules[0]), MODULE_BASE = 0x10000000 };
static uint32_t module_references[MODULE_COUNT];
static bool module_pinned[MODULE_COUNT];
static pthread_mutex_t module_lock = PTHREAD_MUTEX_INITIALIZER;

/* API sets are names in the guest's loader namespace. The host module
 * supplies the same exports for IAT resolution and dynamic loading. */
static const char *contract_host(const char *name)
{
    static const struct { const char *contract, *host; } aliases[] = {
#define CORE(contract) {"api-ms-win-core-" contract ".dll", "kernelbase.dll"}
        CORE("errorhandling-l1-1-0"), CORE("errorhandling-l1-1-1"), CORE("errorhandling-l1-1-3"),
        CORE("file-l1-1-0"), CORE("file-l1-2-0"), CORE("file-l1-2-2"), CORE("file-l2-1-2"),
        CORE("handle-l1-1-0"), CORE("heap-l1-1-0"), CORE("heap-l2-1-0"),
        CORE("libraryloader-l1-1-1"), CORE("libraryloader-l1-2-0"), CORE("libraryloader-l1-2-2"),
        CORE("memory-l1-1-0"), CORE("memory-l1-1-4"),
        CORE("processenvironment-l1-1-0"), CORE("processenvironment-l1-2-0"),
        CORE("profile-l1-1-0"), CORE("synch-l1-1-0"), CORE("synch-l1-2-0"), CORE("synch-l1-2-1"),
        CORE("sysinfo-l1-1-0"), CORE("sysinfo-l1-2-0"), CORE("sysinfo-l1-2-1"),
#undef CORE
        {"api-ms-win-core-processthreads-l1-1-0.dll", "kernel32.dll"},
        {"api-ms-win-core-processthreads-l1-1-2.dll", "kernel32.dll"},
        {"api-ms-win-core-processthreads-l1-1-3.dll", "kernel32.dll"},
        {"api-ms-win-core-rtlsupport-l1-1-0.dll", "ntdll.dll"},
        {"api-ms-win-core-rtlsupport-l1-2-0.dll", "ntdll.dll"},
        {"api-ms-win-ntuser-rectangle-l1-1-0.dll", "user32.dll"},
        {"ext-ms-win-ntuser-window-l1-1-0.dll", "user32.dll"},
        {"ext-ms-win-ntuser-window-l1-1-4.dll", "user32.dll"},
        {"ext-ms-win-ntuser-windowclass-l1-1-1.dll", "user32.dll"},
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        if (nt_module_name_equal(name, aliases[i].contract))
            return aliases[i].host;
    }
    return name;
}

static size_t module_index(const char *name)
{
    name = contract_host(name);
    for (size_t i = 0; i < MODULE_COUNT; ++i) {
        if (!nt_module_name_equal(name, modules[i].name))
            continue;
        if (modules[i].exports == fluid_exports && !nt_runtime_fluidsynth_available())
            return MODULE_COUNT;
#ifdef NT_HAVE_VKD3D
        if (modules[i].exports == d3dcompiler_exports && !nt_runtime_d3dcompiler_available())
            return MODULE_COUNT;
        if (modules[i].exports == dxcompiler_exports && !nt_runtime_dxcompiler_available())
            return MODULE_COUNT;
#endif
        return i;
    }
    return MODULE_COUNT;
}

static nt_guest_proc find_export(size_t module, const char *name)
{
    if (module < MODULE_COUNT) {
        for (size_t i = 0; i < modules[module].count; ++i) {
            if (strcmp(name, modules[module].exports[i].name) == 0)
                return modules[module].exports[i].target;
        }
    }
    return NULL;
}

nt_guest_proc nt_runtime_resolve(const char *module, const char *name)
{
    if (module == NULL || name == NULL)
        return NULL;
    return find_export(module_index(module), name);
}

static nt_guest_proc find_ordinal(size_t module, unsigned int ordinal)
{
    if (module < MODULE_COUNT && ordinal != 0) {
        for (size_t i = 0; i < modules[module].count; ++i) {
            if (ordinal == modules[module].exports[i].ordinal)
                return modules[module].exports[i].target;
        }
    }
    return NULL;
}

nt_guest_proc nt_runtime_resolve_ordinal(const char *module, unsigned int ordinal)
{
    return module == NULL ? NULL : find_ordinal(module_index(module), ordinal);
}

static size_t requested_module(const char *name)
{
    if (name == NULL || *name == '\0')
        return MODULE_COUNT;
    static const char system_path[] = "C:\\Windows\\System32\\";
    if (strncasecmp(name, system_path, sizeof(system_path) - 1) == 0)
        name += sizeof(system_path) - 1;
    if (strchr(name, '.') != NULL)
        return module_index(name);
    char with_extension[128];
    const size_t length = strlen(name);
    if (length >= sizeof(with_extension) - sizeof(".dll") + 1)
        return MODULE_COUNT;
    memcpy(with_extension, name, length);
    memcpy(with_extension + length, ".dll", sizeof(".dll"));
    return module_index(with_extension);
}

static struct nt_external_module_backend external_backend;

void nt_runtime_set_external_module_backend(const struct nt_external_module_backend *backend)
{
    /* This is installed by the host before any guest thread starts. */
    external_backend = backend != NULL ? *backend : (struct nt_external_module_backend){0};
}

uintptr_t nt_runtime_external_module_at(uintptr_t address)
{
    return external_backend.at != NULL ?
           external_backend.at(external_backend.context, address) : 0;
}

const char *nt_runtime_external_module_filename(uintptr_t base)
{
    return external_backend.filename != NULL ?
           external_backend.filename(external_backend.context, base) : NULL;
}

uintptr_t NT_MS_ABI nt_LoadLibraryA(const char *name)
{
    const size_t module = requested_module(name);
    if (module == MODULE_COUNT) {
        const uintptr_t loaded = name != NULL && external_backend.load != NULL ?
            external_backend.load(external_backend.context, name) : 0;
        if (loaded == 0)
            nt_SetLastError(NT_ERROR_MOD_NOT_FOUND);
        return loaded;
    }
    pthread_mutex_lock(&module_lock);
    const bool acquired = module_references[module] < UINT32_MAX;
    if (acquired)
        ++module_references[module];
    pthread_mutex_unlock(&module_lock);
    if (!acquired) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return MODULE_BASE + module;
}

static uintptr_t NT_MS_ABI win_GetModuleHandleA(const char *name)
{
    const uintptr_t guest = nt_runtime_guest_module();
    if (name == NULL && guest != 0)
        return guest;
    const size_t module = requested_module(name);
    if (module != MODULE_COUNT)
        return MODULE_BASE + module;
    if (name != NULL && external_backend.find != NULL) {
        const uintptr_t mapped = external_backend.find(external_backend.context, name);
        if (mapped != 0)
            return mapped;
    }
    if (name != NULL && guest != 0) {
        char path[4099];
        const uint32_t length = nt_GetModuleFileNameA(guest, path, sizeof(path));
        if (length != 0 && length < sizeof(path)) {
            const char *leaf = strrchr(path, '\\');
            if (nt_module_name_equal(name, path) || (leaf != NULL && nt_module_name_equal(name, leaf + 1)))
                return guest;
        }
    }
    nt_SetLastError(NT_ERROR_MOD_NOT_FOUND);
    return 0;
}

static bool narrow_module(const uint16_t *name, char out[128])
{
    if (name != NULL) {
        for (size_t i = 0; i < 128 && name[i] <= 0x7f; ++i) {
            out[i] = (char)name[i];
            if (name[i] == 0)
                return true;
        }
    }
    nt_SetLastError(NT_ERROR_MOD_NOT_FOUND);
    return false;
}

uintptr_t NT_MS_ABI nt_GetModuleHandleW(const uint16_t *name)
{
    char narrow[128];
    if (name == NULL)
        return win_GetModuleHandleA(NULL);
    return narrow_module(name, narrow) ? win_GetModuleHandleA(narrow) : 0;
}

static uintptr_t NT_MS_ABI win_LoadLibraryW(const uint16_t *name)
{
    char narrow[128];
    return narrow_module(name, narrow) ? nt_LoadLibraryA(narrow) : 0;
}

static uintptr_t NT_MS_ABI win_LoadLibraryExW(const uint16_t *name, uintptr_t file, uint32_t flags)
{
    if (file != 0 || (flags & ~UINT32_C(0x1800)) != 0) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    return win_LoadLibraryW(name);
}

static size_t module_handle_index(uintptr_t handle)
{
    return handle >= MODULE_BASE && handle - MODULE_BASE < MODULE_COUNT ?
           handle - MODULE_BASE : MODULE_COUNT;
}

static int32_t NT_MS_ABI win_GetModuleHandleExW(uint32_t flags, const uint16_t *name, uintptr_t *handle)
{
    enum { PIN = 1, UNCHANGED_REFCOUNT = 2, FROM_ADDRESS = 4 };
    if (handle == NULL || (flags & ~7u) != 0 || (flags & 3u) == 3) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    *handle = (flags & FROM_ADDRESS) != 0 ?
              nt_runtime_guest_image_at((uintptr_t)name) : nt_GetModuleHandleW(name);
    if (*handle == 0) {
        nt_SetLastError(NT_ERROR_MOD_NOT_FOUND);
        return 0;
    }
    const size_t module = module_handle_index(*handle);
    if (module != MODULE_COUNT && (flags & UNCHANGED_REFCOUNT) == 0) {
        pthread_mutex_lock(&module_lock);
        if ((flags & PIN) != 0)
            module_pinned[module] = true;
        else if (module_references[module] != UINT32_MAX)
            ++module_references[module];
        else
            *handle = 0;
        pthread_mutex_unlock(&module_lock);
        if (*handle == 0)
            nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
    }
    return *handle != 0;
}

int32_t NT_MS_ABI nt_FreeLibrary(uintptr_t handle)
{
    const size_t module = module_handle_index(handle);
    if (module == MODULE_COUNT) {
        const bool released = external_backend.release != NULL &&
                              external_backend.release(external_backend.context, handle);
        if (!released)
            nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return released;
    }
    pthread_mutex_lock(&module_lock);
    const bool acquired = module_references[module] != 0 || module_pinned[module];
    if (module_references[module] != 0)
        --module_references[module];
    pthread_mutex_unlock(&module_lock);
    if (!acquired)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return acquired;
}

nt_guest_proc NT_MS_ABI nt_GetProcAddress(uintptr_t handle, const char *name)
{
    const size_t module = module_handle_index(handle);
    if (module == MODULE_COUNT) {
        if (external_backend.getproc == NULL || external_backend.filename == NULL ||
            external_backend.filename(external_backend.context, handle) == NULL) {
            nt_SetLastError(NT_ERROR_INVALID_HANDLE);
            return NULL;
        }
        const uintptr_t address = external_backend.getproc(
            external_backend.context, handle,
            (uintptr_t)name > UINT16_MAX ? name : NULL,
            (uintptr_t)name > UINT16_MAX ? 0 : (uint16_t)(uintptr_t)name);
        nt_guest_proc target = NULL;
        if (address != 0)
            memcpy(&target, &address, sizeof(target));
        else
            nt_SetLastError(NT_ERROR_PROC_NOT_FOUND);
        return target;
    }
    const nt_guest_proc target = (uintptr_t)name > UINT16_MAX ? find_export(module, name) :
                                 find_ordinal(module, (unsigned int)(uintptr_t)name);
    if (target == NULL)
        nt_SetLastError(NT_ERROR_PROC_NOT_FOUND);
    return target;
}

static int32_t NT_MS_ABI win_QueryPerformanceFrequency(int64_t *frequency)
{
    int64_t ignored;
    if (frequency == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return nt_query_performance_counter(&ignored, frequency) == NT_STATUS_SUCCESS;
}

static void NT_MS_ABI win_OutputDebugStringA(const char *text)
{
    if (text != NULL)
        fputs(text, stderr);
}

static void NT_MS_ABI win_OutputDebugStringW(const uint16_t *text)
{
    if (text == NULL)
        return;
    const size_t length = nt_runtime_wide_length(text, 32767);
    char *message = nt_runtime_utf16_to_utf8(text, length);
    if (message != NULL) {
        fputs(message, stderr);
        free(message);
    }
}

static uintptr_t pointer_cookie;
static pthread_once_t pointer_once = PTHREAD_ONCE_INIT;

static void initialize_pointer_cookie(void)
{
    uintptr_t seed;
    if (getrandom(&seed, sizeof(seed), 0) != (ssize_t)sizeof(seed))
        seed = (uintptr_t)&pointer_cookie ^ (uintptr_t)nt_GetTickCount64();
    pointer_cookie = seed | 1;
}

static void *NT_MS_ABI win_EncodePointer(void *pointer)
{
    pthread_once(&pointer_once, initialize_pointer_cookie);
    const uintptr_t value = (uintptr_t)pointer ^ pointer_cookie;
    return (void *)((value >> 17) | (value << 47));
}

static void *NT_MS_ABI win_DecodePointer(void *pointer)
{
    pthread_once(&pointer_once, initialize_pointer_cookie);
    const uintptr_t value = (uintptr_t)pointer;
    return (void *)(((value << 17) | (value >> 47)) ^ pointer_cookie);
}
