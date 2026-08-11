#pragma once

#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include <memory>

namespace WoLNamesBlackedOut::Core {

/**
 * @brief D3D11On12 + D3D12 共有リソース管理クラス
 * 
 * D3D11 Compute Shader の出力を D3D12 リソースとして ONNX Runtime に直接渡すための
 * ゼロコピー機構を提供する。
 */
class D3D12SharedResourceManager {
public:
    D3D12SharedResourceManager();
    ~D3D12SharedResourceManager();

    D3D12SharedResourceManager(const D3D12SharedResourceManager&) = delete;
    D3D12SharedResourceManager& operator=(const D3D12SharedResourceManager&) = delete;

    /**
     * @brief D3D11デバイスからD3D11On12デバイスを作成
     * @param d3d11_device 既存のD3D11デバイス
     * @param d3d11_context 既存のD3D11コンテキスト
     * @return 成功時 true
     */
    bool Initialize(ID3D11Device* d3d11_device, ID3D11DeviceContext* d3d11_context);

    /**
     * @brief リソース解放
     */
    void Release();

    /**
     * @brief D3D12共有リソースを作成（D3D11からアクセス可能）
     * 
     * D3D12リソースを共有ハンドル付きで作成し、D3D11側でOpenSharedResource1で開ける。
     * これによりD3D11 Compute Shaderの出力をD3D12リソースとしてONNX Runtimeに直接渡せる。
     * 
     * @param size_bytes バッファサイズ（バイト）
     * @param d3d12_resource 出力: D3D12リソース（ONNX Runtime入力用）
     * @param shared_handle 出力: 共有ハンドル（D3D11でOpenSharedResource1する際に使用）
     * @return 成功時 true
     */
    bool CreateSharedBuffer(
        size_t size_bytes,
        Microsoft::WRL::ComPtr<ID3D12Resource>& d3d12_resource,
        HANDLE& shared_handle
    );

    /**
     * @brief 共有ハンドルをクローズ
     */
    void CloseSharedHandle(HANDLE& shared_handle);

    /**
     * @brief D3D11→D3D12 リソース状態を同期
     * D3D11で書き込み後、D3D12で読み取り前に呼び出す
     */
    void SyncD3D11ToD3D12();

    /**
     * @brief GPU処理が完了するまで待機
     * @return 成功時 true
     */
    bool WaitForGpu();

    /**
     * @brief D3D12デバイスを取得
     */
    ID3D12Device* GetD3D12Device() const { return d3d12_device_.Get(); }

    /**
     * @brief D3D11On12デバイスを取得
     */
    ID3D11Device* GetD3D11On12Device() const { return d3d11on12_device_.Get(); }

    /**
     * @brief D3D11On12コンテキストを取得
     */
    ID3D11DeviceContext* GetD3D11On12Context() const { return d3d11on12_context_.Get(); }

    /**
     * @brief 初期化済みか
     */
    bool IsInitialized() const { return initialized_; }

private:
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12_device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12_command_queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> d3d12_command_allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> d3d12_command_list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> d3d12_fence_;
    UINT64 fence_value_ = 0;
    HANDLE fence_event_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> d3d11on12_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11on12_context_;
    Microsoft::WRL::ComPtr<ID3D11On12Device> d3d11on12_device_interface_;

    bool initialized_ = false;
};

} // namespace WoLNamesBlackedOut::Core
