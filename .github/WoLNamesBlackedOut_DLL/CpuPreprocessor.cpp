#include "pch.h"
#include "CpuPreprocessor.h"
#include <cmath>
#include <algorithm>

namespace WoLNamesBlackedOut::Core {

CpuPreprocessor::CpuPreprocessor() = default;
CpuPreprocessor::~CpuPreprocessor() = default;

bool CpuPreprocessor::Process(
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
) {
    if (!bgra_frame || width <= 0 || height <= 0 || targetWidth <= 0 || targetHeight <= 0 ||
        !output || !scale_x || !scale_y || !pad_left || !pad_top) {
        return false;
    }

    // 1. BGRA Mat（FFmpegからの入力はBGRA形式）→ BGR 3チャンネルに変換
    cv::Mat bgraMat(height, width, CV_8UC4, const_cast<uint8_t*>(bgra_frame));
    cv::Mat bgrMat;
    cv::cvtColor(bgraMat, bgrMat, cv::COLOR_BGRA2BGR);

    // 2. PreprocessShader と同一のアスペクト比維持リサイズ倍率計算（左上寄せ）
    float ratioW = static_cast<float>(targetWidth) / static_cast<float>(width);
    float ratioH = static_cast<float>(targetHeight) / static_cast<float>(height);
    float ratio = (std::min)(ratioW, ratioH);

    int resizedWidth = std::max(1, static_cast<int>(std::round(width * ratio)));
    int resizedHeight = std::max(1, static_cast<int>(std::round(height * ratio)));
    resizedWidth = (std::min)(resizedWidth, targetWidth);
    resizedHeight = (std::min)(resizedHeight, targetHeight);

    cv::Mat resizedMat;
    cv::resize(bgrMat, resizedMat, cv::Size(resizedWidth, resizedHeight), 0, 0, cv::INTER_LINEAR);

    // 3. 左上に配置した黒背景のテンプレート（BGR）
    cv::Mat paddedMat = cv::Mat::zeros(targetHeight, targetWidth, CV_8UC3);
    resizedMat.copyTo(paddedMat(cv::Rect(0, 0, resizedWidth, resizedHeight)));

    // 4. Letterbox パラメータ（後処理の元画像座標復元用）
    // srcX = (dstX - pad_left) / ratio -> srcX = (dstX - pad_left) * scale_x
    *scale_x = (ratio > 0.0f) ? (1.0f / ratio) : 1.0f;
    *scale_y = (ratio > 0.0f) ? (1.0f / ratio) : 1.0f;
    *pad_left = 0;
    *pad_top = 0;

    last_letterbox_params_.scale_x = *scale_x;
    last_letterbox_params_.scale_y = *scale_y;
    last_letterbox_params_.pad_left = *pad_left;
    last_letterbox_params_.pad_top = *pad_top;

    // 5. cv::dnn::blobFromImage で高速に [0.0, 1.0] 正規化 + BGR->RGB 変換 + NCHW 形式 float32 配列生成
    cv::Mat blob = cv::dnn::blobFromImage(
        paddedMat,
        1.0 / 255.0,
        cv::Size(targetWidth, targetHeight),
        cv::Scalar(),
        true,   // swapRB (BGR -> RGB)
        false,  // crop
        CV_32F
    );

    const size_t totalSize = static_cast<size_t>(targetWidth) * targetHeight * 3;
    output->resize(totalSize);
    std::memcpy(output->data(), blob.ptr<float>(), totalSize * sizeof(float));

    return true;
}

} // namespace WoLNamesBlackedOut::Core

