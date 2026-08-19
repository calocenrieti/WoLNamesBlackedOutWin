// dllmain.cpp : DLL アプリケーションのエントリ ポイントを定義します。
#include "pch.h"
#include "CoreTypes.h"
#include "D3D11DeviceManager.h"
#include "VideoPipeline.h"
#include "PreviewPipeline.h"

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <algorithm>

// ============================================================
// グローバル状態管理
// ============================================================

static std::unique_ptr<D3D11DeviceManager> g_deviceManager;
static std::unique_ptr<WoLNamesBlackedOut::Core::VideoPipeline> g_videoPipeline;
static std::unique_ptr<WoLNamesBlackedOut::Core::PreviewPipeline> g_previewPipeline;
static std::mutex g_stateMutex;
static std::atomic<bool> g_cancelFlag{ false };

// GPUベンダー情報のキャッシュ
static char g_gpuVendorChar = 'X';
static bool g_gpuVendorInitialized = false;

// C#側への状態通知コールバック（EPダウンロード・コンパイル進捗用）
static StatusCallback g_statusCallback = nullptr;
static std::mutex g_statusCallbackMutex;

void ReportStatus(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_statusCallbackMutex);
    if (!g_statusCallback) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_statusCallback(buf);
}

// ============================================================
// 内部ユーティリティ
// ============================================================

static char GpuVendorToChar(WoLNamesBlackedOut::Core::GpuVendor vendor) {
    switch (vendor) {
        case WoLNamesBlackedOut::Core::GpuVendor::NVIDIA: return 'N';
        case WoLNamesBlackedOut::Core::GpuVendor::AMD:    return 'A';
        case WoLNamesBlackedOut::Core::GpuVendor::Intel:  return 'I';
        default: return 'X';
    }
}

static WoLNamesBlackedOut::Core::HwEncoderType EncoderTypeFromString(const char* codec) {
    if (!codec) return WoLNamesBlackedOut::Core::HwEncoderType::Auto;
    std::string s(codec);
    if (s.find("nvenc") != std::string::npos) return WoLNamesBlackedOut::Core::HwEncoderType::NVENC;
    if (s.find("qsv") != std::string::npos)   return WoLNamesBlackedOut::Core::HwEncoderType::QSV;
    if (s.find("amf") != std::string::npos)   return WoLNamesBlackedOut::Core::HwEncoderType::AMF;
    return WoLNamesBlackedOut::Core::HwEncoderType::Auto;
}

static WoLNamesBlackedOut::Core::MaskType MaskTypeFromString(const char* typeStr) {
    if (!typeStr) return WoLNamesBlackedOut::Core::MaskType::RectFill;
    std::string s(typeStr);
    if (s == "Solid")        return WoLNamesBlackedOut::Core::MaskType::RectFill;
    if (s == "Inpaint")       return WoLNamesBlackedOut::Core::MaskType::Inpaint;
    if (s == "Mosaic")        return WoLNamesBlackedOut::Core::MaskType::Mosaic;
    if (s == "Blur")          return WoLNamesBlackedOut::Core::MaskType::Blur;
    if (s == "NoInference")   return WoLNamesBlackedOut::Core::MaskType::No_Inference;
    if (s == "No_Inference")  return WoLNamesBlackedOut::Core::MaskType::No_Inference;
    return WoLNamesBlackedOut::Core::MaskType::RectFill;
}

static WoLNamesBlackedOut::Core::MaskType MaskTypeFromInt(int value) {
    switch (value) {
        case 0: return WoLNamesBlackedOut::Core::MaskType::Inpaint;
        case 1: return WoLNamesBlackedOut::Core::MaskType::Mosaic;
        case 2: return WoLNamesBlackedOut::Core::MaskType::Blur;
        case 4: return WoLNamesBlackedOut::Core::MaskType::No_Inference;
        case 3:
        default:
            return WoLNamesBlackedOut::Core::MaskType::RectFill;
    }
}

// ============================================================
// C互換エクスポート関数
// ============================================================

extern "C" __declspec(dllexport) void __stdcall SetStatusCallback(StatusCallback callback) {
    std::lock_guard<std::mutex> lock(g_statusCallbackMutex);
    g_statusCallback = callback;
}

extern "C" __declspec(dllexport) char __stdcall GetGpuVendor() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (!g_gpuVendorInitialized) {
        if (!g_deviceManager) {
            g_deviceManager = std::make_unique<D3D11DeviceManager>();
            if (!g_deviceManager->Initialize()) {
                g_gpuVendorChar = 'X';
                g_gpuVendorInitialized = true;
                return g_gpuVendorChar;
            }
        }
        g_gpuVendorChar = GpuVendorToChar(g_deviceManager->GetGpuVendor());
        g_gpuVendorInitialized = true;
    }
    return g_gpuVendorChar;
}

extern "C" __declspec(dllexport) int __stdcall ProcessVideo(
    const char* input_video_path,
    const char* output_video_path,
    const char* codec,
    const char* hwaccel,
    int width, int height, int fps,
    double trim_start_seconds,
    double trim_end_seconds,
    float conf_threshold,
    const char* color_primaries,
    const RectInfo* rects, int count,
    ColorInfo name_color, ColorInfo fixframe_color,
    bool copyright,
    int blackedOut,
    int fixedFrame,
    int blackedout_param,
    int fixedFrame_param,
    // C# P/Invoke と同順序で受け取る
    int copyright_offset_x,
    int copyright_offset_y,
    float copyright_scale,
    const char* copyright_image_path,
    bool exclude_by_name_enabled,
    int ocr_expand_pixels,
    int ocr_max_rois_per_frame,
    float text_similarity_threshold,
    const char* mask_exclude_text_csv,
    const char* bitrate,
    const char* preset,
    bool disable_audio,
    int crop_top,
    int crop_left,
    int crop_right,
    int crop_bottom,
    ProgressCallback progress_callback
) {
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_cancelFlag.store(false);

        if (!g_deviceManager) {
            g_deviceManager = std::make_unique<D3D11DeviceManager>();
            if (!g_deviceManager->Initialize()) {
                return -1; // D3D11初期化失敗
            }
        }
    }

    // ワイド文字列に変換（UTF-8 → UTF-16）
    auto toWide = [](const char* s) -> std::wstring {
        if (!s) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (len <= 0) return {};
        std::wstring result(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &result[0], len);
        return result;
    };
    std::wstring winput  = toWide(input_video_path);
    std::wstring woutput = toWide(output_video_path);
    std::wstring wcopyrightCustom = toWide(copyright_image_path);
    std::wstring wexcludeCsv = toWide(mask_exclude_text_csv);

    // モデルパス（実行ディレクトリのONNXモデル）
    wchar_t modulePath[MAX_PATH];
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring wmodelPath(modulePath);
    size_t pos = wmodelPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        wmodelPath = wmodelPath.substr(0, pos + 1);
    }
    wmodelPath += L"my_yolov8m_s.onnx";
    std::wstring wocrModelPath(modulePath);
    size_t op = wocrModelPath.find_last_of(L"\\/");
    if (op != std::wstring::npos) {
        wocrModelPath = wocrModelPath.substr(0, op + 1);
    }
    wocrModelPath += L"c_ppocr-v5-rec_sim.onnx";

    std::wstring wocrDictPath(modulePath);
    size_t odp = wocrDictPath.find_last_of(L"\\/");
    if (odp != std::wstring::npos) {
        wocrDictPath = wocrDictPath.substr(0, odp + 1);
    }
    wocrDictPath += L"ppocrv5_en_dict.txt";
    std::wstring wcopyrightPath(modulePath);
    size_t cpos = wcopyrightPath.find_last_of(L"\\/");
    if (cpos != std::wstring::npos) {
        wcopyrightPath = wcopyrightPath.substr(0, cpos + 1);
    }
    wcopyrightPath += L"C_SQUARE_ENIX.png";

    // ビットレートをパース（例: "5M" → 5000000）
    int bitrateValue = 5'000'000;
    if (bitrate) {
        char* end = nullptr;
        long val = strtol(bitrate, &end, 10);
        if (end && (*end == 'M' || *end == 'm')) bitrateValue = (int)(val * 1'000'000);
        else if (val > 0) bitrateValue = (int)val;
    }

    WoLNamesBlackedOut::Core::PipelineConfig config;
    config.input_path  = winput.c_str();
    config.output_path = woutput.c_str();
    config.model_path  = wmodelPath.c_str();
    config.input_width    = 640;
    config.input_height   = 640;
    // PreviewPipelineと同じしきい値に合わせる
    config.conf_threshold = std::clamp(conf_threshold, 0.01f, 0.50f);
    config.iou_threshold  = 0.45f;
    config.encoder_type   = EncoderTypeFromString(codec);
    config.for_x          = (codec != nullptr) && (std::strstr(codec, "h264") != nullptr);
    char codecDbg[256] = {};
    _snprintf_s(codecDbg, sizeof(codecDbg), _TRUNCATE,
        "[ProcessVideo] codec=%s for_x=%d fps=%d conf=%.3f size=%dx%d disable_audio=%d\n",
        codec ? codec : "(null)",
        config.for_x ? 1 : 0,
        fps,
        config.conf_threshold,
        width,
        height,
        disable_audio ? 1 : 0);
    OutputDebugStringA(codecDbg);
    config.bitrate        = bitrateValue;
    config.fps            = fps;
    config.disable_audio  = disable_audio;
    config.trim_start_seconds = std::max(0.0, trim_start_seconds);
    config.trim_end_seconds = trim_end_seconds;
    config.blacked_type    = MaskTypeFromInt(blackedOut);
    config.blackedout_param = blackedout_param;
    config.name_color      = name_color;
    config.fixframe_color  = fixframe_color;
    config.fixmask_type    = MaskTypeFromInt(fixedFrame);
    config.fixmask_param   = fixedFrame_param;
    // copyright 位置・拡大率を出力設定へ受け渡し
    config.copyright_offset_x = copyright_offset_x;
    config.copyright_offset_y = copyright_offset_y;
    config.copyright_scale = std::clamp(copyright_scale, 0.5f, 3.0f);
    config.exclude_by_name_enabled = exclude_by_name_enabled;
    config.ocr_expand_pixels = std::clamp(ocr_expand_pixels, 0, 5);
    config.ocr_max_rois_per_frame = std::clamp(ocr_max_rois_per_frame, 1, 32);
    config.text_similarity_threshold = std::clamp(text_similarity_threshold, 0.50f, 1.00f);
    config.mask_exclude_text_csv = wexcludeCsv.c_str();
    config.ocr_model_path = wocrModelPath.c_str();
    config.ocr_dict_path = wocrDictPath.c_str();
    config.fixed_rect_count = count;
    for (int i = 0; i < count && i < 64; ++i) {
        config.fixed_rects[i] = rects[i];
    }
    config.crop_top = std::max(0, crop_top);
    config.crop_left = std::max(0, crop_left);
    config.crop_right = std::max(0, crop_right);
    config.crop_bottom = std::max(0, crop_bottom);
    config.enable_copyright = copyright;
    config.copyright_image_path = (!wcopyrightCustom.empty() ? wcopyrightCustom.c_str() : wcopyrightPath.c_str());

    if (g_cancelFlag.load()) {
        return -4; // キャンセル要求
    }

    WoLNamesBlackedOut::Core::VideoPipeline* pipeline = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_videoPipeline = std::make_unique<WoLNamesBlackedOut::Core::VideoPipeline>();
        pipeline = g_videoPipeline.get();
    }

    if (!pipeline->Initialize(config)) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_videoPipeline.reset();
        return -2; // パイプライン初期化失敗
    }

    if (g_cancelFlag.load()) {
        pipeline->Stop();
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_videoPipeline.reset();
        return -4; // キャンセル要求
    }

    // Run() はミューテックスロックなしで呼ぶ（CancelProcess() のデッドロック防止）
    // 進捗コールバック用のスレッドを開始
    std::atomic<bool> progress_running{true};
    std::thread progress_thread;
    if (progress_callback) {
        auto progress_start = std::chrono::high_resolution_clock::now();
        progress_thread = std::thread([&progress_running, progress_callback, progress_start]() {
            while (progress_running.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto now = std::chrono::high_resolution_clock::now();
                double elapsed = std::chrono::duration<double>(now - progress_start).count();
                std::lock_guard<std::mutex> lock(g_stateMutex);
                if (g_videoPipeline) {
                    int processed = 0, total = 0;
                    double dummy_elapsed = 0.0;
                    g_videoPipeline->GetProgress(processed, total, dummy_elapsed);
                    progress_callback(processed, total, elapsed);
                }
            }
        });
    }

    auto result = pipeline->Run();

    // 進捗スレッドを停止
    progress_running = false;
    if (progress_thread.joinable()) {
        progress_thread.join();
    }

    // 最終進捗を送信
    if (progress_callback) {
        progress_callback(result.processed_frames, result.total_frames, result.elapsed_seconds);
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_videoPipeline.reset();
    }

    return result.success ? 0 : -3;
}

extern "C" __declspec(dllexport) bool __stdcall CancelProcess() {
    g_cancelFlag.store(true);
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (g_videoPipeline) {
        g_videoPipeline->Stop();
    }
    return true;
}

extern "C" __declspec(dllexport) int __stdcall GetLatestProcessedPreviewFrame(
    unsigned char* out_bgra_buffer,
    int buffer_size,
    int* out_width,
    int* out_height,
    int* out_frame_index
) {
    if (!out_bgra_buffer || buffer_size <= 0 || !out_width || !out_height || !out_frame_index) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (!g_videoPipeline) {
        return -2;
    }

    int width = 0;
    int height = 0;
    int frame_index = -1;
    if (!g_videoPipeline->TryCopyLatestProcessedPreviewFrame(
            out_bgra_buffer,
            buffer_size,
            width,
            height,
            frame_index)) {
        return -3;
    }

    *out_width = width;
    *out_height = height;
    *out_frame_index = frame_index;
    return 0;
}

// ============================================================
// プレビューAPI
// ============================================================

extern "C" __declspec(dllexport) int __stdcall PreviewOpen(
    const PreviewParams* params
) {
    if (!params) return -1;

    std::lock_guard<std::mutex> lock(g_stateMutex);

    if (!g_deviceManager) {
        g_deviceManager = std::make_unique<D3D11DeviceManager>();
        if (!g_deviceManager->Initialize()) {
            return -1;
        }
    }

    g_previewPipeline.reset();
    g_previewPipeline = std::make_unique<WoLNamesBlackedOut::Core::PreviewPipeline>();

    if (!g_previewPipeline->Initialize(
            params->file_path,
            params->model_path,
            params->conf_threshold,
            params->iou_threshold,
            g_deviceManager.get())) {
        g_previewPipeline.reset();
        return -2;
    }

    if (params->copyright_image_path && params->copyright_image_path[0] != L'\0') {
        g_previewPipeline->SetCopyrightImagePath(params->copyright_image_path);
    }
    return 0;
}

extern "C" __declspec(dllexport) int __stdcall PreviewGetFrame(
    int frame_index,
    unsigned char* out_rgba_buffer,
    int buffer_size,
    int* out_width,
    int* out_height
) {
    std::lock_guard<std::mutex> lock(g_stateMutex);

    if (!g_previewPipeline || !out_rgba_buffer || !out_width || !out_height) return -1;

    if (!g_previewPipeline->GetFrame(frame_index, out_rgba_buffer, buffer_size, out_width, out_height)) {
        return -2;
    }
    return 0;
}

extern "C" __declspec(dllexport) int __stdcall PreviewGetDimensions(
    int* out_width,
    int* out_height
) {
    std::lock_guard<std::mutex> lock(g_stateMutex);

    if (!g_previewPipeline || !out_width || !out_height) return -1;

    *out_width = static_cast<int>(g_previewPipeline->GetWidth());
    *out_height = static_cast<int>(g_previewPipeline->GetHeight());
    return (*out_width > 0 && *out_height > 0) ? 0 : -2;
}

extern "C" __declspec(dllexport) int __stdcall PreviewUpdateParams(
    const PreviewMaskParams* params
) {
    std::lock_guard<std::mutex> lock(g_stateMutex);

    if (!g_previewPipeline || !params) return -1;
    g_previewPipeline->UpdateMaskParams(*params);
    return 0;
}

extern "C" __declspec(dllexport) int __stdcall PreviewSetCopyrightOffset(
    int offset_x,
    int offset_y
) {
    std::lock_guard<std::mutex> lock(g_stateMutex);

    if (!g_previewPipeline) return -1;
    g_previewPipeline->SetCopyrightOffset(offset_x, offset_y);
    return 0;
}

extern "C" __declspec(dllexport) int __stdcall PreviewSetCopyrightImagePath(
    const wchar_t* image_path
) {
    std::lock_guard<std::mutex> lock(g_stateMutex);

    if (!g_previewPipeline) return -1;
    g_previewPipeline->SetCopyrightImagePath(image_path);
    return 0;
}

extern "C" __declspec(dllexport) int __stdcall PreviewClose() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_previewPipeline.reset();
    return 0;
}

// ============================================================
// DLLエントリポイント
// ============================================================

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        // グローバルリソース解放
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_previewPipeline.reset();
            g_videoPipeline.reset();
            g_deviceManager.reset();
        }
        break;
    }
    return TRUE;
}

