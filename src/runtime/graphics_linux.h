/* SPDX-License-Identifier: MIT */
#ifndef NT_RUNTIME_GRAPHICS_LINUX_H
#define NT_RUNTIME_GRAPHICS_LINUX_H

#include "graphics.h"

#define COBJMACROS
#define VKD3D_WIN32_WCHAR
#include <vkd3d.h>

#define NT_DXGI_ERROR_INVALID_CALL ((HRESULT)0x887a0001)
#define NT_DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887a0004)
#define NT_DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ((HRESULT)0x887a0022)
#define NT_DXGI_ERROR_SDK_COMPONENT_MISSING ((HRESULT)0x887a002d)

struct nt_vulkan_adapters {
    struct vkd3d_instance *instance;
    VkPhysicalDevice *devices;
    uint32_t count;
};

HRESULT nt_vulkan_adapters_open(struct nt_vulkan_adapters *adapters);
void nt_vulkan_adapters_close(struct nt_vulkan_adapters *adapters);
LUID nt_vulkan_adapter_luid(uint32_t index);
bool nt_dxgi_adapter_info(IUnknown *object, struct vkd3d_instance **instance,
                          VkPhysicalDevice *physical, LUID *luid);

#endif
