/* SPDX-License-Identifier: MIT */
#include "graphics_linux.h"
#include "desktop.h"

#include <stdatomic.h>

static const GUID iid_unknown = {0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const GUID iid_object = {0xaec22fb8,0x76f3,0x4639,{0x9b,0xe0,0x28,0xeb,0x43,0xa6,0x7a,0x2e}};
static const GUID iid_adapters[] = {
    {0x2411e7e1,0x12ac,0x4ccf,{0xbd,0x14,0x97,0x98,0xe8,0x53,0x4d,0xc0}},
    {0x29038f61,0x3839,0x4626,{0x91,0xfd,0x08,0x68,0x79,0x01,0x1a,0x05}},
};
static const GUID iid_factories[] = {
    {0x7b7166ec,0x21c7,0x44ae,{0xb2,0x1a,0xc9,0xae,0x32,0x1a,0xe3,0x69}},
    {0x770aae78,0xf26f,0x4dba,{0xa8,0x29,0x25,0x3c,0x83,0xd1,0xb3,0x87}},
    {0x50c83a1c,0xe072,0x4c48,{0x87,0xb0,0x36,0x30,0xfa,0x36,0xa6,0xd0}},
    {0x25483823,0xcd46,0x4c7d,{0x86,0xca,0x47,0xaa,0x95,0xb8,0x37,0xbd}},
    {0x1bc6ea02,0xef36,0x464f,{0xbf,0x0c,0x21,0xca,0x39,0xe5,0x16,0x8a}},
    {0x7632e1f5,0xee65,0x4dca,{0x87,0xfd,0x84,0xcd,0x75,0xf8,0x83,0x8d}},
    {0xc1b6694f,0xff09,0x44a9,{0xb0,0x3c,0x77,0x90,0x0a,0x0a,0x1d,0x17}},
};

struct adapter_desc {
    uint16_t description[128];
    UINT vendor, device, subsystem, revision;
    SIZE_T dedicated_video, dedicated_system, shared_system;
    LUID luid;
};
struct adapter_desc1 { struct adapter_desc base; UINT flags; };
_Static_assert(sizeof(struct adapter_desc) == 304, "DXGI_ADAPTER_DESC ABI");
_Static_assert(sizeof(struct adapter_desc1) == 312, "DXGI_ADAPTER_DESC1 ABI");

struct dxgi_factory {
    const struct factory_api *vtable;
    atomic_uint refs;
    atomic_uintptr_t window;
    struct nt_vulkan_adapters adapters;
};
struct dxgi_adapter {
    const struct adapter_api *vtable;
    atomic_uint refs;
    struct dxgi_factory *factory;
    uint32_t index;
};

#define DXGI_OBJECT \
    HRESULT (NT_MS_ABI *query)(IUnknown *, REFIID, void **); \
    ULONG (NT_MS_ABI *add_ref)(IUnknown *); \
    ULONG (NT_MS_ABI *release)(IUnknown *); \
    HRESULT (NT_MS_ABI *set_data)(IUnknown *, REFGUID, UINT, const void *); \
    HRESULT (NT_MS_ABI *set_interface)(IUnknown *, REFGUID, const IUnknown *); \
    HRESULT (NT_MS_ABI *get_data)(IUnknown *, REFGUID, UINT *, void *); \
    HRESULT (NT_MS_ABI *parent)(IUnknown *, REFIID, void **)

struct adapter_api {
    DXGI_OBJECT;
    HRESULT (NT_MS_ABI *outputs)(IUnknown *, UINT, void **);
    HRESULT (NT_MS_ABI *desc)(IUnknown *, struct adapter_desc *);
    HRESULT (NT_MS_ABI *support)(IUnknown *, REFGUID, int64_t *);
    HRESULT (NT_MS_ABI *desc1)(IUnknown *, struct adapter_desc1 *);
};
struct factory_api {
    DXGI_OBJECT;
    HRESULT (NT_MS_ABI *adapters)(IUnknown *, UINT, void **);
    HRESULT (NT_MS_ABI *set_window)(IUnknown *, uintptr_t, UINT);
    HRESULT (NT_MS_ABI *get_window)(IUnknown *, uintptr_t *);
    HRESULT (NT_MS_ABI *swapchain)(IUnknown *, IUnknown *, const void *, void **);
    HRESULT (NT_MS_ABI *create_software)(IUnknown *, uintptr_t, void **);
    HRESULT (NT_MS_ABI *adapters1)(IUnknown *, UINT, void **);
    BOOL (NT_MS_ABI *current)(IUnknown *);
    BOOL (NT_MS_ABI *stereo)(IUnknown *);
    HRESULT (NT_MS_ABI *swapchain_hwnd)(IUnknown *, IUnknown *, uintptr_t, const void *,
                                       const void *, IUnknown *, void **);
    HRESULT (NT_MS_ABI *swapchain_core)(IUnknown *, IUnknown *, IUnknown *, const void *, IUnknown *, void **);
    HRESULT (NT_MS_ABI *shared_luid)(IUnknown *, HANDLE, LUID *);
    HRESULT (NT_MS_ABI *stereo_window)(IUnknown *, uintptr_t, UINT, DWORD *);
    HRESULT (NT_MS_ABI *stereo_event)(IUnknown *, HANDLE, DWORD *);
    void (NT_MS_ABI *stereo_unregister)(IUnknown *, DWORD);
    HRESULT (NT_MS_ABI *occlusion_window)(IUnknown *, uintptr_t, UINT, DWORD *);
    HRESULT (NT_MS_ABI *occlusion_event)(IUnknown *, HANDLE, DWORD *);
    void (NT_MS_ABI *occlusion_unregister)(IUnknown *, DWORD);
    HRESULT (NT_MS_ABI *swapchain_composition)(IUnknown *, IUnknown *, const void *, IUnknown *, void **);
    UINT (NT_MS_ABI *flags)(IUnknown *);
    HRESULT (NT_MS_ABI *by_luid)(IUnknown *, LUID, REFIID, void **);
    HRESULT (NT_MS_ABI *warp)(IUnknown *, REFIID, void **);
    HRESULT (NT_MS_ABI *feature)(IUnknown *, UINT, void *, UINT);
    HRESULT (NT_MS_ABI *by_preference)(IUnknown *, UINT, UINT, REFIID, void **);
};
#undef DXGI_OBJECT

_Static_assert(offsetof(struct adapter_api, desc1) == 10 * sizeof(void *), "IDXGIAdapter1 vtable");
_Static_assert(offsetof(struct factory_api, by_preference) == 29 * sizeof(void *), "IDXGIFactory6 vtable");

static bool supports(REFIID iid, const GUID *interfaces, size_t count)
{
    if (!iid)
        return false;
    if (IsEqualGUID(iid, &iid_unknown) || IsEqualGUID(iid, &iid_object))
        return true;
    for (size_t i = 0; i < count; ++i)
        if (IsEqualGUID(iid, &interfaces[i]))
            return true;
    return false;
}
#define ARRAY_COUNT(a) (sizeof(a) / sizeof(*(a)))

static ULONG NT_MS_ABI factory_ref(IUnknown *object)
{
    struct dxgi_factory *factory = (void *)object;
    return atomic_fetch_add(&factory->refs, 1) + 1;
}

static ULONG NT_MS_ABI factory_release(IUnknown *object)
{
    struct dxgi_factory *factory = (void *)object;
    ULONG left = atomic_fetch_sub(&factory->refs, 1) - 1;
    if (!left) {
        nt_vulkan_adapters_close(&factory->adapters);
        free(factory);
    }
    return left;
}

static ULONG NT_MS_ABI adapter_ref(IUnknown *object)
{
    struct dxgi_adapter *adapter = (void *)object;
    return atomic_fetch_add(&adapter->refs, 1) + 1;
}

static ULONG NT_MS_ABI adapter_release(IUnknown *object)
{
    struct dxgi_adapter *adapter = (void *)object;
    ULONG left = atomic_fetch_sub(&adapter->refs, 1) - 1;
    if (!left) {
        factory_release((void *)adapter->factory);
        free(adapter);
    }
    return left;
}

static HRESULT query_object(IUnknown *object, REFIID iid, void **out,
                            const GUID *interfaces, size_t count)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!supports(iid, interfaces, count))
        return E_NOINTERFACE;
    IUnknown_AddRef(object);
    *out = object;
    return S_OK;
}
static HRESULT NT_MS_ABI factory_query(IUnknown *object, REFIID iid, void **out)
{
    return query_object(object, iid, out, iid_factories, ARRAY_COUNT(iid_factories));
}
static HRESULT NT_MS_ABI adapter_query(IUnknown *object, REFIID iid, void **out)
{
    return query_object(object, iid, out, iid_adapters, ARRAY_COUNT(iid_adapters));
}
static HRESULT NT_MS_ABI factory_parent(IUnknown *object, REFIID iid, void **out)
{
    (void)object; (void)iid;
    if (!out)
        return E_POINTER;
    *out = NULL;
    return E_NOINTERFACE;
}
static HRESULT NT_MS_ABI adapter_parent(IUnknown *object, REFIID iid, void **out)
{
    struct dxgi_adapter *adapter = (void *)object;
    return factory_query((void *)adapter->factory, iid, out);
}

static HRESULT NT_MS_ABI set_data(IUnknown *object, REFGUID guid, UINT size, const void *data)
{
    (void)object; (void)guid; (void)size; (void)data;
    return E_NOTIMPL;
}
static HRESULT NT_MS_ABI set_interface(IUnknown *object, REFGUID guid, const IUnknown *data)
{
    (void)object; (void)guid; (void)data;
    return E_NOTIMPL;
}
static HRESULT NT_MS_ABI get_data(IUnknown *object, REFGUID guid, UINT *size, void *data)
{
    (void)object; (void)guid; (void)data;
    if (!size)
        return E_INVALIDARG;
    *size = 0;
    return DXGI_ERROR_NOT_FOUND;
}

static HRESULT NT_MS_ABI adapter_outputs(IUnknown *object, UINT index, void **out)
{
    (void)object; (void)index;
    if (!out)
        return E_INVALIDARG;
    *out = NULL;
    return DXGI_ERROR_NOT_FOUND;
}
static HRESULT NT_MS_ABI adapter_support(IUnknown *object, REFGUID iid, int64_t *version)
{
    (void)object; (void)iid;
    if (version)
        *version = 0;
    return NT_DXGI_ERROR_UNSUPPORTED;
}

static HRESULT NT_MS_ABI adapter_desc1(IUnknown *object, struct adapter_desc1 *desc)
{
    if (!desc)
        return E_INVALIDARG;
    struct dxgi_adapter *adapter = (void *)object;
    VkPhysicalDevice physical = adapter->factory->adapters.devices[adapter->index];
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceProperties(physical, &props);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    *desc = (struct adapter_desc1){0};
    uint16_t *name = nt_runtime_utf8_to_wide(props.deviceName);
    if (!name)
        return E_OUTOFMEMORY;
    size_t length = nt_runtime_wide_length(name, 127);
    if (length == 127 && name[126] >= 0xd800 && name[126] <= 0xdbff)
        --length;
    memcpy(desc->base.description, name, length * sizeof(*name));
    free(name);
    desc->base.vendor = props.vendorID;
    desc->base.device = props.deviceID;
    desc->base.luid = nt_vulkan_adapter_luid(adapter->index);
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        desc->flags = 2; /* DXGI_ADAPTER_FLAG_SOFTWARE */
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        const VkMemoryHeap *heap = &memory.memoryHeaps[i];
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
            (heap->flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) {
            desc->base.dedicated_video += heap->size;
            continue;
        }
        for (uint32_t j = 0; j < memory.memoryTypeCount; ++j) {
            const VkMemoryType *type = &memory.memoryTypes[j];
            if (type->heapIndex == i && (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                desc->base.shared_system += heap->size;
                break;
            }
        }
    }
    return S_OK;
}
static HRESULT NT_MS_ABI adapter_desc(IUnknown *object, struct adapter_desc *desc)
{
    if (!desc)
        return E_INVALIDARG;
    struct adapter_desc1 full;
    HRESULT hr = adapter_desc1(object, &full);
    if (SUCCEEDED(hr))
        *desc = full.base;
    return hr;
}

static const struct adapter_api adapter_vtable = {
    adapter_query, adapter_ref, adapter_release, set_data, set_interface, get_data, adapter_parent,
    adapter_outputs, adapter_desc, adapter_support, adapter_desc1,
};

bool nt_dxgi_adapter_info(IUnknown *object, struct vkd3d_instance **instance,
                          VkPhysicalDevice *physical, LUID *luid)
{
    if (!object || (const void *)object->lpVtbl != &adapter_vtable)
        return false;
    struct dxgi_adapter *adapter = (void *)object;
    *instance = adapter->factory->adapters.instance;
    *physical = adapter->factory->adapters.devices[adapter->index];
    *luid = nt_vulkan_adapter_luid(adapter->index);
    return true;
}

static HRESULT factory_adapter(struct dxgi_factory *factory, UINT index, REFIID iid, void **out)
{
    if (!out)
        return E_INVALIDARG;
    *out = NULL;
    if (index >= factory->adapters.count)
        return DXGI_ERROR_NOT_FOUND;
    if (!supports(iid, iid_adapters, ARRAY_COUNT(iid_adapters)))
        return E_NOINTERFACE;
    struct dxgi_adapter *adapter = malloc(sizeof(*adapter));
    if (!adapter)
        return E_OUTOFMEMORY;
    adapter->vtable = &adapter_vtable;
    atomic_init(&adapter->refs, 1);
    adapter->factory = factory;
    adapter->index = index;
    factory_ref((void *)factory);
    *out = adapter;
    return S_OK;
}

static HRESULT NT_MS_ABI enum_adapters(IUnknown *object, UINT index, void **out)
{
    return factory_adapter((void *)object, index, &iid_adapters[0], out);
}
static HRESULT NT_MS_ABI enum_adapters1(IUnknown *object, UINT index, void **out)
{
    return factory_adapter((void *)object, index, &iid_adapters[1], out);
}
static HRESULT NT_MS_ABI enum_by_luid(IUnknown *object, LUID luid, REFIID iid, void **out)
{
    const LUID first = nt_vulkan_adapter_luid(0);
    UINT index = luid.HighPart == first.HighPart ? luid.LowPart - first.LowPart : UINT32_MAX;
    return factory_adapter((void *)object, index, iid, out);
}

static uint32_t rank_adapter(VkPhysicalDevice physical, UINT preference)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    switch (props.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return preference == 1 ? 0 : 1;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return preference == 2 ? 0 : 1;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 3;
    default: return 2;
    }
}
static HRESULT NT_MS_ABI enum_preference(IUnknown *object, UINT index, UINT preference, REFIID iid, void **out)
{
    struct dxgi_factory *factory = (void *)object;
    if (!out)
        return E_INVALIDARG;
    *out = NULL;
    if (preference > 2)
        return E_INVALIDARG;
    if (preference == 0)
        return factory_adapter(factory, index, iid, out);
    for (uint32_t rank = 0; rank < 4; ++rank)
        for (uint32_t i = 0; i < factory->adapters.count; ++i)
            if (rank_adapter(factory->adapters.devices[i], preference) == rank && index-- == 0)
                return factory_adapter(factory, i, iid, out);
    return DXGI_ERROR_NOT_FOUND;
}
static HRESULT NT_MS_ABI enum_software(IUnknown *object, REFIID iid, void **out)
{
    struct dxgi_factory *factory = (void *)object;
    if (!out)
        return E_INVALIDARG;
    *out = NULL;
    for (uint32_t i = 0; i < factory->adapters.count; ++i)
        if (rank_adapter(factory->adapters.devices[i], 0) == 3)
            return factory_adapter(factory, i, iid, out);
    return DXGI_ERROR_NOT_FOUND;
}

static HRESULT NT_MS_ABI set_window(IUnknown *object, uintptr_t window, UINT flags)
{
    struct dxgi_factory *factory = (void *)object;
    if ((flags & ~7u) || (window && !nt_runtime_window_valid(window)))
        return NT_DXGI_ERROR_INVALID_CALL;
    atomic_store(&factory->window, window);
    return S_OK;
}
static HRESULT NT_MS_ABI get_window(IUnknown *object, uintptr_t *window)
{
    struct dxgi_factory *factory = (void *)object;
    if (!window)
        return E_INVALIDARG;
    uintptr_t associated = atomic_load(&factory->window);
    *window = nt_runtime_window_valid(associated) ? associated : 0;
    return S_OK;
}
static BOOL NT_MS_ABI factory_current(IUnknown *object)
{
    struct dxgi_factory *factory = (void *)object;
    VkInstance instance = vkd3d_instance_get_vk_instance(factory->adapters.instance);
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS || count != factory->adapters.count)
        return false;
    if (!count)
        return true;
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices)
        return false;
    BOOL same = vkEnumeratePhysicalDevices(instance, &count, devices) == VK_SUCCESS &&
                count == factory->adapters.count && !memcmp(devices, factory->adapters.devices, count * sizeof(*devices));
    free(devices);
    return same;
}

static HRESULT unavailable_output(void **out)
{
    if (!out)
        return E_INVALIDARG;
    *out = NULL;
    return NT_DXGI_ERROR_UNSUPPORTED;
}
static HRESULT NT_MS_ABI swapchain(IUnknown *factory, IUnknown *device, const void *desc, void **out)
{
    (void)factory; (void)device; (void)desc;
    return unavailable_output(out);
}
static HRESULT NT_MS_ABI create_software(IUnknown *factory, uintptr_t module, void **out)
{
    (void)factory; (void)module;
    return unavailable_output(out);
}
static BOOL NT_MS_ABI stereo_enabled(IUnknown *factory)
{
    (void)factory;
    return false;
}
static HRESULT NT_MS_ABI swapchain_hwnd(IUnknown *factory, IUnknown *device, uintptr_t window,
        const void *desc, const void *fullscreen, IUnknown *output, void **out)
{
    (void)window; (void)fullscreen; (void)output;
    return swapchain(factory, device, desc, out);
}
static HRESULT NT_MS_ABI swapchain_core(IUnknown *factory, IUnknown *device, IUnknown *window,
        const void *desc, IUnknown *output, void **out)
{
    (void)window; (void)output;
    return swapchain(factory, device, desc, out);
}
static HRESULT NT_MS_ABI swapchain_composition(IUnknown *factory, IUnknown *device,
        const void *desc, IUnknown *output, void **out)
{
    (void)output;
    return swapchain(factory, device, desc, out);
}
static HRESULT NT_MS_ABI shared_luid(IUnknown *factory, HANDLE resource, LUID *out)
{
    (void)factory; (void)resource;
    if (!out)
        return E_INVALIDARG;
    *out = (LUID){0};
    return NT_DXGI_ERROR_UNSUPPORTED;
}
static HRESULT NT_MS_ABI register_window(IUnknown *factory, uintptr_t window, UINT message, DWORD *cookie)
{
    (void)factory; (void)window; (void)message; (void)cookie;
    return NT_DXGI_ERROR_UNSUPPORTED;
}
static HRESULT NT_MS_ABI register_event(IUnknown *factory, HANDLE event, DWORD *cookie)
{
    (void)factory; (void)event; (void)cookie;
    return NT_DXGI_ERROR_UNSUPPORTED;
}
static void NT_MS_ABI unregister_status(IUnknown *factory, DWORD cookie)
{
    (void)factory; (void)cookie;
}
static UINT NT_MS_ABI factory_flags(IUnknown *factory)
{
    (void)factory;
    return 0;
}
static HRESULT NT_MS_ABI check_feature(IUnknown *factory, UINT feature, void *data, UINT size)
{
    (void)factory;
    if (feature != 0 || !data || size != sizeof(BOOL))
        return E_INVALIDARG;
    *(BOOL *)data = false; /* DXGI_FEATURE_PRESENT_ALLOW_TEARING */
    return S_OK;
}

static const struct factory_api factory_vtable = {
    factory_query, factory_ref, factory_release, set_data, set_interface, get_data, factory_parent,
    enum_adapters, set_window, get_window, swapchain, create_software, enum_adapters1,
    factory_current, stereo_enabled, swapchain_hwnd, swapchain_core, shared_luid,
    register_window, register_event, unregister_status, register_window, register_event, unregister_status,
    swapchain_composition, factory_flags, enum_by_luid, enum_software, check_feature, enum_preference,
};

int32_t NT_MS_ABI nt_CreateDXGIFactory2(uint32_t flags, const void *iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (flags & ~1u)
        return E_INVALIDARG;
    if (flags)
        return NT_DXGI_ERROR_SDK_COMPONENT_MISSING;
    if (!supports(iid, iid_factories, ARRAY_COUNT(iid_factories)))
        return E_NOINTERFACE;
    struct dxgi_factory *factory = calloc(1, sizeof(*factory));
    if (!factory)
        return E_OUTOFMEMORY;
    HRESULT hr = nt_vulkan_adapters_open(&factory->adapters);
    if (FAILED(hr)) {
        free(factory);
        return hr;
    }
    factory->vtable = &factory_vtable;
    atomic_init(&factory->refs, 1);
    atomic_init(&factory->window, 0);
    *out = factory;
    return S_OK;
}
int32_t NT_MS_ABI nt_CreateDXGIFactory1(const void *iid, void **out)
{
    return nt_CreateDXGIFactory2(0, iid, out);
}
int32_t NT_MS_ABI nt_CreateDXGIFactory(const void *iid, void **out)
{
    return nt_CreateDXGIFactory2(0, iid, out);
}
