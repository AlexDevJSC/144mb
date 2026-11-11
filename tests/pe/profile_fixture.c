/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>

static int append_path(char *out, size_t capacity, const char *directory, const char *name)
{
    size_t used = 0;
    while (*directory != '\0' && used + 1 < capacity)
        out[used++] = *directory++;
    while (*name != '\0' && used + 1 < capacity)
        out[used++] = *name++;
    out[used] = '\0';
    return *directory == '\0' && *name == '\0';
}

int import_fixture_entry(void)
{
    const char *roaming = getenv("APPDATA");
    const char *local = getenv("LOCALAPPDATA");
    const char *profile = getenv("USERPROFILE");
    const char *temp = getenv("TEMP");
    if (roaming == NULL || local == NULL || profile == NULL || temp == NULL ||
        roaming[0] != 'C' || roaming[1] != ':')
        return 1;
    char path[512];
    if (!append_path(path, sizeof(path), roaming, "\\nt-native-profile-fixture.dat"))
        return 2;
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        char previous[8];
        const size_t count = fread(previous, 1, sizeof(previous), file);
        if (fclose(file) != 0)
            return 3;
        for (size_t i = 0; i < count; ++i) {
            if (previous[i] != 'P')
                return 4;
        }
    }
    file = fopen(path, "ab");
    if (file == NULL)
        return 5;
    const size_t written = fwrite("P", 1, 1, file);
    const int closed = fclose(file);
    return written == 1 && closed == 0 ? 0 : 6;
}
