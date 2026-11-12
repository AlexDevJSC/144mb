/* SPDX-License-Identifier: MIT */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdint.h>

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
    ID3D12Device_Release(device);
    return status;
}
