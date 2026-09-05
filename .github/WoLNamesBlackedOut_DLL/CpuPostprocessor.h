#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>

// OpenCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>

#include "CoreTypes.h"

namespace WoLNamesBlackedOut::Core {

/**
 * @brief CPU/OpenCV による後処理クラス
 * ONNX出力のbboxデータを元画像座標に変換し、OpenCVでマスク処理（モザイク/ブラー/Inpaint/単色/透かし）を実行
 */
class CpuPostprocessor {
public:
    CpuPostprocessor();
    ~CpuPostprocessor();

    // コピー禁止
    CpuPostprocessor(const CpuPostprocessor&) = delete;
    CpuPostprocessor& operator=(const CpuPostprocessor&) = delete;

    /**
     * @brief yolo26 出力から検出結果リストを取得（NMS不要）
     * @param output_data ONNX出力データ
     * @param output_shape 出力形状 [batch, dim1, dim2] (通常 [1, 300, 6])
     * @param score_threshold スコア閾値
     * @param frame_width 元画像幅
     * @param frame_height 元画像高さ
     * @param scale_x 前処理のscale_x（bbox逆変換用）
     * @param scale_y 前処理のscale_y（bbox逆変換用）
     * @param pad_left 前処理のpad_left（bbox逆変換用）
     * @param pad_top 前処理のpad_top（bbox逆変換用）
     * @return 検出結果のリスト
     */
    std::vector<Detection> Process(
        const float* output_data,
        const std::vector<int64_t>& output_shape,
        float score_threshold,
        int frame_width,
        int frame_height,
        float scale_x,
        float scale_y,
        int pad_left = 0,
        int pad_top = 0
    );

    /**
     * @brief 検出矩形に対するマスク処理を実行（in-place）
     * @param frame 処理対象のBGRAフレーム（CV_8UC4）
     * @param detections 検出結果リスト
     * @param mask_type マスク種別
     * @param param モザイク因子/ブラー半径/Inpaint半径
     * @param color 塗りつぶし色
     */
    void ApplyMask(
        cv::Mat& frame,
        const std::vector<Detection>& detections,
        MaskType mask_type,
        int param,
        const ColorInfo& color
    );

    /**
     * @brief 固定矩形に対するマスク処理を実行（in-place）
     * @param frame 処理対象のBGRAフレーム（CV_8UC4）
     * @param rects 固定矩形配列
     * @param count 矩形数
     * @param mask_type マスク種別
     * @param param パラメータ
     * @param color 塗りつぶし色
     */
    void ApplyFixedRects(
        cv::Mat& frame,
        const RectInfo* rects,
        int count,
        MaskType mask_type,
        int param,
        const ColorInfo& color
    );

    /**
     * @brief 透かし画像を右下基準（オフセット付き）でアルファブレンド合成（in-place）
     * @param frame 対象BGRAフレーム（CV_8UC4）
     * @param watermark_bgra 透かしBGRA画像（CV_8UC4）
     * @param offset_x X方向オフセット
     * @param offset_y Y方向オフセット
     * @param scale スケール（1.0 = 原寸）
     */
    void ApplyCopyrightOverlay(
        cv::Mat& frame,
        const cv::Mat& watermark_bgra,
        int offset_x,
        int offset_y,
        float scale
    );

    /**
     * @brief 単一ROIへのモザイク処理
     */
    void ApplyMosaic(cv::Mat& frame, const cv::Rect& roi, int factor);

    /**
     * @brief 単一ROIへのブラー処理
     */
    void ApplyBlur(cv::Mat& frame, const cv::Rect& roi, int radius);

    /**
     * @brief 矩形塗りつぶし
     */
    void DrawFilledRect(cv::Mat& frame, const cv::Rect& rect, const ColorInfo& color);
};

} // namespace WoLNamesBlackedOut::Core

