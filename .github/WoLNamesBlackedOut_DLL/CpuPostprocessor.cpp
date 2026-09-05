#include "pch.h"
#include "CpuPostprocessor.h"
#include <algorithm>
#include <cmath>

namespace WoLNamesBlackedOut::Core {

CpuPostprocessor::CpuPostprocessor() = default;
CpuPostprocessor::~CpuPostprocessor() = default;

std::vector<Detection> CpuPostprocessor::Process(
    const float* output_data,
    const std::vector<int64_t>& output_shape,
    float score_threshold,
    int frame_width,
    int frame_height,
    float scale_x,
    float scale_y,
    int pad_left,
    int pad_top
) {
    std::vector<Detection> detections;
    if (!output_data || output_shape.size() < 2 || frame_width <= 0 || frame_height <= 0) {
        return detections;
    }

    const float max_w = static_cast<float>(frame_width);
    const float max_h = static_cast<float>(frame_height);

    if (output_shape.size() == 3) {
        int64_t dim1 = output_shape[1];
        int64_t dim2 = output_shape[2];

        // yolo26 形式: (1, 300, 6) = [x1, y1, x2, y2, score, class_id]
        if (dim2 == 6) {
            detections.reserve(dim1);
            for (int64_t i = 0; i < dim1; ++i) {
                const float* det = output_data + (i * 6);
                float score = det[4];
                if (score < score_threshold) continue;

                float x1 = (det[0] - static_cast<float>(pad_left)) * scale_x;
                float y1 = (det[1] - static_cast<float>(pad_top)) * scale_y;
                float x2 = (det[2] - static_cast<float>(pad_left)) * scale_x;
                float y2 = (det[3] - static_cast<float>(pad_top)) * scale_y;

                Detection d;
                d.class_id = static_cast<int>(det[5]);
                d.score = score;
                d.x1 = std::clamp((std::min)(x1, x2), 0.0f, max_w);
                d.y1 = std::clamp((std::min)(y1, y2), 0.0f, max_h);
                d.x2 = std::clamp((std::max)(x1, x2), 0.0f, max_w);
                d.y2 = std::clamp((std::max)(y1, y2), 0.0f, max_h);

                if ((d.x2 - d.x1) > 0.0f && (d.y2 - d.y1) > 0.0f) {
                    detections.push_back(d);
                }
            }
            return detections;
        }

        // 転置形式: (1, 6, 300)
        if (dim1 == 6) {
            detections.reserve(dim2);
            for (int64_t i = 0; i < dim2; ++i) {
                float score = output_data[4 * dim2 + i];
                if (score < score_threshold) continue;

                float x1 = (output_data[0 * dim2 + i] - static_cast<float>(pad_left)) * scale_x;
                float y1 = (output_data[1 * dim2 + i] - static_cast<float>(pad_top)) * scale_y;
                float x2 = (output_data[2 * dim2 + i] - static_cast<float>(pad_left)) * scale_x;
                float y2 = (output_data[3 * dim2 + i] - static_cast<float>(pad_top)) * scale_y;

                Detection d;
                d.class_id = static_cast<int>(output_data[5 * dim2 + i]);
                d.score = score;
                d.x1 = std::clamp((std::min)(x1, x2), 0.0f, max_w);
                d.y1 = std::clamp((std::min)(y1, y2), 0.0f, max_h);
                d.x2 = std::clamp((std::max)(x1, x2), 0.0f, max_w);
                d.y2 = std::clamp((std::max)(y1, y2), 0.0f, max_h);

                if ((d.x2 - d.x1) > 0.0f && (d.y2 - d.y1) > 0.0f) {
                    detections.push_back(d);
                }
            }
            return detections;
        }
    }

    return detections;
}

void CpuPostprocessor::ApplyMosaic(cv::Mat& frame, const cv::Rect& roi, int factor) {
    if (frame.empty() || roi.width <= 0 || roi.height <= 0) return;
    cv::Rect safe_roi = roi & cv::Rect(0, 0, frame.cols, frame.rows);
    if (safe_roi.width <= 0 || safe_roi.height <= 0) return;

    int eff_factor = (std::max)(2, factor);
    int new_w = (std::max)(1, safe_roi.width / eff_factor);
    int new_h = (std::max)(1, safe_roi.height / eff_factor);

    cv::Mat roi_mat = frame(safe_roi);
    cv::Mat small_mat;
    cv::resize(roi_mat, small_mat, cv::Size(new_w, new_h), 0, 0, cv::INTER_NEAREST);
    cv::resize(small_mat, roi_mat, cv::Size(safe_roi.width, safe_roi.height), 0, 0, cv::INTER_NEAREST);
}

void CpuPostprocessor::ApplyBlur(cv::Mat& frame, const cv::Rect& roi, int radius) {
    if (frame.empty() || roi.width <= 0 || roi.height <= 0) return;
    cv::Rect safe_roi = roi & cv::Rect(0, 0, frame.cols, frame.rows);
    if (safe_roi.width <= 0 || safe_roi.height <= 0) return;

    int ksize = (std::max)(1, radius * 2 + 1);
    ksize = (std::min)(ksize, (std::min)(safe_roi.width, safe_roi.height));
    if (ksize % 2 == 0) ksize--;
    if (ksize < 1) return;

    cv::Mat roi_mat = frame(safe_roi);
    cv::GaussianBlur(roi_mat, roi_mat, cv::Size(ksize, ksize), 0);
}

void CpuPostprocessor::DrawFilledRect(cv::Mat& frame, const cv::Rect& rect, const ColorInfo& color) {
    if (frame.empty() || rect.width <= 0 || rect.height <= 0) return;
    cv::Rect safe_rect = rect & cv::Rect(0, 0, frame.cols, frame.rows);
    if (safe_rect.width <= 0 || safe_rect.height <= 0) return;

    // BGRA 色
    cv::Scalar bgra_color(color.b, color.g, color.r, 255);
    cv::rectangle(frame, safe_rect, bgra_color, cv::FILLED);
}

void CpuPostprocessor::ApplyMask(
    cv::Mat& frame,
    const std::vector<Detection>& detections,
    MaskType mask_type,
    int param,
    const ColorInfo& color
) {
    if (frame.empty() || detections.empty() || mask_type == MaskType::No_Inference) {
        return;
    }

    if (mask_type == MaskType::Inpaint) {
        cv::Mat mask = cv::Mat::zeros(frame.rows, frame.cols, CV_8UC1);
        for (const auto& d : detections) {
            int x = static_cast<int>(std::round(d.x1));
            int y = static_cast<int>(std::round(d.y1));
            int w = static_cast<int>(std::round(d.x2 - d.x1));
            int h = static_cast<int>(std::round(d.y2 - d.y1));
            cv::Rect r = cv::Rect(x, y, w, h) & cv::Rect(0, 0, frame.cols, frame.rows);
            if (r.width > 0 && r.height > 0) {
                cv::rectangle(mask, r, cv::Scalar(255), cv::FILLED);
            }
        }

        cv::Mat bgr;
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
        cv::Mat inpainted;
        int inpaint_radius = (std::max)(1, (std::min)(param > 0 ? param : 3, 10));
        cv::inpaint(bgr, mask, inpainted, inpaint_radius, cv::INPAINT_TELEA);
        cv::cvtColor(inpainted, frame, cv::COLOR_BGR2BGRA);
        return;
    }

    for (const auto& d : detections) {
        int x = static_cast<int>(std::round(d.x1));
        int y = static_cast<int>(std::round(d.y1));
        int w = static_cast<int>(std::round(d.x2 - d.x1));
        int h = static_cast<int>(std::round(d.y2 - d.y1));
        cv::Rect r(x, y, w, h);

        if (mask_type == MaskType::Mosaic) {
            int factor = (param > 0) ? param : 10;
            ApplyMosaic(frame, r, factor);
        } else if (mask_type == MaskType::Blur) {
            int radius = (param > 0) ? param : 15;
            ApplyBlur(frame, r, radius);
        } else if (mask_type == MaskType::RectFill) {
            DrawFilledRect(frame, r, color);
        }
    }
}

void CpuPostprocessor::ApplyFixedRects(
    cv::Mat& frame,
    const RectInfo* rects,
    int count,
    MaskType mask_type,
    int param,
    const ColorInfo& color
) {
    if (frame.empty() || !rects || count <= 0 || mask_type == MaskType::No_Inference) {
        return;
    }

    if (mask_type == MaskType::Inpaint) {
        cv::Mat mask = cv::Mat::zeros(frame.rows, frame.cols, CV_8UC1);
        for (int i = 0; i < count; ++i) {
            cv::Rect r = cv::Rect(rects[i].x, rects[i].y, rects[i].width, rects[i].height) & cv::Rect(0, 0, frame.cols, frame.rows);
            if (r.width > 0 && r.height > 0) {
                cv::rectangle(mask, r, cv::Scalar(255), cv::FILLED);
            }
        }
        cv::Mat bgr;
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
        cv::Mat inpainted;
        int inpaint_radius = (std::max)(1, (std::min)(param > 0 ? param : 3, 10));
        cv::inpaint(bgr, mask, inpainted, inpaint_radius, cv::INPAINT_TELEA);
        cv::cvtColor(inpainted, frame, cv::COLOR_BGR2BGRA);
        return;
    }

    for (int i = 0; i < count; ++i) {
        cv::Rect r(rects[i].x, rects[i].y, rects[i].width, rects[i].height);
        if (mask_type == MaskType::Mosaic) {
            int factor = (param > 0) ? param : 10;
            ApplyMosaic(frame, r, factor);
        } else if (mask_type == MaskType::Blur) {
            int radius = (param > 0) ? param : 15;
            ApplyBlur(frame, r, radius);
        } else if (mask_type == MaskType::RectFill) {
            DrawFilledRect(frame, r, color);
        }
    }
}

void CpuPostprocessor::ApplyCopyrightOverlay(
    cv::Mat& frame,
    const cv::Mat& watermark_bgra,
    int offset_x,
    int offset_y,
    float scale
) {
    if (frame.empty() || watermark_bgra.empty() || watermark_bgra.channels() != 4) {
        return;
    }

    float eff_scale = (scale > 0.01f) ? scale : 1.0f;
    cv::Mat scaled_wm;
    int wm_w = static_cast<int>(std::round(watermark_bgra.cols * eff_scale));
    int wm_h = static_cast<int>(std::round(watermark_bgra.rows * eff_scale));
    wm_w = (std::max)(1, wm_w);
    wm_h = (std::max)(1, wm_h);

    if (wm_w != watermark_bgra.cols || wm_h != watermark_bgra.rows) {
        cv::resize(watermark_bgra, scaled_wm, cv::Size(wm_w, wm_h), 0, 0, cv::INTER_LINEAR);
    } else {
        scaled_wm = watermark_bgra;
    }

    // 右下基準配置
    int dest_x = frame.cols - wm_w + offset_x;
    int dest_y = frame.rows - wm_h + offset_y;

    int src_x = 0;
    int src_y = 0;
    if (dest_x < 0) { src_x = -dest_x; wm_w += dest_x; dest_x = 0; }
    if (dest_y < 0) { src_y = -dest_y; wm_h += dest_y; dest_y = 0; }
    if (dest_x + wm_w > frame.cols) { wm_w = frame.cols - dest_x; }
    if (dest_y + wm_h > frame.rows) { wm_h = frame.rows - dest_y; }

    if (wm_w <= 0 || wm_h <= 0) return;

    cv::Mat frame_roi = frame(cv::Rect(dest_x, dest_y, wm_w, wm_h));
    cv::Mat wm_roi = scaled_wm(cv::Rect(src_x, src_y, wm_w, wm_h));

    // アルファブレンド (BGRA)
    const uint8_t* wm_ptr = wm_roi.ptr<uint8_t>();
    uint8_t* dst_ptr = frame_roi.ptr<uint8_t>();
    int wm_step = static_cast<int>(wm_roi.step);
    int dst_step = static_cast<int>(frame_roi.step);

    for (int y = 0; y < wm_h; ++y) {
        const uint8_t* wm_row = wm_ptr + y * wm_step;
        uint8_t* dst_row = dst_ptr + y * dst_step;
        for (int x = 0; x < wm_w; ++x) {
            float alpha = static_cast<float>(wm_row[x * 4 + 3]) / 255.0f;
            if (alpha <= 0.0f) continue;
            float inv_alpha = 1.0f - alpha;
            dst_row[x * 4 + 0] = static_cast<uint8_t>(dst_row[x * 4 + 0] * inv_alpha + wm_row[x * 4 + 0] * alpha);
            dst_row[x * 4 + 1] = static_cast<uint8_t>(dst_row[x * 4 + 1] * inv_alpha + wm_row[x * 4 + 1] * alpha);
            dst_row[x * 4 + 2] = static_cast<uint8_t>(dst_row[x * 4 + 2] * inv_alpha + wm_row[x * 4 + 2] * alpha);
        }
    }
}

} // namespace WoLNamesBlackedOut::Core

