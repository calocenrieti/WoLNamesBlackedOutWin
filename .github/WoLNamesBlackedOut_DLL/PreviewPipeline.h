#pragma once

#include "CoreTypes.h"
#include "D3D11DeviceManager.h"
#include "WinMLUtils.h"
#include "MaskShader.h"
#include "ByteTrackInterop.h"
#include "OcrRecognizer.h"

#include <d3d11.h>
#include <wrl/client.h>

// FFmpeg
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace WoLNamesBlackedOut::Core {

/**
 * @brief プレビュー専用パイプライン（動画・静止画両対応）
 * 推論結果をキャッシュし、マスク設定変更時はマスク適用のみ再実行
 */
class PreviewPipeline {
public:
    PreviewPipeline();
    ~PreviewPipeline();

    PreviewPipeline(const PreviewPipeline&) = delete;
    PreviewPipeline& operator=(const PreviewPipeline&) = delete;

    /**
     * @brief プレビューパイプラインを初期化
     * @param file_path 入力ファイルパス（.mp4 / .jpg / .png）
     * @param model_path ONNXモデルパス
     * @param device_manager D3D11デバイスマネージャー
     * @return 成功時 true
     */
    bool Initialize(
        const wchar_t* file_path,
        const wchar_t* model_path,
        float conf_threshold,
        float iou_threshold,
        D3D11DeviceManager* device_manager
    );

    /**
     * @brief リソース解放
     */
    void Cleanup();

    /**
     * @brief 指定フレームを取得（RGBAバッファに書き出し）
     * @param frame_index フレームインデックス（動画用、静止画は無視）
     * @param out_rgba_buffer 出力RGBAバッファ（呼び出し側で確保）
     * @param buffer_size バッファサイズ（バイト）
     * @param out_width 出力画像幅
     * @param out_height 出力画像高さ
     * @return 成功時 true
     */
    bool GetFrame(
        int frame_index,
        unsigned char* out_rgba_buffer,
        int buffer_size,
        int* out_width,
        int* out_height
    );

    /**
     * @brief マスクパラメータを更新（推論結果キャッシュを再利用）
     * @param params マスクパラメータ
     */
    void UpdateMaskParams(const PreviewMaskParams& params);

    /**
     * @brief Copyright image offset を設定
     * @param offset_x X position offset
     * @param offset_y Y position offset
     */
    void SetCopyrightOffset(int offset_x, int offset_y);

    /**
     * @brief Copyright画像パスを設定（空ならデフォルト画像）
     */
    void SetCopyrightImagePath(const wchar_t* image_path);

    /**
     * @brief ファイルが動画かどうか
     * @return 動画時 true
     */
    bool IsVideo() const { return is_video_; }

    /**
     * @brief 総フレーム数を取得（動画用）
     * @return 総フレーム数
     */
    int GetTotalFrames() const { return total_frames_; }

    /**
     * @brief FPSを取得（動画用）
     * @return FPS
     */
    int GetFps() const { return fps_; }

private:
    // 初期化ヘルパー
    bool InitializeVideo(const wchar_t* file_path);
    bool InitializeImage(const wchar_t* file_path);

    // フレーム取得ヘルパー
    bool GetVideoFrame(int frame_index);
    bool GetImageFrame();

    // 推論実行（1回のみ、結果をキャッシュ）
    bool RunInference(ID3D11Texture2D* source_texture, uint32_t width, uint32_t height);

    // マスク適用（推論結果キャッシュを使用）
    bool ApplyMask(ID3D11Texture2D* source_texture, uint32_t width, uint32_t height);

    // テクスチャからRGBAバッファにコピー
    bool CopyTextureToRgbaBuffer(
        ID3D11Texture2D* texture,
        unsigned char* out_buffer,
        int buffer_size,
        int* out_width,
        int* out_height
    );

    // 画像ファイルをD3D11テクスチャに読み込み
    bool LoadImageToTexture(const wchar_t* file_path, Microsoft::WRL::ComPtr<ID3D11Texture2D>& out_texture, uint32_t& out_width, uint32_t& out_height);

    // 透かし画像を初回のみロード
    bool EnsureCopyrightWatermarkLoaded(const std::wstring& override_path = L"");

    // 静止画用テクスチャをRGBAに変換
    bool ConvertImageToRgbaTexture(ID3D11Texture2D* source_texture, uint32_t width, uint32_t height, Microsoft::WRL::ComPtr<ID3D11Texture2D>& out_rgba_texture);

private:
    // D3D11
    D3D11DeviceManager* device_manager_;
    ID3D11Device* device_;
    ID3D11DeviceContext* context_;

    // FFmpeg（動画用）
    AVFormatContext* format_ctx_;
    AVCodecContext* decoder_ctx_;
    int video_stream_index_;
    AVBufferRef* hw_device_ctx_;

    // WinML
    WinMLUtils winml_utils_;

    // マスクシェーダー
    MaskShader mask_shader_;

    // ファイル情報
    bool is_video_;
    std::wstring file_path_;
    int total_frames_;
    int fps_;
    int video_width_;
    int video_height_;
    float conf_threshold_;
    float iou_threshold_;

    // 推論結果キャッシュ
    std::vector<Detection> cached_detections_;
    bool inference_done_;
    std::mutex inference_mutex_;

    // 名前除外（ByteTrack + OCR）
    std::unique_ptr<ByteTrackInterop::Tracker> tracker_;
    OcrRecognizer ocr_recognizer_;
    std::unordered_map<uint64_t, std::string> ocr_text_by_track_id_;
    std::unordered_map<uint64_t, int> ocr_last_frame_by_track_;
    int preview_frame_counter_ = 0;

    // マスクパラメータ
    PreviewMaskParams mask_params_;
    std::mutex params_mutex_;

    // 静止画用テクスチャキャッシュ
    Microsoft::WRL::ComPtr<ID3D11Texture2D> image_texture_;
    uint32_t image_width_;
    uint32_t image_height_;

    // 透かしリソース
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copyright_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> copyright_srv_;
    uint32_t copyright_width_;
    uint32_t copyright_height_;
    std::wstring copyright_image_path_override_;

    // 作業用テクスチャ
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rgba_texture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_texture_;
};

} // namespace WoLNamesBlackedOut::Core