#include "pch.h"
#include "PreviewPipeline.h"
#include "TextMatchUtils.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>

// WIC for image loading
#include <wincodec.h>

using namespace WoLNamesBlackedOut::Core;

// ============================================================
// ファイルログユーティリティ（OutputDebugStringA の代替）
// ============================================================
static void LogToFile(const char* msg) {
    OutputDebugStringA(msg);
}

static void LogFmt(const char* fmt, ...) {
    char buf[1024] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LogToFile(buf);
}

using PreviewClock = std::chrono::steady_clock;

static double ElapsedMilliseconds(PreviewClock::time_point start) {
    return std::chrono::duration<double, std::milli>(PreviewClock::now() - start).count();
}

// ============================================================
// コンストラクタ / デストラクタ
// ============================================================

PreviewPipeline::PreviewPipeline()
    : device_manager_(nullptr), device_(nullptr), context_(nullptr),
      format_ctx_(nullptr), decoder_ctx_(nullptr), video_stream_index_(-1),
      hw_device_ctx_(nullptr), is_video_(false), total_frames_(0), fps_(30),
      video_width_(0), video_height_(0), conf_threshold_(0.15f), iou_threshold_(0.45f), inference_done_(false),
            image_width_(0), image_height_(0),
            copyright_width_(0), copyright_height_(0) {
    memset(&mask_params_, 0, sizeof(mask_params_));
}

PreviewPipeline::~PreviewPipeline() {
    Cleanup();
}

// ============================================================
// 初期化
// ============================================================

bool PreviewPipeline::Initialize(
    const wchar_t* file_path,
    const wchar_t* model_path,
    float conf_threshold,
    float iou_threshold,
    D3D11DeviceManager* device_manager) {
    if (!file_path || !device_manager) {
        LogToFile("[PreviewPipeline] Initialize: null file_path or device_manager\n");
        return false;
    }

    Cleanup();

    device_manager_ = device_manager;
    device_ = device_manager->GetDevice();
    context_ = device_manager->GetContext();
    file_path_ = file_path;
    conf_threshold_ = std::clamp(conf_threshold, 0.01f, 0.50f);
    iou_threshold_ = std::clamp(iou_threshold, 0.01f, 0.95f);

    if (!device_) {
        LogToFile("[PreviewPipeline] Initialize: D3D11 device is null\n");
        return false;
    }
    if (!context_) {
        LogToFile("[PreviewPipeline] Initialize: D3D11 context is null\n");
        return false;
    }

    // ファイル拡張子で動画/静止画を判別
    std::wstring path(file_path);
    std::transform(path.begin(), path.end(), path.begin(), ::tolower);
    is_video_ = (path.find(L".mp4") != std::wstring::npos ||
                 path.find(L".mov") != std::wstring::npos ||
                 path.find(L".avi") != std::wstring::npos ||
                 path.find(L".mkv") != std::wstring::npos ||
                 path.find(L".wmv") != std::wstring::npos ||
                 path.find(L".flv") != std::wstring::npos ||
                 path.find(L".webm") != std::wstring::npos);

    {
        char path_narrow[400] = {};
        WideCharToMultiByte(CP_ACP, 0, file_path, -1, path_narrow, sizeof(path_narrow)-1, nullptr, nullptr);
        LogFmt("[PreviewPipeline] Initialize: file='%s' is_video=%d\n", path_narrow, (int)is_video_);
    }

    // WinML初期化
    winml_utils_.SetD3D11Device(device_, context_);
    winml_utils_.SetGpuVendor(device_manager_->GetGpuVendor());

    LogToFile("[PreviewPipeline] Calling WinML Initialize...\n");
    if (!winml_utils_.Initialize()) {
        LogToFile("[PreviewPipeline] FAIL: WinML Initialize\n");
        return false;
    }
    LogToFile("[PreviewPipeline] WinML Initialize OK\n");

    // ONNXモデルロード
    if (model_path) {
        char model_narrow[400] = {};
        WideCharToMultiByte(CP_ACP, 0, model_path, -1, model_narrow, sizeof(model_narrow)-1, nullptr, nullptr);
        LogFmt("[PreviewPipeline] LoadModel: '%s'\n", model_narrow);
        if (!winml_utils_.LoadModel(model_path)) {
            LogToFile("[PreviewPipeline] WARN: LoadModel failed (continuing without inference)\n");
        } else {
            LogToFile("[PreviewPipeline] LoadModel OK\n");
        }
    } else {
        LogToFile("[PreviewPipeline] model_path is null, skipping LoadModel\n");
    }

    // マスクシェーダー初期化
    LogToFile("[PreviewPipeline] Calling MaskShader Initialize...\n");
    if (!mask_shader_.Initialize(device_)) {
        LogToFile("[PreviewPipeline] FAIL: MaskShader Initialize\n");
        return false;
    }
    LogToFile("[PreviewPipeline] MaskShader Initialize OK\n");

    tracker_ = std::make_unique<ByteTrackInterop::Tracker>(30, 30, conf_threshold_);
    ocr_text_by_track_id_.clear();
    ocr_last_frame_by_track_.clear();
    preview_frame_counter_ = 0;
    LogFmt("[ExcludeByName][Preview][Init] conf=%.3f\n", conf_threshold_);

    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring basePath(modulePath);
    size_t basePos = basePath.find_last_of(L"\\/");
    if (basePos != std::wstring::npos) {
        basePath = basePath.substr(0, basePos + 1);
    }

    std::wstring ocrModelPath = basePath + L"c_ppocr-v5-rec_sim.onnx";
    std::wstring ocrDictPath = basePath + L"ppocrv5_en_dict.txt";

    ocr_recognizer_.SetD3D11Device(device_, context_);
    ocr_recognizer_.SetGpuVendor(device_manager_->GetGpuVendor());
    const bool dict_loaded = ocr_recognizer_.LoadDictionary(ocrDictPath.c_str());
    const bool model_loaded = ocr_recognizer_.LoadModel(ocrModelPath.c_str(), true);
    LogFmt("[ExcludeByName][Preview][Init] dict_loaded=%d model_loaded=%d\n", dict_loaded ? 1 : 0, model_loaded ? 1 : 0);
    if (!dict_loaded || !model_loaded) {
        LogToFile("[PreviewPipeline] WARN: OCR not ready, exclude-by-name will fallback to regular masking\n");
    }

    // 動画/静止画別初期化
    LogToFile(is_video_ ? "[PreviewPipeline] Calling InitializeVideo...\n"
                        : "[PreviewPipeline] Calling InitializeImage...\n");
    bool result = is_video_ ? InitializeVideo(file_path) : InitializeImage(file_path);
    if (!result) {
        LogToFile(is_video_ ? "[PreviewPipeline] FAIL: InitializeVideo\n"
                            : "[PreviewPipeline] FAIL: InitializeImage\n");
        return false;
    }
    LogToFile("[PreviewPipeline] Initialize complete OK\n");

    return true;
}

bool PreviewPipeline::InitializeVideo(const wchar_t* file_path) {
    avformat_network_init();

    format_ctx_ = avformat_alloc_context();
    if (!format_ctx_) {
        LogToFile("[InitializeVideo] avformat_alloc_context failed\n");
        return false;
    }

    // UTF-8に変換
    int len = WideCharToMultiByte(CP_UTF8, 0, file_path, -1, nullptr, 0, nullptr, nullptr);
    std::string path_utf8;
    if (len > 0) {
        path_utf8.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, file_path, -1, &path_utf8[0], len, nullptr, nullptr);
    }

    LogFmt("[InitializeVideo] Opening: '%s'\n", path_utf8.c_str());

    int ret = avformat_open_input(&format_ctx_, path_utf8.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errbuf[256]; av_strerror(ret, errbuf, sizeof(errbuf));
        LogFmt("[InitializeVideo] avformat_open_input FAIL: %s\n", errbuf);
        return false;
    }

    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
        LogToFile("[InitializeVideo] avformat_find_stream_info failed\n");
        return false;
    }

    // ビデオストリームを検索
    video_stream_index_ = -1;
    for (unsigned int i = 0; i < format_ctx_->nb_streams; i++) {
        if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = static_cast<int>(i);
            break;
        }
    }

    if (video_stream_index_ == -1) {
        LogToFile("[InitializeVideo] No video stream found\n");
        return false;
    }

    AVCodecParameters* codec_params = format_ctx_->streams[video_stream_index_]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(codec_params->codec_id);
    if (!decoder) {
        LogFmt("[InitializeVideo] No decoder for codec_id=%d\n", (int)codec_params->codec_id);
        return false;
    }

    LogFmt("[InitializeVideo] Decoder: %s\n", decoder->name);

    decoder_ctx_ = avcodec_alloc_context3(decoder);
    if (!decoder_ctx_) {
        LogToFile("[InitializeVideo] avcodec_alloc_context3 failed\n");
        return false;
    }

    // HWデコード用コンテキスト
    hw_device_ctx_ = device_manager_->GetHWDeviceContext();
    if (hw_device_ctx_) {
        decoder_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
        LogToFile("[InitializeVideo] HW device context attached\n");
    } else {
        LogToFile("[InitializeVideo] No HW device context, using SW decode\n");
    }

    if (avcodec_parameters_to_context(decoder_ctx_, codec_params) < 0) {
        LogToFile("[InitializeVideo] avcodec_parameters_to_context failed\n");
        return false;
    }

    ret = avcodec_open2(decoder_ctx_, decoder, nullptr);
    if (ret < 0) {
        char errbuf[256]; av_strerror(ret, errbuf, sizeof(errbuf));
        LogFmt("[InitializeVideo] avcodec_open2 FAIL: %s\n", errbuf);
        return false;
    }

    // 動画情報を取得
    video_width_ = codec_params->width;
    video_height_ = codec_params->height;
    fps_ = static_cast<int>(av_q2d(format_ctx_->streams[video_stream_index_]->avg_frame_rate));
    if (fps_ <= 0) {
        fps_ = static_cast<int>(av_q2d(format_ctx_->streams[video_stream_index_]->r_frame_rate));
    }
    if (fps_ <= 0) fps_ = 30;

    total_frames_ = static_cast<int>(format_ctx_->streams[video_stream_index_]->nb_frames);
    if (total_frames_ <= 0) {
        int64_t duration = format_ctx_->streams[video_stream_index_]->duration;
        AVRational time_base = format_ctx_->streams[video_stream_index_]->time_base;
        if (duration > 0 && time_base.den > 0) {
            total_frames_ = static_cast<int>(duration * time_base.num / time_base.den * fps_);
        }
    }

    LogFmt("[InitializeVideo] OK: %dx%d fps=%d frames=%d\n",
        video_width_, video_height_, fps_, total_frames_);

    return true;
}

bool PreviewPipeline::InitializeImage(const wchar_t* file_path) {
    // 画像ファイルをD3D11テクスチャに読み込み
    const auto decodeStart = PreviewClock::now();
    if (!LoadImageToTexture(file_path, image_texture_, image_width_, image_height_)) {
        std::cerr << "Failed to load image file." << std::endl;
        return false;
    }
    const double decodeMs = ElapsedMilliseconds(decodeStart);

    video_width_ = static_cast<int>(image_width_);
    video_height_ = static_cast<int>(image_height_);
    total_frames_ = 1;
    fps_ = 1;

    // 静止画の場合、Open時に推論を実行してキャッシュ
    const auto inferenceStart = PreviewClock::now();
    if (winml_utils_.IsModelLoaded()) {
        RunInference(image_texture_.Get(), image_width_, image_height_);
    }
    const double inferenceMs = ElapsedMilliseconds(inferenceStart);

    LogFmt("[PreviewTiming] image_decode_upload_ms=%.3f inference_total_ms=%.3f size=%ux%u\n",
        decodeMs, inferenceMs, image_width_, image_height_);

    std::cout << "Image preview initialized: " << image_width_ << "x" << image_height_ << std::endl;

    return true;
}

// ============================================================
// リソース解放
// ============================================================

void PreviewPipeline::Cleanup() {
    try {
        inference_done_ = false;
        cached_detections_.clear();
        tracker_.reset();
        ocr_text_by_track_id_.clear();
        ocr_last_frame_by_track_.clear();
        preview_frame_counter_ = 0;

        if (decoder_ctx_) {
            av_buffer_unref(&decoder_ctx_->hw_device_ctx);
            avcodec_free_context(&decoder_ctx_);
        }
        if (format_ctx_) {
            avformat_close_input(&format_ctx_);
        }

        image_texture_.Reset();
        copyright_texture_.Reset();
        copyright_srv_.Reset();
        copyright_width_ = 0;
        copyright_height_ = 0;
        rgba_texture_.Reset();
        output_texture_.Reset();

        mask_shader_.Release();
        winml_utils_.Uninitialize();

        device_manager_ = nullptr;
        device_ = nullptr;
        context_ = nullptr;
        format_ctx_ = nullptr;
        decoder_ctx_ = nullptr;
        video_stream_index_ = -1;
        hw_device_ctx_ = nullptr;
        is_video_ = false;
        total_frames_ = 0;
        fps_ = 30;
        video_width_ = 0;
        video_height_ = 0;
        image_width_ = 0;
        image_height_ = 0;
    }
    catch (const winrt::hresult_error& ex) {
        std::string msg = winrt::to_string(ex.message());
        LogFmt("[PreviewPipeline] Cleanup WinRT exception: %s\n", msg.c_str());
    }
    catch (const std::exception& ex) {
        LogFmt("[PreviewPipeline] Cleanup exception: %s\n", ex.what());
    }
}

// ============================================================
// フレーム取得
// ============================================================

bool PreviewPipeline::GetFrame(
    int frame_index,
    unsigned char* out_rgba_buffer,
    int buffer_size,
    int* out_width,
    int* out_height) {

    if (!out_rgba_buffer || !out_width || !out_height) return false;

    bool result = false;
    const auto frameStart = PreviewClock::now();
    const auto maskStart = PreviewClock::now();

    if (is_video_) {
        result = GetVideoFrame(frame_index);
    } else {
        result = GetImageFrame();
    }

    if (!result || !output_texture_) return false;

    const double maskMs = ElapsedMilliseconds(maskStart);
    const auto readbackStart = PreviewClock::now();
    const bool copyResult = CopyTextureToRgbaBuffer(output_texture_.Get(), out_rgba_buffer, buffer_size, out_width, out_height);
    const double readbackMs = ElapsedMilliseconds(readbackStart);
    LogFmt("[PreviewTiming] mask_ms=%.3f final_gpu_readback_ms=%.3f get_frame_total_ms=%.3f\n",
        maskMs, readbackMs, ElapsedMilliseconds(frameStart));
    return copyResult;
}

bool PreviewPipeline::GetVideoFrame(int frame_index) {
    if (!format_ctx_ || !decoder_ctx_ || video_stream_index_ < 0) return false;

    // 指定フレーム位置にシーク
    AVStream* stream = format_ctx_->streams[video_stream_index_];
    int64_t timestamp = av_rescale_q(frame_index, {1, fps_}, stream->time_base);

    if (av_seek_frame(format_ctx_, video_stream_index_, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
        std::cerr << "Failed to seek to frame " << frame_index << std::endl;
        return false;
    }

    avcodec_flush_buffers(decoder_ctx_);

    // フレームをデコード
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    bool frame_decoded = false;

    while (av_read_frame(format_ctx_, packet) >= 0) {
        if (packet->stream_index == video_stream_index_) {
            int ret = avcodec_send_packet(decoder_ctx_, packet);
            if (ret >= 0) {
                ret = avcodec_receive_frame(decoder_ctx_, frame);
                if (ret == 0) {
                    frame_decoded = true;
                    break;
                }
            }
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    if (!frame_decoded || !frame->data[0]) {
        av_frame_free(&frame);
        return false;
    }

    // D3D11テクスチャを取得 → 必ず BGRA に正規化する
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source_texture;
    {
        // まず SW フレームとして取得（HW/SW 両対応）
        AVFrame* sw_frame = av_frame_alloc();
        bool uploaded = false;
        if (sw_frame) {
            AVFrame* target = frame;
            // HW フレームなら SW へ転送
            if (frame->format == AV_PIX_FMT_D3D11 ||
                (frame->format != AV_PIX_FMT_YUV420P &&
                 frame->format != AV_PIX_FMT_NV12 &&
                 frame->format != AV_PIX_FMT_BGR24 &&
                 frame->format != AV_PIX_FMT_RGB24 &&
                 frame->format != AV_PIX_FMT_BGRA &&
                 frame->format != AV_PIX_FMT_RGBA)) {
                if (av_hwframe_transfer_data(sw_frame, frame, 0) >= 0) {
                    target = sw_frame;
                }
            }
            // → BGRA 変換してテクスチャ作成
            struct SwsContext* sws = sws_getContext(
                video_width_, video_height_, (AVPixelFormat)target->format,
                video_width_, video_height_, AV_PIX_FMT_BGRA,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (sws) {
                std::vector<uint8_t> pixels(video_width_ * video_height_ * 4);
                uint8_t* dst_data[1] = { pixels.data() };
                int dst_linesize[1] = { (int)video_width_ * 4 };
                sws_scale(sws, target->data, target->linesize, 0, video_height_, dst_data, dst_linesize);
                sws_freeContext(sws);

                D3D11_TEXTURE2D_DESC desc = {};
                desc.Width  = video_width_;
                desc.Height = video_height_;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                D3D11_SUBRESOURCE_DATA init_data = {};
                init_data.pSysMem = pixels.data();
                init_data.SysMemPitch = video_width_ * 4;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(device_->CreateTexture2D(&desc, &init_data, tex.ReleaseAndGetAddressOf()))) {
                    source_texture = tex;
                    uploaded = true;
                }
            }
            av_frame_free(&sw_frame);
        }
        if (!uploaded) {
            av_frame_free(&frame);
            return false;
        }
    }

    // 動画プレビューではフレームごとに推論を更新する
    // （静止画は InitializeImage で1回推論してキャッシュを再利用）
    if (winml_utils_.IsModelLoaded()) {
        {
            std::lock_guard<std::mutex> lock(inference_mutex_);
            inference_done_ = false;
            cached_detections_.clear();
        }
        RunInference(source_texture.Get(), video_width_, video_height_);
    }

    // マスク適用
    bool mask_result = ApplyMask(source_texture.Get(), video_width_, video_height_);

    av_frame_free(&frame);

    return mask_result;
}

bool PreviewPipeline::GetImageFrame() {
    if (!image_texture_) return false;

    // 静止画は推論済み（InitializeImageで実行）
    // マスク適用のみ実行
    return ApplyMask(image_texture_.Get(), image_width_, image_height_);
}

// ============================================================
// 推論実行（1回のみ、結果をキャッシュ）
// ============================================================

bool PreviewPipeline::RunInference(ID3D11Texture2D* source_texture, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(inference_mutex_);

    if (inference_done_) return true;
    if (!winml_utils_.IsModelLoaded()) {
        LogToFile("[RunInference] No model loaded, skipping inference\n");
        inference_done_ = true;
        return true;
    }

    try {
        const auto inferenceStart = PreviewClock::now();
        // --- モデルの実際の入力サイズを取得 ---
        const std::string& input_name = winml_utils_.GetInputNames().empty() ? "images" : winml_utils_.GetInputNames()[0];
        auto model_shape = winml_utils_.GetInputShape(input_name);
        // shape: [1, 3, H, W]
        uint32_t MODEL_H = (model_shape.size() >= 4 && model_shape[2] > 0) ? (uint32_t)model_shape[2] : 736;
        uint32_t MODEL_W = (model_shape.size() >= 4 && model_shape[3] > 0) ? (uint32_t)model_shape[3] : 1280;
        LogFmt("[RunInference] Model input: %ux%u\n", MODEL_W, MODEL_H);

        // staging texture を作成して CPU 読み出し
        D3D11_TEXTURE2D_DESC src_desc;
        source_texture->GetDesc(&src_desc);

        D3D11_TEXTURE2D_DESC stage_desc = src_desc;
        stage_desc.Usage = D3D11_USAGE_STAGING;
        stage_desc.BindFlags = 0;
        stage_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stage_desc.MiscFlags = 0;
        stage_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> stage_tex;
        HRESULT hr = device_->CreateTexture2D(&stage_desc, nullptr, stage_tex.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            stage_desc.Format = src_desc.Format;
            hr = device_->CreateTexture2D(&stage_desc, nullptr, stage_tex.ReleaseAndGetAddressOf());
        }
        if (FAILED(hr)) {
            LogFmt("[RunInference] CreateTexture2D(staging) failed HR=0x%08X\n", (unsigned)hr);
            inference_done_ = true;
            return true;
        }

        const auto readbackStart = PreviewClock::now();
        context_->CopyResource(stage_tex.Get(), source_texture);

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = context_->Map(stage_tex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            LogFmt("[RunInference] Map failed HR=0x%08X\n", (unsigned)hr);
            inference_done_ = true;
            return true;
        }

        const uint8_t* src_data = static_cast<const uint8_t*>(mapped.pData);
        uint32_t src_w = src_desc.Width;
        uint32_t src_h = src_desc.Height;

        // --- YOLO26 前処理: 左上揃えリサイズ（letterbox なし）---
        // resizeScales = original_dim / target_dim（参照コードと同じ）
        float scale_x = float(src_w) / float(MODEL_W);
        float scale_y = float(src_h) / float(MODEL_H);
        float resizeScales = std::max(scale_x, scale_y); // 参照コード同様、長辺基準

        // モデル入力サイズにリサイズ（黒パディングで左上揃え）
        int scaled_w = (scale_x >= scale_y)
            ? (int)MODEL_W
            : (int)(src_w / resizeScales);
        int scaled_h = (scale_y >= scale_x)
            ? (int)MODEL_H
            : (int)(src_h / resizeScales);

        struct SwsContext* sws = sws_getContext(
            src_w, src_h, AV_PIX_FMT_BGRA,
            scaled_w, scaled_h, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        std::vector<float> input_tensor(1 * 3 * MODEL_H * MODEL_W, 0.0f); // 黒パディング

        if (sws) {
            std::vector<uint8_t> rgb_scaled(scaled_w * scaled_h * 3);
            const uint8_t* src_planes[1] = { src_data };
            int src_strides[1] = { static_cast<int>(mapped.RowPitch) };
            uint8_t* dst_planes[1] = { rgb_scaled.data() };
            int dst_strides[1] = { scaled_w * 3 };
            sws_scale(sws, src_planes, src_strides, 0, src_h, dst_planes, dst_strides);
            sws_freeContext(sws);

            // NCHW RGB 形式（左上揃え、残りは 0.0）
            for (int y = 0; y < scaled_h && y < (int)MODEL_H; ++y) {
                for (int x = 0; x < scaled_w && x < (int)MODEL_W; ++x) {
                    const uint8_t* p = &rgb_scaled[(y * scaled_w + x) * 3];
                    input_tensor[0 * MODEL_H * MODEL_W + y * MODEL_W + x] = p[0] / 255.0f; // R
                    input_tensor[1 * MODEL_H * MODEL_W + y * MODEL_W + x] = p[1] / 255.0f; // G
                    input_tensor[2 * MODEL_H * MODEL_W + y * MODEL_W + x] = p[2] / 255.0f; // B
                }
            }
        }
        context_->Unmap(stage_tex.Get(), 0);
        const double preprocessingReadbackMs = ElapsedMilliseconds(readbackStart);

        // --- 2. ONNX 推論実行 ---
        std::vector<int64_t> shape = {1, 3, (int64_t)MODEL_H, (int64_t)MODEL_W};
        const auto evaluateStart = PreviewClock::now();
        auto raw_output = winml_utils_.Evaluate(input_name, input_tensor.data(), input_tensor.size(), shape);
        const double evaluateMs = ElapsedMilliseconds(evaluateStart);
        LogFmt("[PreviewTiming] inference_preprocess_readback_ms=%.3f inference_evaluate_ms=%.3f inference_pipeline_ms=%.3f\n",
            preprocessingReadbackMs, evaluateMs, ElapsedMilliseconds(inferenceStart));

        // --- 3. YOLO26 出力パース: shape (N, detections, 6) = [x1, y1, x2, y2, score, class_id] ---
        cached_detections_.clear();

        if (!raw_output.empty()) {
            auto out_shape = winml_utils_.GetOutputShape(
                winml_utils_.GetOutputNames().empty() ? "output0" : winml_utils_.GetOutputNames()[0]);

            LogFmt("[RunInference] Output shape: [%lld, %lld, %lld]\n",
                out_shape.size() >= 1 ? out_shape[0] : 0,
                out_shape.size() >= 2 ? out_shape[1] : 0,
                out_shape.size() >= 3 ? out_shape[2] : 0);

            if (out_shape.size() >= 3) {
                int64_t batch    = out_shape[0];
                int64_t dim1     = out_shape[1];
                int64_t dim2     = out_shape[2];
                int64_t detections = 0;
                int64_t per_det    = 0;

                // YOLO26: (N, detections, 6)
                if (dim2 == 6) {
                    detections = dim1;
                    per_det    = dim2;
                }
                // 旧 YOLOv8: (N, 6, num_boxes) -- 念のため対応
                else if (dim1 == 6) {
                    detections = dim2;
                    per_det    = dim1;
                }
                else {
                    // フォールバック
                    detections = dim1;
                    per_det    = dim2;
                }

                const float score_thresh = conf_threshold_;

                for (int64_t i = 0; i < detections; ++i) {
                    const float* det;
                    float x1_m, y1_m, x2_m, y2_m, score;
                    int   class_id;

                    if (dim2 == 6) {
                        // 行優先: det[i] = [x1, y1, x2, y2, score, cls]
                        det      = &raw_output[i * per_det];
                        x1_m     = det[0];
                        y1_m     = det[1];
                        x2_m     = det[2];
                        y2_m     = det[3];
                        score    = det[4];
                        class_id = static_cast<int>(det[5]);
                    } else {
                        // 列優先: per_det 行 × detections 列
                        x1_m     = raw_output[0 * detections + i];
                        y1_m     = raw_output[1 * detections + i];
                        x2_m     = raw_output[2 * detections + i];
                        y2_m     = raw_output[3 * detections + i];
                        score    = raw_output[4 * detections + i];
                        class_id = static_cast<int>(raw_output[5 * detections + i]);
                    }

                    if (score <= score_thresh) continue;
                    if (x1_m >= x2_m || y1_m >= y2_m) continue;

                    // モデル座標 → 元画像座標
                    Detection d;
                    d.class_id = class_id;
                    d.score    = score;
                    d.x1 = std::max(0.f, x1_m * resizeScales);
                    d.y1 = std::max(0.f, y1_m * resizeScales);
                    d.x2 = std::min(float(src_w), x2_m * resizeScales);
                    d.y2 = std::min(float(src_h), y2_m * resizeScales);

                    if (d.x2 <= d.x1 || d.y2 <= d.y1) continue;
                    cached_detections_.push_back(d);
                }

                LogFmt("[RunInference] detections=%zu\n", cached_detections_.size());
                for (size_t di = 0; di < cached_detections_.size(); ++di) {
                    const auto& d = cached_detections_[di];
                    LogFmt("[RunInference] det[%zu] cls=%d score=%.3f x1=%.1f y1=%.1f x2=%.1f y2=%.1f\n",
                        di, d.class_id, d.score, d.x1, d.y1, d.x2, d.y2);
                }
            }
        }

        inference_done_ = true;
        return true;
    }
    catch (const std::exception& ex) {
        LogFmt("[RunInference] exception: %s\n", ex.what());
        inference_done_ = true;
        return true;
    }
}

// ============================================================
// マスク適用（推論結果キャッシュを使用）
// ============================================================

bool PreviewPipeline::ApplyMask(ID3D11Texture2D* source_texture, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(params_mutex_);

    if (!source_texture) return false;

    // マスク種別を取得
    MaskType blacked_type = static_cast<MaskType>(mask_params_.blacked_type);
    MaskType fixmask_type = static_cast<MaskType>(mask_params_.fixmask_type);

    std::vector<Detection> active_detections = cached_detections_;

    if (mask_params_.exclude_by_name_enabled && tracker_) {
        preview_frame_counter_++;
        LogFmt("[ExcludeByName][Preview] frame=%d det_in=%zu\n", preview_frame_counter_, cached_detections_.size());
        std::vector<ByteTrackInterop::DetectionPtr> tracker_inputs;
        tracker_inputs.reserve(cached_detections_.size());
        for (const auto& d : cached_detections_) {
            tracker_inputs.push_back(std::make_shared<ByteTrackInterop::Detection>(d));
        }

        auto tracked = tracker_->update(tracker_inputs);
        LogFmt("[ExcludeByName][Preview] frame=%d tracked=%zu\n", preview_frame_counter_, tracked.size());
        std::vector<OcrTrackRoi> ocr_rois;
        std::vector<std::pair<Detection, uint64_t>> tracked_entries;
        tracked_entries.reserve(tracked.size());

        for (const auto& tr : tracked) {
            if (!tr) {
                continue;
            }

            const uint64_t track_id = static_cast<uint64_t>(tr->track_id());
            Detection d = tr->getDetection();
            tracked_entries.push_back({ d, track_id });

            if (!ocr_recognizer_.IsReady() || track_id == 0) {
                continue;
            }

            const auto last_it = ocr_last_frame_by_track_.find(track_id);
            const bool has_text = ocr_text_by_track_id_.find(track_id) != ocr_text_by_track_id_.end();
            const bool need_refresh = (last_it == ocr_last_frame_by_track_.end()) ||
                ((preview_frame_counter_ - last_it->second) >= std::max(1, fps_));

            if (!has_text || need_refresh) {
                ocr_rois.push_back({ track_id, d });
            }
        }

        if (!ocr_rois.empty() && ocr_recognizer_.IsReady()) {
            LogFmt("[ExcludeByName][Preview] frame=%d ocr_request=%zu\n", preview_frame_counter_, ocr_rois.size());
            auto ocr_results = ocr_recognizer_.Recognize(
                source_texture,
                width,
                height,
                ocr_rois,
                mask_params_.ocr_expand_pixels,
                mask_params_.ocr_max_rois_per_frame);

            for (const auto& ocr : ocr_results) {
                const std::string text = TextMatch::Sanitize(ocr.text);
                if (!text.empty()) {
                    ocr_text_by_track_id_[ocr.track_id] = text;
                }
                ocr_last_frame_by_track_[ocr.track_id] = preview_frame_counter_;
                LogFmt("[ExcludeByName][Preview][OCR] frame=%d track=%llu raw='%s' sanitized='%s' conf=%.3f\n",
                    preview_frame_counter_,
                    static_cast<unsigned long long>(ocr.track_id),
                    ocr.text.c_str(),
                    text.c_str(),
                    ocr.confidence);
            }
        }

        std::wstring exclude_csv_w(mask_params_.mask_exclude_text_csv);
        std::vector<std::string> exclude_texts = TextMatch::SplitCsv(TextMatch::ToUtf8(exclude_csv_w.c_str()));
        LogFmt("[ExcludeByName][Preview] frame=%d csv_count=%zu sim=%.2f\n", preview_frame_counter_, exclude_texts.size(), mask_params_.text_similarity_threshold);

        active_detections.clear();
        active_detections.reserve(tracked_entries.size());
        for (const auto& entry : tracked_entries) {
            if (entry.second != 0 && !exclude_texts.empty()) {
                auto it = ocr_text_by_track_id_.find(entry.second);
                if (it != ocr_text_by_track_id_.end()) {
                    const bool excluded = TextMatch::IsExcludedBySimilarity(it->second, exclude_texts, mask_params_.text_similarity_threshold);
                    LogFmt("[ExcludeByName][Preview][Match] frame=%d track=%llu text='%s' excluded=%d\n",
                        preview_frame_counter_,
                        static_cast<unsigned long long>(entry.second),
                        it->second.c_str(),
                        excluded ? 1 : 0);
                    if (excluded) {
                        continue;
                    }
                }
            }
            active_detections.push_back(entry.first);
        }
        LogFmt("[ExcludeByName][Preview] frame=%d mask_after=%zu\n", preview_frame_counter_, active_detections.size());
    }

    // マスク適用が不要な場合
    if (blacked_type == MaskType::No_Inference && mask_params_.fixed_rect_count == 0) {
        output_texture_ = source_texture;
        output_texture_->AddRef();
        return true;
    }

    // 検出部マスクテクスチャを生成
    auto detection_mask = mask_shader_.CreateMaskTexture(width, height, active_detections);
    // 固定矩形マスクテクスチャを生成
    std::vector<Detection> fixed_detections;
    for (int i = 0; i < mask_params_.fixed_rect_count && i < 64; ++i) {
        Detection d;
        d.class_id = -1;
        d.score = 1.0f;
        d.x1 = static_cast<float>(mask_params_.fixed_rects[i].x);
        d.y1 = static_cast<float>(mask_params_.fixed_rects[i].y);
        d.x2 = static_cast<float>(mask_params_.fixed_rects[i].x + mask_params_.fixed_rects[i].width);
        d.y2 = static_cast<float>(mask_params_.fixed_rects[i].y + mask_params_.fixed_rects[i].height);
        fixed_detections.push_back(d);
    }
    auto fixed_mask = mask_shader_.CreateMaskTexture(width, height, fixed_detections);

    // 両方のマスクが空でも、後段のcopyright合成は実行できるように
    // ここでは早期returnせず、output_texture_に元画像をセットして処理継続する
    bool has_detection = detection_mask != nullptr && !active_detections.empty();
    bool has_fixed = fixed_mask != nullptr && !fixed_detections.empty();

    if (!has_detection && !has_fixed) {
        output_texture_ = source_texture;
        output_texture_->AddRef();
        std::wstring overridePath = copyright_image_path_override_;
        if (mask_params_.enable_copyright && output_texture_) {
            if (EnsureCopyrightWatermarkLoaded(overridePath)) {
                float scale = std::clamp(mask_params_.copyright_scale, 0.5f, 3.0f);
                uint32_t target_w = std::max<uint32_t>(1, static_cast<uint32_t>(std::round(copyright_width_ * scale)));
                uint32_t target_h = std::max<uint32_t>(1, static_cast<uint32_t>(std::round(copyright_height_ * scale)));
                if (target_w > 0 && target_h > 0) {
                    int pos_x = static_cast<int>(width) - static_cast<int>(target_w) + mask_params_.copyright_offset_x;
                    int pos_y = static_cast<int>(height) - static_cast<int>(target_h) + mask_params_.copyright_offset_y;

                    const int max_x = static_cast<int>(width) - static_cast<int>(target_w);
                    const int max_y = static_cast<int>(height) - static_cast<int>(target_h);
                    pos_x = std::max(0, std::min(pos_x, std::max(0, max_x)));
                    pos_y = std::max(0, std::min(pos_y, std::max(0, max_y)));

                    Microsoft::WRL::ComPtr<ID3D11Texture2D> watermarked;
                    if (mask_shader_.ApplyCopyrightOverlay(
                        output_texture_.Get(),
                        copyright_srv_.Get(),
                        target_w,
                        target_h,
                        pos_x,
                        pos_y,
                        watermarked) && watermarked.Get()) {
                        output_texture_ = watermarked;
                    }
                }
            }
        }

        return true;
    }

    // 検出部マスク適用
    Microsoft::WRL::ComPtr<ID3D11Texture2D> detection_output;
    bool detection_result = false;
    if (has_detection) {
        switch (blacked_type) {
            case MaskType::Inpaint:
                detection_result = mask_shader_.ApplyInpaint(source_texture, detection_mask.Get(), mask_params_.blackedout_param, detection_output);
                break;
            case MaskType::Mosaic:
                detection_result = mask_shader_.ApplyMosaic(source_texture, detection_mask.Get(), mask_params_.blackedout_param, detection_output);
                break;
            case MaskType::Blur:
                detection_result = mask_shader_.ApplyBlur(source_texture, detection_mask.Get(), mask_params_.blackedout_param, detection_output);
                break;
            case MaskType::RectFill:
            default: {
                float color[4] = {
                    mask_params_.name_color.r / 255.0f,
                    mask_params_.name_color.g / 255.0f,
                    mask_params_.name_color.b / 255.0f,
                    1.0f
                };
                detection_result = mask_shader_.ApplyRectFill(source_texture, detection_mask.Get(), color, detection_output);
                break;
            }
        }
    } else {
        detection_output = source_texture;
        detection_output->AddRef();
        detection_result = true;
    }

    // 固定矩形マスク適用
    Microsoft::WRL::ComPtr<ID3D11Texture2D> fixed_output;
    bool fixed_result = false;
    if (has_fixed) {
        switch (fixmask_type) {
            case MaskType::Inpaint:
                fixed_result = mask_shader_.ApplyInpaint(source_texture, fixed_mask.Get(), mask_params_.fixedFrame_param, fixed_output);
                break;
            case MaskType::Mosaic:
                fixed_result = mask_shader_.ApplyMosaic(source_texture, fixed_mask.Get(), mask_params_.fixedFrame_param, fixed_output);
                break;
            case MaskType::Blur:
                fixed_result = mask_shader_.ApplyBlur(source_texture, fixed_mask.Get(), mask_params_.fixedFrame_param, fixed_output);
                break;
            case MaskType::RectFill:
            default: {
                float color[4] = {
                    mask_params_.fixframe_color.r / 255.0f,
                    mask_params_.fixframe_color.g / 255.0f,
                    mask_params_.fixframe_color.b / 255.0f,
                    1.0f
                };
                fixed_result = mask_shader_.ApplyRectFill(source_texture, fixed_mask.Get(), color, fixed_output);
                break;
            }
        }
    } else {
        fixed_output = source_texture;
        fixed_output->AddRef();
        fixed_result = true;
    }

    // 両方の結果を合成（固定矩形マスク領域=fixed_output、それ以外=detection_output）
    bool result = false;
    if (has_fixed) {
        result = mask_shader_.CompositeWithMask(detection_output.Get(), fixed_output.Get(), fixed_mask.Get(), output_texture_);
    } else {
        output_texture_ = detection_output;
        result = detection_result;
    }

    if (!result || !output_texture_) {
        output_texture_ = source_texture;
        output_texture_->AddRef();
    }

    std::wstring overridePath = copyright_image_path_override_;
    if (mask_params_.enable_copyright && output_texture_) {
        if (EnsureCopyrightWatermarkLoaded(overridePath)) {
            float scale = std::clamp(mask_params_.copyright_scale, 0.5f, 3.0f);
            uint32_t target_w = std::max<uint32_t>(1, static_cast<uint32_t>(std::round(copyright_width_ * scale)));
            uint32_t target_h = std::max<uint32_t>(1, static_cast<uint32_t>(std::round(copyright_height_ * scale)));
            if (target_w > 0 && target_h > 0) {
                // Use configurable offset from mask_params_
                // Offset is relative to the right-bottom corner
                int pos_x = static_cast<int>(width) - static_cast<int>(target_w) + mask_params_.copyright_offset_x;
                int pos_y = static_cast<int>(height) - static_cast<int>(target_h) + mask_params_.copyright_offset_y;

                const int max_x = static_cast<int>(width) - static_cast<int>(target_w);
                const int max_y = static_cast<int>(height) - static_cast<int>(target_h);
                pos_x = std::max(0, std::min(pos_x, std::max(0, max_x)));
                pos_y = std::max(0, std::min(pos_y, std::max(0, max_y)));

                Microsoft::WRL::ComPtr<ID3D11Texture2D> watermarked;
                if (mask_shader_.ApplyCopyrightOverlay(
                    output_texture_.Get(),
                    copyright_srv_.Get(),
                    target_w,
                    target_h,
                    pos_x,
                    pos_y,
                    watermarked) && watermarked.Get()) {
                    output_texture_ = watermarked;
                }
            }
        }
    }

    return true;
}

bool PreviewPipeline::EnsureCopyrightWatermarkLoaded(const std::wstring& override_path) {
    if (copyright_srv_.Get()) {
        return true;
    }

    std::vector<std::wstring> candidates;
    auto appendCandidate = [&candidates](const std::wstring& path) {
        if (path.empty()) {
            return;
        }
        if (std::find(candidates.begin(), candidates.end(), path) == candidates.end()) {
            candidates.push_back(path);
        }
    };

    auto appendFromDir = [&candidates](const std::wstring& dir) {
        if (dir.empty()) {
            return;
        }
        std::wstring path = dir;
        if (path.back() != L'\\' && path.back() != L'/') {
            path += L"\\";
        }
        path += L"C_SQUARE_ENIX.png";
        if (std::find(candidates.begin(), candidates.end(), path) == candidates.end()) {
            candidates.push_back(path);
        }
    };

    if (!override_path.empty()) {
        appendCandidate(override_path);
    }

    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) > 0) {
        std::wstring exePath(modulePath);
        size_t pos = exePath.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            appendFromDir(exePath.substr(0, pos + 1));
        }
    }

    wchar_t cwd[MAX_PATH] = {};
    if (GetCurrentDirectoryW(MAX_PATH, cwd) > 0) {
        appendFromDir(cwd);
    }

    appendFromDir(L".\\App12\\App12");
    appendFromDir(L".\\App12");

    bool loaded = false;
    for (const auto& imagePath : candidates) {
        DWORD attrs = GetFileAttributesW(imagePath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }

        LogFmt("[PreviewPipeline] Trying watermark: %ls\n", imagePath.c_str());
        if (LoadImageToTexture(imagePath.c_str(), copyright_texture_, copyright_width_, copyright_height_)) {
            loaded = true;
            break;
        }
    }

    if (!loaded) {
        LogToFile("[PreviewPipeline] Failed to load copyright image from all candidates\n");
        return false;
    }

    HRESULT hr = device_->CreateShaderResourceView(
        copyright_texture_.Get(),
        nullptr,
        copyright_srv_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !copyright_srv_.Get()) {
        LogFmt("[PreviewPipeline] Failed to create copyright SRV. hr=0x%08X\n", (unsigned)hr);
        copyright_texture_.Reset();
        copyright_width_ = 0;
        copyright_height_ = 0;
        return false;
    }

    return true;
}

// ============================================================
// マスクパラメータ更新
// ============================================================

void PreviewPipeline::UpdateMaskParams(const PreviewMaskParams& params) {
    std::lock_guard<std::mutex> lock(params_mutex_);
    memcpy(&mask_params_, &params, sizeof(PreviewMaskParams));
}

void PreviewPipeline::SetCopyrightOffset(int offset_x, int offset_y) {
    std::lock_guard<std::mutex> lock(params_mutex_);
    mask_params_.copyright_offset_x = offset_x;
    mask_params_.copyright_offset_y = offset_y;
}

void PreviewPipeline::SetCopyrightImagePath(const wchar_t* image_path) {
    std::lock_guard<std::mutex> lock(params_mutex_);
    if (image_path && image_path[0] != L'\0') {
        copyright_image_path_override_ = image_path;
    } else {
        copyright_image_path_override_.clear();
    }
    copyright_srv_.Reset();
    copyright_texture_.Reset();
    copyright_width_ = 0;
    copyright_height_ = 0;
}

// ============================================================
// テクスチャからRGBAバッファにコピー
// ============================================================

bool PreviewPipeline::CopyTextureToRgbaBuffer(
    ID3D11Texture2D* texture,
    unsigned char* out_buffer,
    int buffer_size,
    int* out_width,
    int* out_height) {

    if (!texture || !out_buffer || !out_width || !out_height) return false;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    *out_width = static_cast<int>(desc.Width);
    *out_height = static_cast<int>(desc.Height);

    int required_size = desc.Width * desc.Height * 4;
    if (buffer_size < required_size) {
        std::cerr << "Buffer too small: " << buffer_size << " < " << required_size << std::endl;
        return false;
    }

    // CPU読み取り可能なステージングテクスチャを作成
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = device_->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        std::cerr << "Failed to create staging texture. HRESULT: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // テクスチャをコピー
    context_->CopyResource(stagingTexture.Get(), texture);

    // マップして読み取り
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context_->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::cerr << "Failed to map staging texture. HRESULT: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // 行ごとにコピー
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = out_buffer;
    size_t row_pitch = mapped.RowPitch;
    size_t copy_width = desc.Width * 4;

    for (uint32_t y = 0; y < desc.Height; ++y) {
        memcpy(dst + y * copy_width, src + y * row_pitch, copy_width);
    }

    context_->Unmap(stagingTexture.Get(), 0);

    return true;
}

// ============================================================
// 画像ファイルをD3D11テクスチャに読み込み（WIC使用）
// ============================================================

bool PreviewPipeline::LoadImageToTexture(
    const wchar_t* file_path,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& out_texture,
    uint32_t& out_width,
    uint32_t& out_height) {

    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool shouldUninitialize = SUCCEEDED(coInitHr);
    if (coInitHr == RPC_E_CHANGED_MODE) {
        shouldUninitialize = false;
    }

    // WICファクトリ作成
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory)
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to create WIC factory. HRESULT: 0x" << std::hex << hr << std::endl;
        if (shouldUninitialize) CoUninitialize();
        return false;
    }

    // デコーダー作成
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = wicFactory->CreateDecoderFromFilename(
        file_path,
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &decoder
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to create WIC decoder. HRESULT: 0x" << std::hex << hr << std::endl;
        if (shouldUninitialize) CoUninitialize();
        return false;
    }

    // フレーム取得
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        std::cerr << "Failed to get WIC frame. HRESULT: 0x" << std::hex << hr << std::endl;
        if (shouldUninitialize) CoUninitialize();
        return false;
    }

    // サイズ取得
    UINT width, height;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) {
        if (shouldUninitialize) CoUninitialize();
        return false;
    }

    out_width = width;
    out_height = height;

    // ピクセルフォーマット変換（BGRA32）
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        if (shouldUninitialize) CoUninitialize();
        return false;
    }

    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeMedianCut
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize format converter. HRESULT: 0x" << std::hex << hr << std::endl;
        if (shouldUninitialize) CoUninitialize();
        return false;
    }

    // ピクセルデータ読み取り
    const auto decodeStart = PreviewClock::now();
    size_t buffer_size = width * height * 4;
    std::vector<BYTE> pixelData(buffer_size);
    hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(buffer_size), pixelData.data());
    if (FAILED(hr)) {
        std::cerr << "Failed to copy pixels. HRESULT: 0x" << std::hex << hr << std::endl;
        if (shouldUninitialize) CoUninitialize();
        return false;
    }
    const double wicDecodeMs = ElapsedMilliseconds(decodeStart);

    // D3D11テクスチャ作成
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixelData.data();
    initData.SysMemPitch = width * 4;

    const auto uploadStart = PreviewClock::now();
    hr = device_->CreateTexture2D(&texDesc, &initData, &out_texture);
    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 texture. HRESULT: 0x" << std::hex << hr << std::endl;
        if (shouldUninitialize) CoUninitialize();
        return false;
    }
    LogFmt("[PreviewTiming] wic_decode_ms=%.3f texture_upload_ms=%.3f\n",
        wicDecodeMs, ElapsedMilliseconds(uploadStart));

    if (shouldUninitialize) CoUninitialize();
    return true;
}
