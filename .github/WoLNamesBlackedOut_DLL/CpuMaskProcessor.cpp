#include "pch.h"
#include "CpuMaskProcessor.h"
#include <algorithm>

namespace WoLNamesBlackedOut::Core {

std::vector<float> CpuMaskProcessor::Process(
    const cv::Mat& src_bgra,
    int dstWidth,
    int dstHeight) const
{
    if (src_bgra.empty() || src_bgra.channels() != 4) {
        return {};
    }
    
    // 1. 左上寄せLetterboxリサイズ（GPUと同一）
    float ratioW = static_cast<float>(dstWidth) / static_cast<float>(src_bgra.cols);
    float ratioH = static_cast<float>(dstHeight) / static_cast<float>(src_bgra.rows);
    float resizeRatio = std::min(ratioW, ratioH);
    
    int resized_w = static_cast<int>(std::round(src_bgra.cols * resizeRatio));
    int resized_h = static_cast<int>(std::round(src_bgra.rows * resizeRatio));
    resized_w = std::max(1, resized_w);
    resized_h = std::max(1, resized_h);
    
    // BGRA → RGBA変換
    cv::Mat rgba;
    cv::cvtColor(src_bgra, rgba, cv::COLOR_BGRA2RGBA);
    
    cv::Mat resized;
    cv::resize(rgba, resized, cv::Size(resized_w, resized_h), 0, 0, cv::INTER_LINEAR);
    
    // 2. 左上寄せパディング
    cv::Mat padded(dstHeight, dstWidth, CV_8UC4, cv::Scalar(0, 0, 0, 255));
    resized.copyTo(padded(cv::Rect(0, 0, resized_w, resized_h)));
    
    // 3. RGBA → float32 [0, 255]
    size_t totalElements = static_cast<size_t>(dstWidth * dstHeight * 4);
    std::vector<float> output(totalElements);
    
    float* outputPtr = output.data();
    const uint8_t* paddedPtr = padded.ptr<uint8_t>();
    int paddedStep = padded.step;
    
    for (int y = 0; y < dstHeight; y++) {
        for (int x = 0; x < dstWidth; x++) {
            const uint8_t* pixel = paddedPtr + y * paddedStep + x * 4;
            outputPtr[0] = static_cast<float>(pixel[0]);   // R
            outputPtr[1] = static_cast<float>(pixel[1]);   // G
            outputPtr[2] = static_cast<float>(pixel[2]);   // B
            outputPtr[3] = static_cast<float>(pixel[3]);   // A
            outputPtr += 4;
        }
    }
    
    return output;
}

} // namespace WoLNamesBlackedOut::Core
