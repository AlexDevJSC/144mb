/* SPDX-License-Identifier: MIT */
#include "ntnative/vulkan_probe.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    assert(nt_vulkan_version_at_least((1u << 22) | (3u << 12), 1, 3));
    assert(!nt_vulkan_version_at_least((1u << 22) | (2u << 12), 1, 3));
    assert(nt_vulkan_version_at_least((2u << 22), 1, 3));
    assert(!nt_vulkan_version_at_least((1u << 22), 2, 0));
    struct nt_vulkan_report probe = {0};
    nt_vulkan_probe(&probe);
    assert(probe.state >= NT_VULKAN_LOADER_MISSING &&
           probe.state <= NT_VULKAN_PRELIMINARY_READY);
    assert(nt_vulkan_state_name(probe.state)[0] != 0);
    const char *expected = getenv("NT_VULKAN_EXPECT_STATE");
    if (expected != NULL) {
        const int wanted = atoi(expected);
        if ((int)probe.state != wanted) {
            fprintf(stderr, "Expected Vulkan state %d, obtained %d\n",
                    wanted, (int)probe.state);
            return 1;
        }
    }
    if (probe.state == NT_VULKAN_PRELIMINARY_READY) {
        assert(nt_vulkan_version_at_least(probe.loader_api_version, 1, 3));
        assert(probe.physical_devices != 0);
        assert(probe.swapchain_devices != 0);
    }
    puts("PASS Vulkan loader preflight and version checks (not a D3D12 device)");
    return 0;
}
