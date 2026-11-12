/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_VULKAN_PROBE_H
#define NTNATIVE_VULKAN_PROBE_H
#include <stdint.h>

enum nt_vulkan_state {
    NT_VULKAN_LOADER_MISSING = 0,
    NT_VULKAN_SYMBOL_MISSING,
    NT_VULKAN_LOADER_ERROR,
    NT_VULKAN_NO_PHYSICAL_DEVICE,
    NT_VULKAN_NO_SWAPCHAIN,
    NT_VULKAN_API_BELOW_1_3,
    NT_VULKAN_PRELIMINARY_READY
};

struct nt_vulkan_report {
    enum nt_vulkan_state state;
    uint32_t loader_api_version;
    uint32_t physical_devices;
    uint32_t swapchain_devices;
    int32_t last_vk_result;
};

/* Host-only Vulkan preflight; NOT a D3D12 implementation or proof of device support. */
void nt_vulkan_probe(struct nt_vulkan_report *result);
const char *nt_vulkan_state_name(enum nt_vulkan_state state);
int nt_vulkan_version_at_least(uint32_t version, unsigned major, unsigned minor);
#endif
