/* SPDX-License-Identifier: MIT */
#include <windows.h>
#include <shlobj.h>
#include <initguid.h>
#include <knownfolders.h>
#include <stdio.h>

int import_fixture_entry(void)
{
    PWSTR directory = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &directory) != S_OK || directory == NULL)
        return 1;
    const char expected[] = "C:\\Users\\guest\\Documents";
    const char suffix[] = "\\shared.txt";
    char path[sizeof(expected) + sizeof(suffix) - 1];
    for (size_t i = 0; i < sizeof(expected); ++i) {
        if (directory[i] != (WCHAR)expected[i]) {
            CoTaskMemFree(directory);
            return 2;
        }
        path[i] = (char)directory[i];
    }
    CoTaskMemFree(directory);
    for (size_t i = 0; i < sizeof(suffix); ++i)
        path[sizeof(expected) - 1 + i] = suffix[i];
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return 3;
    char previous;
    const size_t read = fread(&previous, 1, 1, file);
    if (fclose(file) != 0 || read != 1 || previous != 'L')
        return 4;
    file = fopen(path, "ab");
    if (file == NULL)
        return 5;
    const size_t written = fwrite("W", 1, 1, file);
    const int closed = fclose(file);
    return written == 1 && closed == 0 ? 0 : 6;
}
