#include "pch.h"
#include "CpuPostprocessor.h"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <random>

namespace WoLNamesBlackedOut::Core {

namespace {
cv::Rect ToSafeRect(const Detection& d, int maxWidth, int maxHeight) {
    int x = static_cast<int>(std::round(d.x1));
    int y = static_cast<int>(std::round(d.y1));
    int w = static_cast<int>(std::round(d.x2 - d.x1));
    int h = static_cast<int>(std::round(d.y2 - d.y1));
    return cv::Rect(x, y, w, h) & cv::Rect(0, 0, maxWidth, maxHeight);
}

void AlphaBlendImage(cv::Mat& destBgra, const cv::Mat& srcBgra, const cv::Rect& destRect) {
    if (destRect.width <= 0 || destRect.height <= 0) return;
    cv::Mat roi = destBgra(destRect);

    for (int y = 0; y < destRect.height; ++y) {
        const uint8_t* src = srcBgra.ptr<uint8_t>(y);
        uint8_t* dst = roi.ptr<uint8_t>(y);
        for (int x = 0; x < destRect.width; ++x) {
            float alpha = static_cast<float>(src[x * 4 + 3]) / 255.0f;
            if (alpha <= 0.0f) continue;
            float invAlpha = 1.0f - alpha;
            dst[x * 4 + 0] = static_cast<uint8_t>(dst[x * 4 + 0] * invAlpha + src[x * 4 + 0] * alpha);
            dst[x * 4 + 1] = static_cast<uint8_t>(dst[x * 4 + 1] * invAlpha + src[x * 4 + 1] * alpha);
            dst[x * 4 + 2] = static_cast<uint8_t>(dst[x * 4 + 2] * invAlpha + src[x * 4 + 2] * alpha);
        }
    }
}

void ApplyImageFit(cv::Mat& frame, const cv::Rect& roi, const cv::Mat& imageBgra) {
    cv::Mat resized;
    cv::resize(imageBgra, resized, roi.size(), 0, 0, cv::INTER_LINEAR);
    AlphaBlendImage(frame, resized, roi);
}

void ApplyImageTile1(cv::Mat& frame, const cv::Rect& roi, const cv::Mat& imageBgra) {
    if (roi.width <= 0 || roi.height <= 0) return;
    int tileH = roi.height;
    int tileW = static_cast<int>(std::round(static_cast<double>(imageBgra.cols) * tileH / std::max(1, imageBgra.rows)));
    tileW = (std::max)(1, tileW);

    cv::Mat tile;
    cv::resize(imageBgra, tile, cv::Size(tileW, tileH), 0, 0, cv::INTER_LINEAR);

    int x = roi.x;
    int right = roi.x + roi.width;
    while (x < right) {
        int drawW = (std::min)(tileW, right - x);
        cv::Rect srcRect(0, 0, drawW, tileH);
        cv::Rect dstRect(x, roi.y, drawW, tileH);
        cv::Mat croppedTile = tile(srcRect);
        AlphaBlendImage(frame, croppedTile, dstRect);
        x += drawW;
    }
}

void ApplyImageTile2(cv::Mat& frame, const cv::Rect& roi, const cv::Mat& imageBgra) {
    if (roi.width <= 0 || roi.height <= 0 || imageBgra.cols <= 0 || imageBgra.rows <= 0) {
        return;
    }

    cv::Mat scaledTile;
    const cv::Mat* tileSource = &imageBgra;
    if (imageBgra.rows > roi.height) {
        int scaledH = roi.height;
        int scaledW = static_cast<int>(std::round(static_cast<double>(imageBgra.cols) * scaledH / std::max(1, imageBgra.rows)));
        scaledW = (std::max)(1, scaledW);
        cv::resize(imageBgra, scaledTile, cv::Size(scaledW, scaledH), 0, 0, cv::INTER_LINEAR);
        tileSource = &scaledTile;
    }

    const int tileW = tileSource->cols;
    const int tileH = tileSource->rows;
    int y = roi.y;
    const int bottom = roi.y + roi.height;

    while (y < bottom) {
        int drawH = (std::min)(tileH, bottom - y);
        int x = roi.x;
        const int right = roi.x + roi.width;
        while (x < right) {
            int drawW = (std::min)(tileW, right - x);
            cv::Rect srcRect(0, 0, drawW, drawH);
            cv::Rect dstRect(x, y, drawW, drawH);
            cv::Mat croppedTile = (*tileSource)(srcRect);
            AlphaBlendImage(frame, croppedTile, dstRect);
            x += drawW;
        }

        y += drawH;
    }
}

uint64_t MixSeed(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

void ApplyImageRandom(cv::Mat& frame, const cv::Rect& roi, const cv::Mat& imageBgra, float minScale, float maxScale, bool allowOverflow, int64_t randomLayoutSeed) {
    if (roi.width <= 0 || roi.height <= 0) return;
    cv::Mat covered = cv::Mat::zeros(roi.height, roi.width, CV_8UC1);

    uint64_t seed = static_cast<uint64_t>(randomLayoutSeed);
    seed ^= static_cast<uint64_t>(static_cast<uint32_t>(roi.width)) << 1;
    seed ^= static_cast<uint64_t>(static_cast<uint32_t>(roi.height)) << 17;
    seed ^= static_cast<uint64_t>(static_cast<uint32_t>(roi.x / 8)) << 33;
    seed ^= static_cast<uint64_t>(static_cast<uint32_t>(roi.y / 8)) << 49;
    if (allowOverflow) {
        seed ^= 0x9e3779b97f4a7c15ULL;
    }
    std::mt19937 rng(static_cast<uint32_t>(MixSeed(seed) & 0xffffffffULL));
    std::uniform_real_distribution<float> scaleDist(minScale, maxScale);
    std::uniform_real_distribution<float> angleDist(0.0f, 360.0f);
    const cv::Rect frameRect(0, 0, frame.cols, frame.rows);

    const int maxPlacements = 256;
    int placements = 0;
    while (placements < maxPlacements) {
        double coveredRatio = static_cast<double>(cv::countNonZero(covered)) / static_cast<double>(covered.total());
        if (coveredRatio >= 0.98) {
            break;
        }

        float scale = scaleDist(rng);
        int targetH = (std::max)(1, static_cast<int>(std::round(roi.height * scale)));
        int targetW = (std::max)(1, static_cast<int>(std::round(static_cast<double>(imageBgra.cols) * targetH / std::max(1, imageBgra.rows))));

        cv::Mat resized;
        cv::resize(imageBgra, resized, cv::Size(targetW, targetH), 0, 0, cv::INTER_LINEAR);

        cv::Point2f center(static_cast<float>(targetW) * 0.5f, static_cast<float>(targetH) * 0.5f);
        float angle = angleDist(rng);
        cv::Mat rot = cv::getRotationMatrix2D(center, angle, 1.0);
        cv::Rect bbox = cv::RotatedRect(center, resized.size(), angle).boundingRect();
        rot.at<double>(0, 2) += bbox.width * 0.5 - center.x;
        rot.at<double>(1, 2) += bbox.height * 0.5 - center.y;

        cv::Mat rotated;
        cv::warpAffine(resized, rotated, rot, bbox.size(), cv::INTER_LINEAR, cv::BORDER_TRANSPARENT);
        if (rotated.empty()) {
            break;
        }

        const int minRelX = allowOverflow ? (-rotated.cols + 1) : 0;
        const int maxRelX = allowOverflow ? (roi.width - 1) : (std::max)(0, roi.width - rotated.cols);
        const int minRelY = allowOverflow ? (-rotated.rows + 1) : 0;
        const int maxRelY = allowOverflow ? (roi.height - 1) : (std::max)(0, roi.height - rotated.rows);
        std::uniform_int_distribution<int> xDist(minRelX, maxRelX);
        std::uniform_int_distribution<int> yDist(minRelY, maxRelY);

        int relX = xDist(rng);
        int relY = yDist(rng);
        cv::Rect dstRect(roi.x + relX, roi.y + relY, rotated.cols, rotated.rows);
        cv::Rect clipTarget = allowOverflow ? frameRect : roi;
        cv::Rect clipped = dstRect & clipTarget;
        if (clipped.width <= 0 || clipped.height <= 0) {
            placements++;
            continue;
        }

        cv::Rect srcRect(clipped.x - dstRect.x, clipped.y - dstRect.y, clipped.width, clipped.height);
        cv::Mat cropped = rotated(srcRect);
        AlphaBlendImage(frame, cropped, clipped);

        cv::Rect coveredGlobal = clipped & roi;
        if (coveredGlobal.width > 0 && coveredGlobal.height > 0) {
            cv::Rect coveredRect(coveredGlobal.x - roi.x, coveredGlobal.y - roi.y, coveredGlobal.width, coveredGlobal.height);
            cv::rectangle(covered, coveredRect, cv::Scalar(255), cv::FILLED);
        }
        placements++;
    }
}
} // namespace

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

void CpuPostprocessor::ApplyImageMask(
    cv::Mat& frame,
    const std::vector<Detection>& detections,
    const cv::Mat& image_bgra,
    int image_mode,
    float random_min_scale,
    float random_max_scale,
    bool random_allow_overflow,
    int64_t random_layout_seed
) {
    if (frame.empty() || detections.empty() || image_bgra.empty() || image_bgra.channels() != 4) {
        return;
    }

    float minScale = (std::clamp)(random_min_scale, 0.3f, 1.0f);
    float maxScale = (std::clamp)(random_max_scale, 1.0f, 5.0f);
    if (maxScale < minScale) {
        std::swap(minScale, maxScale);
    }

    for (const auto& d : detections) {
        cv::Rect roi = ToSafeRect(d, frame.cols, frame.rows);
        if (roi.width <= 0 || roi.height <= 0) {
            continue;
        }

        switch (image_mode) {
            case 1:
                ApplyImageTile1(frame, roi, image_bgra);
                break;
            case 2:
                ApplyImageTile2(frame, roi, image_bgra);
                break;
            case 3:
                ApplyImageRandom(frame, roi, image_bgra, minScale, maxScale, random_allow_overflow, random_layout_seed);
                break;
            case 0:
            default:
                ApplyImageFit(frame, roi, image_bgra);
                break;
        }
    }
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

