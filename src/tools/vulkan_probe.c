/* SPDX-License-Identifier: MIT */
#include "ntnative/vulkan_probe.h"
#include <stdio.h>

int main(void)
{
    struct nt_vulkan_report report;
    nt_vulkan_probe(&report);
    printf("Vulkan loader: %u.%u.%u\n", (report.loader_api_version >> 22) & 0x7fu,
           (report.loader_api_version >> 12) & 0x3ffu, report.loader_api_version & 0xfffu);
    printf("Physical devices: %u; swapchain extension: %u\n", report.physical_devices,
           report.swapchain_devices);
    printf("Status: %s (VkResult %d)\n", nt_vulkan_state_name(report.state),
           report.last_vk_result);
    puts("D3D12CreateDevice is not available. This diagnostic does not implement COM or Vulkan rendering.");
    return 0;
}
