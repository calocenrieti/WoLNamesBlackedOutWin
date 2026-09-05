#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <mutex>
#include <atomic>

namespace WoLNamesBlackedOut::Core {

/**
 * @brief ハードウェアアクセラレータの利用可否を判定するクラス
 * D3D_FEATURE_LEVEL_11_0以上の判定と、ONNX Runtime EP選択に使用
 */
class HardwareAcceleratorChecker {
public:
    /**
     * @brief エンジン
     * GPU: Compute Shader + DirectML推論
     * NPU: NPU推論 + CPU前処理/後処理
     * CPU: 全工程CPU/OpenCV
     */
    enum class Engine {
        GPU,
        NPU,
        CPU,
        Unknown
    };

    HardwareAcceleratorChecker();
    ~HardwareAcceleratorChecker();

    // コピー禁止
    HardwareAcceleratorChecker(const HardwareAcceleratorChecker&) = delete;
    HardwareAcceleratorChecker& operator=(const HardwareAcceleratorChecker&) = delete;

    /**
     * @brief Compute Shaderが利用可能か判定
     * @return D3D_FEATURE_LEVEL_11_0以上なら true
     */
    bool IsComputeShaderAvailable() const { return compute_shader_available_.load(); }

    /**
     * @brief GPUハードウェアが利用可能か判定
     * @return D3D_DRIVER_TYPE_HARDWARE でデバイス作成できれば true
     */
    bool IsGpuHardwareAvailable() const { return gpu_available_.load(); }

    /**
     * @brief 推奨エンジンを取得
     * GPU利用可能ならGPU、否则NPU、否则CPUを返す
     */
    Engine GetRecommendedEngine() const;

    /**
     * @brief GPUベンダー名を取得（"NVIDIA", "AMD", "Intel", など）
     * 空文字列の場合はGPUが利用できない
     */
    std::string GetGpuVendorName() const;

    /**
     * @brief NPUが利用可能か判定（Windows NPU API）
     */
    bool IsNpuAvailable() const;

private:
    std::atomic<bool> compute_shader_available_{ false };
    std::atomic<bool> gpu_available_{ false };
    std::atomic<bool> npu_available_{ false };
    std::string gpu_vendor_name_;
    mutable std::mutex init_mutex_;
    mutable bool initialized_ = false;

    /**
     * @brief 初期化（初回呼び出し時に自動実行）
     */
    void InitializeIfNeeded() const;

    /**
     * @brief D3D11デバイス作成を試み、機能レベルとGPU情報を取得
     */
    void CheckGpuCapabilities();

    /**
     * @brief Windows NPU API（Windows.AI.MachineLearning）でNPU利用可否を判定
     */
    void CheckNpuCapabilities();
};

} // namespace WoLNamesBlackedOut::Core
