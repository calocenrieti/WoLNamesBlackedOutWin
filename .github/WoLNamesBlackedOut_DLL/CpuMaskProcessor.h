#pragma once

#include <opencv2/opencv.hpp>
#include <vector>

namespace WoLNamesBlackedOut::Core {

// CPU/OpenCVベースのマスク画像処理クラス
// GPUパイプライン（MaskShader）と同一の処理結果を生成する
class CpuMaskProcessor {
public:
    CpuMaskProcessor() = default;
    ~CpuMaskProcessor() = default;

    // 入力BGRA画像からマスク用のRGBA float32テンソルを生成
    // src_bgra: 入力BGRA画像（CV_8UC4）
    // dstWidth, dstHeight: マスクモデルの入力サイズ
    // 戻り値: RGBA float32データ（dstWidth*dstHeight*4要素、値域[0, 255]）
    std::vector<float> Process(
        const cv::Mat& src_bgra,
        int dstWidth,
        int dstHeight) const;
};

} // namespace WoLNamesBlackedOut::Core
