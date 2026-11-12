/* SPDX-License-Identifier: MIT */
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

static int test_shell(void)
{
    const WCHAR path[] = L"C:\\test-file.txt";
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 21;
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
    return test_shell();
}
