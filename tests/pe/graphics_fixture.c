/* SPDX-License-Identifier: MIT */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdint.h>

/* Some MinGW versions lower the fixture's aggregate initialization to memset. */
void *memset(void *destination, int value, size_t size)
{
    volatile unsigned char *bytes = destination;
    for (size_t i = 0; i < size; ++i)
        bytes[i] = (unsigned char)value;
    return destination;
}

static int root_signatures(ID3D12Device *device)
{
    const D3D12_ROOT_SIGNATURE_DESC desc = {0};
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned = {
        D3D_ROOT_SIGNATURE_VERSION_1_0, {.Desc_1_0 = {0}}
    };
    ID3DBlob *blob = NULL, *errors = NULL;
    if (D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors) != S_OK ||
        !blob || errors || ID3D10Blob_GetBufferSize(blob) <= 32)
        return 20;
    ID3D10Blob_Release(blob);
    blob = NULL;
    if (D3D12SerializeVersionedRootSignature(&versioned, &blob, &errors) != S_OK ||
        !blob || errors || ID3D10Blob_GetBufferSize(blob) <= 32)
        return 21;
    const unsigned char *bytes = ID3D10Blob_GetBufferPointer(blob);
    if (bytes[0] != 'D' || bytes[1] != 'X' || bytes[2] != 'B' || bytes[3] != 'C')
        return 22;
    ID3D12RootSignature *signature = NULL;
    if (FAILED(ID3D12Device_CreateRootSignature(device, 0, bytes, ID3D10Blob_GetBufferSize(blob),
                                               &IID_ID3D12RootSignature, (void **)&signature)) || !signature)
        return 23;
    ID3D12RootSignature_Release(signature);
    ID3D10Blob_Release(blob);
    return 0;
}

static HRESULT create_buffer(ID3D12Device *device, D3D12_HEAP_TYPE type,
                             D3D12_RESOURCE_STATES state, ID3D12Resource **buffer)
{
    const D3D12_HEAP_PROPERTIES properties = {.Type = type, .CreationNodeMask = 1, .VisibleNodeMask = 1};
    const D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = 256, .Height = 1,
        .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };
    return ID3D12Device_CreateCommittedResource(device, &properties, D3D12_HEAP_FLAG_NONE,
        &desc, state, NULL, &IID_ID3D12Resource, (void **)buffer);
}

struct sl_header { const void *next; GUID type; size_t version; };
struct sl_viewport { struct sl_header header; UINT index; };
struct sl_resource { struct sl_header header; UINT type; void *native, *memory, *view; UINT state; };
struct sl_tag { struct sl_header header; const struct sl_resource *resource; UINT type, lifecycle, extent[4]; };
struct sl_token;
struct sl_token_api { UINT (WINAPI *index)(const struct sl_token *); };
struct sl_token { const struct sl_token_api *vtable; struct sl_header header; };

__declspec(dllimport) int WINAPI slInit(const void *, UINT64);
__declspec(dllimport) int WINAPI slShutdown(void);
__declspec(dllimport) int WINAPI slSetD3DDevice(void *);
__declspec(dllimport) int WINAPI slGetNativeInterface(void *, void **);
__declspec(dllimport) int WINAPI slGetNewFrameToken(struct sl_token **, const UINT *);
__declspec(dllimport) int WINAPI slSetConstants(const void *, const void *, const void *);
__declspec(dllimport) int WINAPI slSetTagForFrame(const void *, const void *, const void *, UINT, void *);
__declspec(dllimport) int WINAPI slGetFeatureRequirements(UINT, void *);
__declspec(dllimport) int WINAPI slIsFeatureSupported(UINT, const void *);
__declspec(dllimport) int WINAPI slSetFeatureLoaded(UINT, unsigned char);
__declspec(dllimport) int WINAPI slGetFeatureFunction(UINT, const char *, void **);
__declspec(dllimport) int WINAPI slFreeResources(UINT, const void *);
__declspec(dllimport) int WINAPI slEvaluateFeature(UINT, const void *, const void *, UINT, void *);

static int streamline(ID3D12Device *device)
{
    const struct {
        struct sl_header header;
        UINT64 reserved[7], flags;
        UINT64 reserved2[5];
        UINT render_api;
    } preferences = {
        .header = {NULL,{0x1ca10965,0xbf8e,0x432b,{0x8d,0xa1,0x67,0x16,0xd8,0x79,0xfb,0x14}},1},
        .flags = 128, .render_api = 1,
    };
    const struct sl_viewport viewport = {
        {NULL,{0x171b6435,0x9b3c,0x4fc8,{0x99,0x94,0xfb,0xe5,0x25,0x69,0xaa,0xa4}},1}, 7,
    };
    const struct {
        struct sl_header header;
        unsigned char values[424];
    } constants = {
        .header = {NULL,{0xdcd35ad7,0x4e4a,0x4bad,{0xa9,0x0c,0xe0,0xc4,0x9e,0xb2,0x3a,0xfe}},2},
    };
    struct sl_header requirements = {
        NULL,{0x66714097,0xac6d,0x4bc6,{0x89,0x15,0x1e,0x0f,0x55,0xa6,0xb6,0x1f}},2,
    };
    ID3D12Device *native = NULL;
    struct sl_token *frame = NULL, *same = NULL;
    const UINT index = 42;
    void *function = device;
    if (slGetNewFrameToken(&frame, &index) != 23 ||
        slInit(&preferences, 0x0002000b0001fedcULL) || slSetD3DDevice(device) ||
        slGetNativeInterface(device, (void **)&native) || native != device)
        return 40;
    ID3D12Device_Release(native);
    if (slGetNewFrameToken(&frame, &index) || !frame || frame->vtable->index(frame) != index ||
        slGetNewFrameToken(&same, &index) || same != frame ||
        slSetConstants(&constants, frame, &viewport) || slSetConstants(&constants, frame, &viewport) != 27)
        return 41;
    ID3D12Resource *buffer = NULL;
    if (FAILED(create_buffer(device, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, &buffer)))
        return 42;
    const struct sl_resource resource = {
        {NULL,{0x3a9d70cf,0x2418,0x4b72,{0x83,0x91,0x13,0xf8,0x72,0x1c,0x72,0x61}},1},
        1, buffer, NULL, NULL, D3D12_RESOURCE_STATE_GENERIC_READ,
    };
    struct sl_tag tag = {
        {NULL,{0x4c6a5aad,0xb445,0x496c,{0x87,0xff,0x1a,0xf3,0x84,0x5b,0xe6,0x53}},1},
        &resource, 3, 1, {0},
    };
    if (slSetTagForFrame(frame, &viewport, &tag, 1, NULL))
        return 43;
    ID3D12Resource_Release(buffer);
    tag.resource = NULL;
    if (slSetTagForFrame(frame, &viewport, &tag, 1, NULL) ||
        slGetFeatureRequirements(0, &requirements) != 31 ||
        slIsFeatureSupported(0, NULL) != 31 || slSetFeatureLoaded(0, 1) != 31 ||
        slGetFeatureFunction(0, "slDLSSSetOptions", &function) != 31 || function ||
        slFreeResources(0, &viewport) != 31 || slEvaluateFeature(0, frame, NULL, 0, NULL) != 25 ||
        slEvaluateFeature(0, frame, NULL, 0, device) != 31 ||
        slShutdown() || slGetNewFrameToken(&frame, &index) != 23)
        return 44;
    return 0;
}

static int copy_buffer(ID3D12Device *device)
{
    ID3D12Resource *upload = NULL, *readback = NULL;
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12Fence *fence = NULL;
    void *data = NULL;
    if (FAILED(create_buffer(device, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, &upload)) ||
        FAILED(create_buffer(device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, &readback)) ||
        FAILED(ID3D12Resource_Map(upload, 0, NULL, &data)))
        return 30;
    *(uint32_t *)data = 0x12d3a55a;
    ID3D12Resource_Unmap(upload, 0, NULL);
    const D3D12_COMMAND_QUEUE_DESC queue_desc = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (FAILED(ID3D12Device_CreateCommandQueue(device, &queue_desc, &IID_ID3D12CommandQueue, (void **)&queue)) ||
        FAILED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  &IID_ID3D12CommandAllocator, (void **)&allocator)) ||
        FAILED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
                                             &IID_ID3D12GraphicsCommandList, (void **)&list)) ||
        FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)))
        return 31;
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, upload, 0, sizeof(uint32_t));
    if (FAILED(ID3D12GraphicsCommandList_Close(list)))
        return 32;
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!event || FAILED(ID3D12Fence_SetEventOnCompletion(fence, 1, event)))
        return 33;
    ID3D12CommandList *lists[] = {(ID3D12CommandList *)list};
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    if (FAILED(ID3D12CommandQueue_Signal(queue, fence, 1)) ||
        WaitForSingleObject(event, 5000) != WAIT_OBJECT_0 || ID3D12Fence_GetCompletedValue(fence) != 1)
        return 34;
    if (FAILED(ID3D12Resource_Map(readback, 0, NULL, &data)) || *(const uint32_t *)data != 0x12d3a55a)
        return 35;
    ID3D12Resource_Unmap(readback, 0, NULL);
    CloseHandle(event);
    ID3D12Fence_Release(fence);
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12CommandQueue_Release(queue);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(upload);
    return 0;
}

int import_fixture_entry(void)
{
    IDXGIFactory6 *factory = NULL, *parent = NULL;
    IDXGIFactory1 *factory1 = NULL;
    IDXGIAdapter1 *adapter = NULL, *same_adapter = NULL;
    ID3D12Device *device = NULL;
    DXGI_ADAPTER_DESC1 desc = {0}, same_desc = {0};
    if (CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory1) != S_OK || !factory1)
        return 10;
    IDXGIFactory1_Release(factory1);
    if (CreateDXGIFactory2(0, &IID_IDXGIFactory6, (void **)&factory) != S_OK || !factory ||
        !IDXGIFactory6_IsCurrent(factory))
        return 11;
    if (IDXGIFactory6_EnumAdapterByGpuPreference(factory, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            &IID_IDXGIAdapter1, (void **)&adapter) != S_OK || !adapter ||
        IDXGIAdapter1_GetDesc1(adapter, &desc) != S_OK || !desc.Description[0])
        return 12;
    if (IDXGIAdapter1_GetParent(adapter, &IID_IDXGIFactory6, (void **)&parent) != S_OK || parent != factory ||
        IDXGIFactory6_EnumAdapterByLuid(parent, desc.AdapterLuid, &IID_IDXGIAdapter1,
                                       (void **)&same_adapter) != S_OK || !same_adapter ||
        IDXGIAdapter1_GetDesc1(same_adapter, &same_desc) != S_OK ||
        same_desc.AdapterLuid.LowPart != desc.AdapterLuid.LowPart || same_desc.VendorId != desc.VendorId)
        return 13;
    IDXGIAdapter1_Release(same_adapter);
    IDXGIFactory6_Release(parent);
    IDXGIFactory6_Release(factory);
    if (D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, NULL) != S_FALSE ||
        D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                          (void **)&device) != S_OK || !device)
        return 14;
    IDXGIAdapter1_Release(adapter);
    const LUID luid = ID3D12Device_GetAdapterLuid(device);
    if (ID3D12Device_GetNodeCount(device) != 1 || luid.LowPart != desc.AdapterLuid.LowPart ||
        luid.HighPart != desc.AdapterLuid.HighPart)
        return 15;
    ID3D12Device *same_device = NULL;
    if (ID3D12Device_QueryInterface(device, &IID_IUnknown, (void **)&same_device) != S_OK ||
        same_device != device)
        return 16;
    ID3D12Device_Release(same_device);
    int status = root_signatures(device);
    if (!status)
        status = copy_buffer(device);
    if (!status)
        status = streamline(device);
    ID3D12Device_Release(device);
    return status;
}
