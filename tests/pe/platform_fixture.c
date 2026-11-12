/* SPDX-License-Identifier: MIT */
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

static int test_console(void)
{
    if (AttachConsole(0x7fffffffu) || GetLastError() != ERROR_INVALID_PARAMETER)
        return 11;
    if (!AllocConsole())
        return 12;
    HWND console = GetConsoleWindow();
    if (!console || !GetWindowLongPtrA(console, GWLP_WNDPROC) ||
        AllocConsole() || GetLastError() != ERROR_ACCESS_DENIED)
        return 13;
    DWORD done = 0;
    if (!SetConsoleTitleA("PE console") ||
        !WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), "output\n", 7, &done, NULL) || done != 7)
        return 14;
    SendDlgItemMessageA(console, 101, WM_SETTEXT, 0, (LPARAM)"input");
    SendMessageA(console, WM_COMMAND, IDOK, 0);
    WCHAR wide[16];
    if (!ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), wide, 16, &done, NULL) ||
        done != 6 || wide[0] != L'i' || wide[4] != L't' || wide[5] != L'\n')
        return 15;
    if (!FreeConsole() || GetConsoleWindow() || GetWindowLongPtrA(console, GWLP_WNDPROC))
        return 16;
    return 0;
}

static int test_shell(void)
{
    const WCHAR path[] = L"C:\\test-file.txt";
    SECURITY_ATTRIBUTES attributes_file = {sizeof(attributes_file), NULL, TRUE};
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, &attributes_file, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 21;
    DWORD handle_flags = 0;
    if (!GetHandleInformation(file, &handle_flags) || handle_flags != HANDLE_FLAG_INHERIT)
        return 25;
    CloseHandle(file);
    PIDLIST_ABSOLUTE root = NULL, item = NULL;
    SFGAOF attributes = 0;
    int code = 0;
    if (FAILED(SHParseDisplayName(L"C:\\", NULL, &root, SFGAO_FILESYSTEM | SFGAO_FOLDER, &attributes)) ||
        !root || attributes != (SFGAO_FILESYSTEM | SFGAO_FOLDER))
        code = 22;
    if (!code && (FAILED(SHParseDisplayName(path, NULL, &item, SFGAO_FILESYSTEM | SFGAO_FOLDER, &attributes)) ||
        !item || attributes != SFGAO_FILESYSTEM))
        code = 23;
    PCUITEMID_CHILD child = (PCUITEMID_CHILD)item;
    if (!code && (FAILED(SHOpenFolderAndSelectItems(root, 1, &child, 0)) ||
        (INT_PTR)ShellExecuteW(NULL, L"explore", L"C:\\", NULL, NULL, SW_SHOW) <= 32 ||
        (INT_PTR)ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOW) != SE_ERR_NOASSOC))
        code = 24;
    CoTaskMemFree(item);
    CoTaskMemFree(root);
    DeleteFileW(path);
    return code;
}

int import_fixture_entry(void)
{
    int code = test_console();
    return code ? code : test_shell();
}
