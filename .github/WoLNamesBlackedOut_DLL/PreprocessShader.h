#pragma once

#include "CoreTypes.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>

namespace WoLNamesBlackedOut::Core {

/**
 * @brief D3D11 Compute Shader で画像前処理をGPU上で完結させるクラス
 *
 * 入力: BGRAテクスチャ（元動画解像度）
 * 出力: NCHW float CPUバッファ（モデル入力サイズ）
 *
 * 処理内容:
 *   - Bilinear resize
 *   - Letterbox padding（グレー114/255）
 *   - BGR → RGB 変換
 *   - [0,255] → [0,1] 正規化（/255.0）
 *   - NCHW レイアウト変換
 *
 * 参考: microsoft/DirectML/Samples/yolov4/Assets/ImageToTensor.hlsl
 */
class PreprocessShader {
public:
    PreprocessShader();
    ~PreprocessShader();

    PreprocessShader(const PreprocessShader&) = delete;
    PreprocessShader& operator=(const PreprocessShader&) = delete;

    /**
     * @brief シェーダーとD3D11リソースを初期化
     */
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    /**
     * @brief リソース解放
     */
    void Release();

    /**
     * @brief BGRAテクスチャを前処理してNCHW float CPUバッファを出力
     *
     * @param srcTexture   入力BGRAテクスチャ
     * @param srcWidth     入力幅（ピクセル）
     * @param srcHeight    入力高さ（ピクセル）
     * @param dstWidth     出力幅（モデル入力サイズ）
     * @param dstHeight    出力高さ（モデル入力サイズ）
     * @param outputBuffer 出力NCHWバッファ [1,3,dstHeight,dstWidth]、内部でresize
     * @return 成功時 true
     */
    bool Process(
        ID3D11Texture2D* srcTexture,
        uint32_t srcWidth,
        uint32_t srcHeight,
        uint32_t dstWidth,
        uint32_t dstHeight,
        std::vector<float>& outputBuffer);

    /**
     * @brief BGRAテクスチャを前処理してGPUバッファ（UAV）を出力
     *
     * DirectML EPとのゼロコピー連携用。出力バッファはD3D11_USAGE_DEFAULT。
     *
     * @param srcTexture    入力BGRAテクスチャ
     * @param srcWidth      入力幅
     * @param srcHeight     入力高さ
     * @param dstWidth      出力幅
     * @param dstHeight     出力高さ
     * @param outputBuffer  出力GPUバッファ（UAV書き込み先）
     * @param outputSize    出力バッファサイズ（バイト）
     * @return 成功時 true
     */
    bool ProcessToGpuBuffer(
        ID3D11Texture2D* srcTexture,
        uint32_t srcWidth,
        uint32_t srcHeight,
        uint32_t dstWidth,
        uint32_t dstHeight,
        Microsoft::WRL::ComPtr<ID3D11Buffer>& outputBuffer,
        size_t& outputSize);

    /**
     * @brief BGRAテクスチャを前処理してD3D11On12共有バッファに出力
     *
     * D3D12ゼロコピー連携用。出力バッファはD3D11On12デバイス上のUAV。
     *
     * @param srcTexture    入力BGRAテクスチャ（通常のD3D11デバイス上）
     * @param srcWidth      入力幅
     * @param srcHeight     入力高さ
     * @param dstWidth      出力幅
     * @param dstHeight     出力高さ
     * @param d3d11on12_device  D3D11On12デバイス
     * @param d3d11on12_context D3D11On12コンテキスト
     * @param outputBuffer  出力共有バッファ（D3D11On12デバイス上、UAV書き込み先）
     * @return 成功時 true
     */
    bool ProcessToSharedBuffer(
        ID3D11Texture2D* srcTexture,
        uint32_t srcWidth,
        uint32_t srcHeight,
        uint32_t dstWidth,
        uint32_t dstHeight,
        ID3D11Device* d3d11on12_device,
        ID3D11DeviceContext* d3d11on12_context,
        Microsoft::WRL::ComPtr<ID3D11Buffer>& outputBuffer);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    // Compute Shader
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs_;

    // 定数バッファ
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;

    // サンプラーステート
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerState_;

    // 一時的なUAVバッファ（Process()内で再利用）
    Microsoft::WRL::ComPtr<ID3D11Buffer> uavBuffer_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav_;
    uint32_t uavBufferSize_ = 0;

    // ステージングバッファ（GPU→CPU読み出し用）
    Microsoft::WRL::ComPtr<ID3D11Buffer> stagingBuffer_;
    uint32_t stagingBufferSize_ = 0;

    bool initialized_ = false;

    bool EnsureUavBuffer(uint32_t dstWidth, uint32_t dstHeight);
    bool EnsureStagingBuffer(uint32_t dstWidth, uint32_t dstHeight);
};

} // namespace WoLNamesBlackedOut::Core
