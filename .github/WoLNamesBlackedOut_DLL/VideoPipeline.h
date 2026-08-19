#pragma once

// DirectX 11
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

#include "D3D11DeviceManager.h"
#include "WinMLUtils.h"
#include "MaskShader.h"
#include "CoreTypes.h"
#include "ByteTrackInterop.h"
#include "OcrRecognizer.h"

#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace WoLNamesBlackedOut::Core {

/**
 * @brief 動画処理パイプラインクラス
 * 3スレッド構成: Decode/Inference/Encode
 */
class VideoPipeline {
public:
	VideoPipeline();
	~VideoPipeline();

	// コピーコンストラクタと代入演算子を削除
	VideoPipeline(const VideoPipeline&) = delete;
	VideoPipeline& operator=(const VideoPipeline&) = delete;

	/**
	 * @brief パイプラインを初期化
	 * @param config パイプライン設定
	 * @return 成功時 true
	 */
	bool Initialize(const PipelineConfig& config);

	/**
	 * @brief パイプラインを実行
	 * @return 実行結果
	 */
	PipelineResult Run();

	/**
	 * @brief パイプラインを停止
	 */
	void Stop();

	/**
	 * @brief パイプラインが実行中か
	 * @return 実行中 true
	 */
	bool IsRunning() const { return running_.load(); }

	/**
	 * @brief 現在の進捗を取得（スレッドセーフ）
	 */
	void GetProgress(int& processed_frames, int& total_frames, double& elapsed_seconds) const {
		processed_frames = processed_frames_.load();
		total_frames = estimated_total_frames_.load();
		elapsed_seconds = elapsed_seconds_.load();
	}

	/**
	 * @brief 最新の処理済みプレビューフレームを取得
	 * @param out_bgra 出力BGRAバッファ（呼び出し元確保）
	 * @param buffer_size out_bgra のバイト数
	 * @param out_width フレーム幅
	 * @param out_height フレーム高さ
	 * @param out_frame_index 処理済みフレーム番号（0-based）
	 * @return 取得成功時 true
	 */
	bool TryCopyLatestProcessedPreviewFrame(uint8_t* out_bgra,
		int buffer_size,
		int& out_width,
		int& out_height,
		int& out_frame_index) const;

private:
	// スレッド関数
	void DecodeThread();
	void InferenceThread();
	void EncodeThread();
	void OcrThread();

	// キュー操作
	void PushVideoFrame(const GpuFrame& frame);
	void PushAudioPacket(const AVPacket* packet);
	GpuFrame PopVideoFrame();
	AVPacket* PopAudioPacket();

	// 前処理
	PreProcessResult LetterboxTransform(const GpuFrame& input);

	// 後処理 - ONNX Runtime出力（std::vector<float>）を受ける形式に更新
	std::vector<Detection> PostProcessDetections(const std::vector<float>& output_data, 
												 const PreProcessResult& preproc,
												 uint32_t original_width,
												 uint32_t original_height);

	// NMS（Non-Maximum Suppression）
	std::vector<Detection> NonMaxSuppression(std::vector<Detection>& detections, float iou_threshold);

	// Letterboxシェーダー
	bool InitializeLetterboxShader();
	bool PerformLetterboxTransform(const GpuFrame& input, PreProcessResult& preproc);
	bool LoadImageToTexture(const wchar_t* file_path, Microsoft::WRL::ComPtr<ID3D11Texture2D>& out_texture, uint32_t& out_width, uint32_t& out_height);
	bool EnsureCopyrightWatermarkLoaded();
	struct LetterboxConstantBuffer {
		float scale_x;
		float scale_y;
		float pad_left;
		float pad_top;
		float input_width;
		float input_height;
		float output_width;
		float output_height;
		float _padding[4]; // 8 used + 4 padding = 12 floats = 48 bytes (16-byte aligned)
	};

	// リソース解放
public:
	void Cleanup();
private:

	// 設定
	PipelineConfig config_;

	// D3D11デバイス
	D3D11DeviceManager device_manager_;

	// WinML
	WinMLUtils winml_utils_;

	// FFmpegコンテキスト
	AVFormatContext* input_format_ctx_;
	AVFormatContext* output_format_ctx_;
	AVCodecContext* decoder_ctx_;
	AVCodecContext* encoder_ctx_;
	AVCodecContext* audio_encoder_ctx_;

	// HWフレームコンテキスト
	AVBufferRef* hw_device_ctx_;
	AVBufferRef* hw_frames_ctx_;

	// ストリーム
	AVStream* input_audio_stream_;

	// ストリームインデックス
	int video_stream_index_;
	int audio_stream_index_;
	int output_video_stream_index_;
	int output_audio_stream_index_;

	// キュー
	std::queue<GpuFrame> video_queue_;
	std::queue<AVPacket*> audio_queue_;
	std::queue<GpuFrame> inference_output_queue_; // 推論結果キュー
	std::mutex video_mutex_;
	std::mutex audio_mutex_;
	std::mutex inference_mutex_;
	std::condition_variable video_cv_;
	std::condition_variable audio_cv_;
	std::condition_variable inference_cv_;

	// 制御
	std::atomic<bool> running_;
	std::atomic<bool> stop_requested_;
	std::atomic<bool> decode_finished_;
	std::atomic<bool> encode_failed_;
	bool use_d3d11_hw_encode_;

	// スレッド
	std::thread decode_thread_;
	std::thread inference_thread_;
	std::thread encode_thread_;

	// 統計
	std::atomic<int> processed_frames_;
	std::atomic<int> total_frames_;
	std::atomic<int> detected_objects_;
	std::atomic<double> elapsed_seconds_;
	std::atomic<int> estimated_total_frames_;

	struct StageTimingStats {
		uint64_t count = 0;
		double total_ms = 0.0;
		double max_ms = 0.0;

		void Reset() {
			count = 0;
			total_ms = 0.0;
			max_ms = 0.0;
		}

		void Add(double ms) {
			count++;
			total_ms += ms;
			if (ms > max_ms) {
				max_ms = ms;
			}
		}

		double AvgMs() const {
			return count > 0 ? (total_ms / static_cast<double>(count)) : 0.0;
		}
	};

	StageTimingStats decode_stage_ms_;
	StageTimingStats inference_stage_ms_;
	StageTimingStats ocr_stage_ms_;
	StageTimingStats encode_stage_ms_;
	StageTimingStats decode_queue_wait_ms_;
	StageTimingStats encode_queue_wait_ms_;

	// 推論用
	Microsoft::WRL::ComPtr<ID3D11Texture2D> letterbox_texture_;
	float letterbox_scale_x_;
	float letterbox_scale_y_;
	float letterbox_pad_left_;
	float letterbox_pad_top_;

	// Letterboxシェーダー
	Microsoft::WRL::ComPtr<ID3D11VertexShader> letterbox_vertex_shader_;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> letterbox_pixel_shader_;
	Microsoft::WRL::ComPtr<ID3D11InputLayout> letterbox_input_layout_;
	Microsoft::WRL::ComPtr<ID3D11Buffer> letterbox_constant_buffer_;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> letterbox_sampler_;

	// マスクシェーダー
	MaskShader mask_shader_;
	uint32_t mosaic_size_;
	uint32_t blur_radius_;

	// 透かしリソース
	Microsoft::WRL::ComPtr<ID3D11Texture2D> copyright_texture_;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> copyright_srv_;
	uint32_t copyright_width_;
	uint32_t copyright_height_;

	// 固定矩形データ（InitializePipelineからコピー）
	int fixed_rect_count_;
	RectInfo fixed_rects_[64];

	// 名前除外（ByteTrack + OCR）
	struct OcrRequest {
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		uint32_t width = 0;
		uint32_t height = 0;
		int frame_number = 0;
		int inference_counter = 0;
		std::vector<OcrTrackRoi> rois;
	};

	std::unique_ptr<ByteTrackInterop::Tracker> tracker_;
	OcrRecognizer ocr_recognizer_;
	std::queue<OcrRequest> ocr_queue_;
	std::mutex ocr_mutex_;
	std::condition_variable ocr_cv_;
	std::thread ocr_thread_;
	std::atomic<bool> ocr_stop_requested_{ false };
	std::mutex ocr_result_mutex_;
	std::unordered_set<uint64_t> ocr_pending_track_ids_;
	std::unordered_map<uint64_t, std::string> ocr_text_by_track_id_;
	std::unordered_map<uint64_t, int> ocr_last_frame_by_track_;
	std::vector<std::string> mask_exclude_texts_;
	int inference_frame_counter_ = 0;
	int ocr_refresh_interval_frames_ = 30;

	// 処理済み最新プレビュー（EncodeThreadで更新）
	mutable std::mutex latest_processed_preview_mutex_;
	std::vector<uint8_t> latest_processed_preview_bgra_;
	int latest_processed_preview_width_ = 0;
	int latest_processed_preview_height_ = 0;
	std::atomic<int> latest_processed_preview_frame_index_{ -1 };
	mutable std::atomic<ULONGLONG> latest_processed_preview_pull_tick_ms_{ 0 };
};

} // namespace WoLNamesBlackedOut::Core

// 注: C#からの呼び出し用エクスポート関数は dllmain.cpp に集約しました。
// 旧 InitializePipeline/RunPipeline/StopPipeline/CleanupPipeline は削除。
