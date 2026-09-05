#pragma once

#include <vector>
#include <string>

// OpenCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>

namespace WoLNamesBlackedOut::Core {

/**
 * @brief CPU/OpenCV による前処理クラス
 * FFmpegから取得したBGRAフレームをYOLO入力用float配列に変換
 * - リサイズ + レターボックス（左上配置、既存パイプラインと同一）
 * - 正規化（×1/255）
 * - NCHW変換
 */
class CpuPreprocessor {
public:
    CpuPreprocessor();
    ~CpuPreprocessor();

    // コピー禁止
    CpuPreprocessor(const CpuPreprocessor&) = delete;
    CpuPreprocessor& operator=(const CpuPreprocessor&) = delete;

    /**
     * @brief 前処理を実行
     * @param bgra_frame 入力BGRAフレーム（BGRA8888形式）
     * @param width 画像幅
     * @param height 画像高さ
     * @param targetWidth 出力幅（モデル入力）
     * @param targetHeight 出力高さ（モデル入力）
     * @param output 出力float配列（NCHW, batch=1, size=3*targetH*targetW）
     * @param scale_x 出力X座標に対する元画像の倍率（後処理用）
     * @param scale_y 出力Y座標に対する元画像の倍率（後処理用）
     * @param pad_left letterboxの左パッドピクセル数（後処理用）
     * @param pad_top letterboxの上パッドピクセル数（後処理用）
     * @return 成功時 true
     */
    bool Process(
        const uint8_t* bgra_frame,
        int width,
        int height,
        int targetWidth,
        int targetHeight,
        std::vector<float>* output,
        float* scale_x,
        float* scale_y,
        int* pad_left,
        int* pad_top
    );

    /**
     * @brief Letterbox変換パラメータを取得（後処理用）
     */
    struct LetterboxParams {
        float scale_x = 1.0f;
        float scale_y = 1.0f;
        int pad_left = 0;
        int pad_top = 0;
    };

    const LetterboxParams& GetLastLetterboxParams() const { return last_letterbox_params_; }

private:
    LetterboxParams last_letterbox_params_;
};

} // namespace WoLNamesBlackedOut::Core
