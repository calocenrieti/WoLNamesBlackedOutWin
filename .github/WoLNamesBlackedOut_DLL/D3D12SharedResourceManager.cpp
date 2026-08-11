#include "pch.h"
#include "D3D12SharedResourceManager.h"
#include <iostream>

namespace WoLNamesBlackedOut::Core {

D3D12SharedResourceManager::D3D12SharedResourceManager() = default;

D3D12SharedResourceManager::~D3D12SharedResourceManager() {
    Release();
}

bool D3D12SharedResourceManager::Initialize(ID3D11Device* d3d11_device, ID3D11DeviceContext* d3d11_context) {
    if (!d3d11_device || !d3d11_context) {
        std::cerr << "[D3D12SharedResourceManager] Invalid D3D11 device/context" << std::endl;
        return false;
    }

    if (initialized_) {
        return true;
    }

    // 1. D3D11デバイスからアダプターを取得
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = d3d11_device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (FAILED(hr)) {
        std::cerr << "[D3D12SharedResourceManager] QueryInterface IDXGIDevice failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgi_adapter;
    hr = dxgi_device->GetAdapter(&dxgi_adapter);
    if (FAILED(hr)) {
        std::cerr << "[D3D12SharedResourceManager] GetAdapter failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // 2. D3D12デバイスを作成
    hr = D3D12CreateDevice(
        dxgi_adapter.Get(),
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&d3d12_device_)
    );
    if (FAILED(hr) || !d3d12_device_) {
        std::cerr << "[D3D12SharedResourceManager] D3D12CreateDevice failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // 3. D3D12コマンドキューを作成
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = d3d12_device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&d3d12_command_queue_));
    if (FAILED(hr)) {
        std::cerr << "[D3D12SharedResourceManager] CreateCommandQueue failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // 4. コマンドアロケーターとコマンドリストを作成
    hr = d3d12_device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&d3d12_command_allocator_)
    );
    if (FAILED(hr)) {
        std::cerr << "[D3D12SharedResourceManager] CreateCommandAllocator failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = d3d12_device_->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        d3d12_command_allocator_.Get(),
        nullptr,
        IID_PPV_ARGS(&d3d12_command_list_)
    );
    if (FAILED(hr)) {
        std::cerr << "[D3D12SharedResourceManager] CreateCommandList failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // コマンドリストは作成時にオープン状態なので、一旦クローズ
    d3d12_command_list_->Close();

    // 5. フェンスを作成
    hr = d3d12_device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d3d12_fence_));
    if (FAILED(hr)) {
        std::cerr << "[D3D12SharedResourceManager] CreateFence failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
        std::cerr << "[D3D12SharedResourceManager] CreateEvent failed" << std::endl;
        return false;
    }

    // 6. D3D11On12デバイスを作成
    IUnknown* const queues[] = { d3d12_command_queue_.Get() };
    hr = D3D11On12CreateDevice(
        d3d12_device_.Get(),
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        queues,
        ARRAYSIZE(queues),
        0,
        &d3d11on12_device_,
        &d3d11on12_context_,
        nullptr
    );
    if (FAILED(hr) || !d3d11on12_device_) {
        std::cerr << "[D3D12SharedResourceManager] D3D11On12CreateDevice failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // 7. ID3D11On12Device インターフェースを取得
    hr = d3d11on12_device_->QueryInterface(IID_PPV_ARGS(&d3d11on12_device_interface_));
    if (FAILED(hr)) {
        std::cerr << "[D3D12SharedResourceManager] QueryInterface ID3D11On12Device failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    initialized_ = true;
    std::cout << "[D3D12SharedResourceManager] Initialized successfully" << std::endl;
    return true;
}

void D3D12SharedResourceManager::Release() {
    if (fence_event_) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }

    d3d11on12_device_interface_.Reset();
    d3d11on12_context_.Reset();
    d3d11on12_device_.Reset();

    d3d12_command_list_.Reset();
    d3d12_command_allocator_.Reset();
    d3d12_command_queue_.Reset();
    d3d12_fence_.Reset();
    d3d12_device_.Reset();

    initialized_ = false;
    fence_value_ = 0;
}

bool D3D12SharedResourceManager::CreateSharedBuffer(
    size_t size_bytes,
    Microsoft::WRL::ComPtr<ID3D12Resource>& d3d12_resource,
    HANDLE& shared_handle) {

    if (!initialized_ || !d3d12_device_) {
        std::cerr << "[D3D12SharedResourceManager] Not initialized" << std::endl;
        return false;
    }

    // 既存の共有ハンドルをクローズ
    if (shared_handle != nullptr) {
        CloseHandle(shared_handle);
        shared_handle = nullptr;
    }

    // アライメント調整（256バイト境界）
    size_t aligned_size = (size_bytes + 255) & ~255;

    // D3D12 リソースを作成（DEFAULT heap、UAV可能、共有可能）
    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC resource_desc = {};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Alignment = 0;
    resource_desc.Width = aligned_size;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT hr = d3d12_device_->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_SHARED,
        &resource_desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&d3d12_resource)
    );
    if (FAILED(hr) || !d3d12_resource) {
        std::cerr << "[D3D12SharedResourceManager] CreateCommittedResource failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // 共有ハンドルを作成（NTハンドル）
    hr = d3d12_device_->CreateSharedHandle(
        d3d12_resource.Get(),
        nullptr,
        GENERIC_ALL,
        nullptr,
        &shared_handle
    );
    if (FAILED(hr) || !shared_handle) {
        std::cerr << "[D3D12SharedResourceManager] CreateSharedHandle failed: 0x" << std::hex << hr << std::endl;
        d3d12_resource.Reset();
        return false;
    }

    return true;
}

void D3D12SharedResourceManager::CloseSharedHandle(HANDLE& shared_handle) {
    if (shared_handle != nullptr) {
        CloseHandle(shared_handle);
        shared_handle = nullptr;
    }
}

void D3D12SharedResourceManager::SyncD3D11ToD3D12() {
    if (!initialized_ || !d3d11on12_context_) {
        return;
    }
    // D3D11On12: D3D11 コマンドを D3D12 コマンドキューにフラッシュ
    d3d11on12_context_->Flush();
}

bool D3D12SharedResourceManager::WaitForGpu() {
    if (!initialized_ || !d3d12_command_queue_ || !d3d12_fence_) {
        return false;
    }

    // フェンス値をインクリメント
    UINT64 fence_value = ++fence_value_;

    // コマンドキューにフェンスシグナルを送信
    HRESULT hr = d3d12_command_queue_->Signal(d3d12_fence_.Get(), fence_value);
    if (FAILED(hr)) {
        return false;
    }

    // フェンスがシグナルされるまで待機
    if (d3d12_fence_->GetCompletedValue() < fence_value) {
        hr = d3d12_fence_->SetEventOnCompletion(fence_value, fence_event_);
        if (FAILED(hr)) {
            return false;
        }
        WaitForSingleObject(fence_event_, INFINITE);
    }

    return true;
}

} // namespace WoLNamesBlackedOut::Core
