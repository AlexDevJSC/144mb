/* SPDX-License-Identifier: MIT */
#define _WIN32_WINNT 0x0a00
#include <windows.h>

/* Executed on Linux by the project's general AMD64 PE loader.
 * SDL offscreen supplies a deterministic virtual display. */
_Static_assert(sizeof(DISPLAYCONFIG_PATH_INFO) == 72, "DISPLAYCONFIG_PATH_INFO ABI");
_Static_assert(sizeof(DISPLAYCONFIG_MODE_INFO) == 64, "DISPLAYCONFIG_MODE_INFO ABI");
_Static_assert(sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME) == 84, "SOURCE_DEVICE_NAME ABI");
_Static_assert(sizeof(DISPLAYCONFIG_TARGET_DEVICE_NAME) == 420, "TARGET_DEVICE_NAME ABI");

int import_fixture_entry(void)
{
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS)
        return 11;
    if (paths == 0 || paths > 64 || modes != paths * 2)
        return 12;
    if (GetDisplayConfigBufferSizes(0, &paths, &modes) != ERROR_INVALID_PARAMETER)
        return 13;

    DISPLAYCONFIG_PATH_INFO path[64] = {0};
    DISPLAYCONFIG_MODE_INFO mode[128] = {0};
    UINT32 short_paths = 0, short_modes = 0;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &short_paths, path,
                           &short_modes, mode, NULL) != ERROR_INSUFFICIENT_BUFFER)
        return 14;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path,
                           &modes, mode, NULL) != ERROR_SUCCESS)
        return 15;

    for (UINT32 i = 0; i < paths; ++i) {
        if (!(path[i].flags & DISPLAYCONFIG_PATH_ACTIVE) ||
            path[i].sourceInfo.id != i || path[i].targetInfo.id != i ||
            path[i].sourceInfo.modeInfoIdx != i * 2 ||
            path[i].targetInfo.modeInfoIdx != i * 2 + 1 ||
            mode[i * 2].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
            mode[i * 2 + 1].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET ||
            mode[i * 2].sourceMode.width == 0 || mode[i * 2].sourceMode.height == 0)
            return 16;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {0};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.id = i;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            source.viewGdiDeviceName[0] != L'\\')
            return 17;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target = {0};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.id = i;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS)
            return 18;
        target.header.size -= 1;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_INVALID_PARAMETER)
            return 19;
    }
    UINT32 topology = 0;
    if (QueryDisplayConfig(QDC_DATABASE_CURRENT, &paths, path,
                           &modes, mode, &topology) != ERROR_SUCCESS || topology == 0)
        return 20;
    return 0;
}
