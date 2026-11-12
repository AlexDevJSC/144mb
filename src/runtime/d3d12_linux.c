/* SPDX-License-Identifier: MIT */
#include "graphics_linux.h"
#include "sync.h"

#include <vkd3d_utils.h>

static HRESULT signal_guest_event(HANDLE event)
{
    return nt_SetEvent((uintptr_t)event) ? S_OK : E_INVALIDARG;
}

HRESULT nt_vulkan_adapters_open(struct nt_vulkan_adapters *adapters)
{
    static const struct vkd3d_application_info application = {
        .type = VKD3D_STRUCTURE_TYPE_APPLICATION_INFO,
        .application_name = "nt-native-linux",
        .api_version = VKD3D_API_VERSION_1_2,
    };
    const struct vkd3d_instance_create_info info = {
        .type = VKD3D_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .next = &application,
        .pfn_signal_event = signal_guest_event,
        .wchar_size = sizeof(uint16_t),
        .pfn_vkGetInstanceProcAddr = vkGetInstanceProcAddr,
    };
    *adapters = (struct nt_vulkan_adapters){0};
    HRESULT hr = vkd3d_create_instance(&info, &adapters->instance);
    if (FAILED(hr))
        return hr;

    VkInstance vk = vkd3d_instance_get_vk_instance(adapters->instance);
    VkResult result = vkEnumeratePhysicalDevices(vk, &adapters->count, NULL);
    if (result == VK_SUCCESS && adapters->count) {
        adapters->devices = calloc(adapters->count, sizeof(*adapters->devices));
        if (!adapters->devices) {
            nt_vulkan_adapters_close(adapters);
            return E_OUTOFMEMORY;
        }
        result = vkEnumeratePhysicalDevices(vk, &adapters->count, adapters->devices);
    }
    if (result == VK_SUCCESS)
        return S_OK;

    nt_vulkan_adapters_close(adapters);
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
        return E_OUTOFMEMORY;
    return NT_DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
}

void nt_vulkan_adapters_close(struct nt_vulkan_adapters *adapters)
{
    free(adapters->devices);
    if (adapters->instance)
        vkd3d_instance_decref(adapters->instance);
}

LUID nt_vulkan_adapter_luid(uint32_t index)
{
    /* Identity of an adapter within the Vulkan enumeration exposed to the guest. */
    return (LUID){.LowPart = index + 1, .HighPart = 0x4e540000};
}

static uint32_t default_adapter(const struct nt_vulkan_adapters *adapters)
{
    for (uint32_t i = 0; i < adapters->count; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(adapters->devices[i], &props);
        if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
            return i;
    }
    return 0;
}

int32_t NT_MS_ABI nt_D3D12CreateDevice(void *adapter, uint32_t minimum_level,
                                      const void *iid, void **device)
{
    if (device)
        *device = NULL;
    if (!iid || (minimum_level != D3D_FEATURE_LEVEL_11_0 &&
                 minimum_level != D3D_FEATURE_LEVEL_11_1 &&
                 minimum_level != D3D_FEATURE_LEVEL_12_0 &&
                 minimum_level != D3D_FEATURE_LEVEL_12_1 && minimum_level != 0xc200))
        return E_INVALIDARG;

    struct nt_vulkan_adapters available = {0};
    struct vkd3d_device_create_info info = {
        .type = VKD3D_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .minimum_feature_level = minimum_level,
    };
    if (adapter) {
        if (!nt_dxgi_adapter_info(adapter, &info.instance, &info.vk_physical_device, &info.adapter_luid))
            return E_INVALIDARG;
        info.parent = adapter;
    } else {
        HRESULT hr = nt_vulkan_adapters_open(&available);
        if (FAILED(hr))
            return hr;
        if (!available.count) {
            nt_vulkan_adapters_close(&available);
            return NT_DXGI_ERROR_UNSUPPORTED;
        }
        uint32_t index = default_adapter(&available);
        info.instance = available.instance;
        info.vk_physical_device = available.devices[index];
        info.adapter_luid = nt_vulkan_adapter_luid(index);
    }

    void *probe = NULL;
    HRESULT hr = vkd3d_create_device(&info, iid, device ? device : &probe);
    nt_vulkan_adapters_close(&available);
    if (SUCCEEDED(hr) && !device) {
        IUnknown_Release((IUnknown *)probe);
        return S_FALSE;
    }
    return hr;
}

int32_t NT_MS_ABI nt_D3D12SerializeRootSignature(const void *desc, uint32_t version,
                                                void **blob, void **errors)
{
    if (blob)
        *blob = NULL;
    if (errors)
        *errors = NULL;
    if (!desc || !blob)
        return E_INVALIDARG;
    ID3DBlob *signature = NULL, *messages = NULL;
    HRESULT hr = D3D12SerializeRootSignature(desc, version, &signature, errors ? &messages : NULL);
    *blob = signature;
    if (errors)
        *errors = messages;
    return hr;
}

int32_t NT_MS_ABI nt_D3D12SerializeVersionedRootSignature(const void *desc, void **blob, void **errors)
{
    if (blob)
        *blob = NULL;
    if (errors)
        *errors = NULL;
    if (!desc || !blob)
        return E_INVALIDARG;
    ID3DBlob *signature = NULL, *messages = NULL;
    HRESULT hr = D3D12SerializeVersionedRootSignature(desc, &signature, errors ? &messages : NULL);
    *blob = signature;
    if (errors)
        *errors = messages;
    return hr;
}
