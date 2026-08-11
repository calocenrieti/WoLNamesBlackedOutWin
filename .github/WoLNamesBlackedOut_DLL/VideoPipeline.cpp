#include "pch.h"
#include "VideoPipeline.h"
#include "WinMLUtils.h"
#include "TextMatchUtils.h"

// WinML 2.0 ヘッダー（pch.hでinclude済みだが、型定義を明示的に再確保）
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Microsoft.Windows.AI.MachineLearning.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include <iostream>
#include <chrono>
#include <codecvt>
#include <locale>
#include <algorithm>
#include <sstream>
#include <iomanip>

using namespace WoLNamesBlackedOut::Core;

// ============================================================
// デバッグログユーティリティ（OutputDebugStringA + コンソール）
// ============================================================
static void PipelineLog(const char* msg) {
	OutputDebugStringA(msg);
	std::cout << msg;
}

template<typename... Args>
static void PipelineLogFmt(const char* fmt, Args... args) {
	char buf[1024] = {};
	snprintf(buf, sizeof(buf), fmt, args...);
	PipelineLog(buf);
}

static double ElapsedMs(const std::chrono::high_resolution_clock::time_point& start,
	const std::chrono::high_resolution_clock::time_point& end) {
	return std::chrono::duration<double, std::milli>(end - start).count();
}

// Letterboxシェーダーのソースコード
static const char* letterbox_vertex_shader_src = 
    "cbuffer ConstantBuffer : register(b0) {\n"
    "    float2 scale;\n"
    "    float2 pad;\n"
    "    float2 input_size;\n"
    "    float2 output_size;\n"
    "};\n"
    "\n"
    "struct VS_INPUT {\n"
    "    float4 pos : POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "\n"
    "struct PS_INPUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "\n"
    "PS_INPUT main(VS_INPUT input) {\n"
    "    PS_INPUT output;\n"
    "    output.pos = input.pos;\n"
    "    output.uv = input.uv * scale + pad;\n"
    "    return output;\n"
    "}\n";

static const char* letterbox_pixel_shader_src = 
    "cbuffer ConstantBuffer : register(b0) {\n"
    "    float2 scale;\n"
    "    float2 pad;\n"
    "    float2 input_size;\n"
    "    float2 output_size;\n"
    "};\n"
    "struct PS_INPUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "Texture2D inputTexture : register(t0);\n"
    "SamplerState samplerState : register(s0);\n"
    "\n"
    "float4 main(PS_INPUT input) : SV_TARGET {\n"
    "    float2 uv = (input.uv - pad) / scale;\n"
    "    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {\n"
    "        return float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
    "    }\n"
    "    return inputTexture.Sample(samplerState, uv);\n"
    "}\n";

namespace WoLNamesBlackedOut::Core {

VideoPipeline::VideoPipeline()
	: input_format_ctx_(nullptr), output_format_ctx_(nullptr),
	  decoder_ctx_(nullptr), encoder_ctx_(nullptr), audio_encoder_ctx_(nullptr),
	  hw_device_ctx_(nullptr), hw_frames_ctx_(nullptr),
	  video_stream_index_(-1), audio_stream_index_(-1),
	  output_video_stream_index_(-1), output_audio_stream_index_(-1),
	  running_(false), stop_requested_(false), decode_finished_(false), encode_failed_(false),
	  use_d3d11_hw_encode_(false),
	  processed_frames_(0), total_frames_(0), detected_objects_(0), elapsed_seconds_(0.0),
	  estimated_total_frames_(0),
	  copyright_width_(0), copyright_height_(0) {
}

VideoPipeline::~VideoPipeline() {
	Cleanup();
}

bool VideoPipeline::Initialize(const PipelineConfig& config) {
	config_ = config;

	// 1. D3D11デバイス初期化
	D3D_DRIVER_TYPE device_type = D3D_DRIVER_TYPE_HARDWARE;
	PipelineLog("[VideoPipeline::Initialize] Step 1: D3D11 device init\n");
	if (!device_manager_.Initialize(device_type)) {
		PipelineLog("[VideoPipeline::Initialize] FAILED: D3D11 device init\n");
		return false;
	}
	PipelineLog("[VideoPipeline::Initialize] D3D11 device OK\n");

	// 2. GPU判別とエンコーダー自動選択
	GpuVendor gpu_vendor = device_manager_.GetGpuVendor();
	HwEncoderType encoder_type = config.encoder_type;

	if (encoder_type == HwEncoderType::Auto) {
		encoder_type = device_manager_.AutoDetectEncoder();
	}

	std::cout << "GPU: " << device_manager_.GetGpuName() << std::endl;
	// Encoder は AutoDetectEncoder() の結果で判別
	std::cout << "Encoder: Auto-detected" << std::endl;

	// 3. WinML初期化
	PipelineLog("[VideoPipeline::Initialize] Step 2: WinML init\n");
	winml_utils_.SetD3D11Device(device_manager_.GetDevice(), device_manager_.GetContext());
	winml_utils_.SetGpuVendor(device_manager_.GetGpuVendor());
	if (!winml_utils_.Initialize()) {
		PipelineLog("[VideoPipeline::Initialize] FAILED: WinML init\n");
		return false;
	}
	PipelineLog("[VideoPipeline::Initialize] WinML OK\n");

	// 4. ONNXモデルロード
	if (config_.model_path) {
		PipelineLog("[VideoPipeline::Initialize] Step 3: Load ONNX model\n");
		if (!winml_utils_.LoadModel(config_.model_path)) {
			PipelineLog("[VideoPipeline::Initialize] FAILED: Load ONNX model\n");
			return false;
		}
		PipelineLog("[VideoPipeline::Initialize] ONNX model OK\n");

		// モデルの入力形状に合わせてconfigを更新
		auto input_names = winml_utils_.GetInputNames();
		if (!input_names.empty()) {
			auto shape = winml_utils_.GetInputShape(input_names[0]);
			if (shape.size() == 4 && shape[2] > 0 && shape[3] > 0) {
				config_.input_width = static_cast<int>(shape[3]);
				config_.input_height = static_cast<int>(shape[2]);
				PipelineLogFmt("[VideoPipeline::Initialize] Model input shape updated to %dx%d\n",
					config_.input_width, config_.input_height);
			}
		}
	}

	if (config_.exclude_by_name_enabled) {
		PipelineLog("[VideoPipeline::Initialize] Step 3.5: Init ByteTrack + OCR\n");
		const int tracker_fps = std::max(1, config_.fps);
		tracker_ = std::make_unique<ByteTrackInterop::Tracker>(
			tracker_fps,
			30,
			std::clamp(config_.conf_threshold, 0.01f, 0.95f));

		ocr_text_by_track_id_.clear();
		ocr_last_frame_by_track_.clear();
		ocr_pending_track_ids_.clear();
		mask_exclude_texts_.clear();
		inference_frame_counter_ = 0;
		ocr_refresh_interval_frames_ = std::max(1, config_.fps * 3);
		ocr_stop_requested_ = false;
		{
			std::lock_guard<std::mutex> lock(ocr_mutex_);
			while (!ocr_queue_.empty()) {
				ocr_queue_.pop();
			}
		}

		if (config_.mask_exclude_text_csv) {
			mask_exclude_texts_ = TextMatch::SplitCsv(TextMatch::ToUtf8(config_.mask_exclude_text_csv));
		}
		PipelineLogFmt("[ExcludeByName][Init] enabled=1 conf=%.3f ocr_expand=%d ocr_max_rois=%d sim=%.2f csv_count=%zu\n",
			config_.conf_threshold,
			config_.ocr_expand_pixels,
			config_.ocr_max_rois_per_frame,
			config_.text_similarity_threshold,
			mask_exclude_texts_.size());

		ocr_recognizer_.SetD3D11Device(device_manager_.GetDevice(), device_manager_.GetContext());
		ocr_recognizer_.SetGpuVendor(device_manager_.GetGpuVendor());

		const bool dict_loaded = ocr_recognizer_.LoadDictionary(config_.ocr_dict_path);
		const bool model_loaded = ocr_recognizer_.LoadModel(config_.ocr_model_path, true);
		PipelineLogFmt("[ExcludeByName][Init] dict_loaded=%d model_loaded=%d\n", dict_loaded ? 1 : 0, model_loaded ? 1 : 0);
		if (!dict_loaded || !model_loaded) {
			PipelineLog("[VideoPipeline::Initialize] WARN: OCR init failed, disable exclude_by_name\n");
			config_.exclude_by_name_enabled = false;
			tracker_.reset();
		} else {
			PipelineLog("[VideoPipeline::Initialize] ByteTrack + OCR ready\n");
		}
	}

	// 6. マスクシェーダー初期化
	mosaic_size_ = config_.blackedout_param > 0 ? config_.blackedout_param : 3;
	blur_radius_ = config_.blackedout_param > 0 ? config_.blackedout_param : 3;
	PipelineLog("[VideoPipeline::Initialize] Step 4: Mask shader init\n");
	if (!mask_shader_.Initialize(device_manager_.GetDevice())) {
		PipelineLog("[VideoPipeline::Initialize] FAILED: Mask shader init\n");
		return false;
	}
	PipelineLog("[VideoPipeline::Initialize] Mask shader OK\n");

	if (config_.enable_copyright) {
		if (EnsureCopyrightWatermarkLoaded()) {
			PipelineLog("[VideoPipeline::Initialize] Copyright watermark loaded\n");
		} else {
			PipelineLog("[VideoPipeline::Initialize] WARN: Copyright watermark not available\n");
		}
	}

	// 7. FFmpeg初期化 (FFmpeg 5.0+ では av_register_all() は不要)
	avformat_network_init();

	// 入力ファイルオープン
	input_format_ctx_ = avformat_alloc_context();
	if (!input_format_ctx_) {
		std::cerr << "Failed to allocate input format context." << std::endl;
		return false;
	}

	std::string input_path_utf8;
	const char* input_path_c = nullptr;
	if (config_.input_path) {
		int len = WideCharToMultiByte(CP_UTF8, 0, config_.input_path, -1, nullptr, 0, nullptr, nullptr);
		if (len > 0) {
			input_path_utf8.resize(len - 1);
			WideCharToMultiByte(CP_UTF8, 0, config_.input_path, -1, &input_path_utf8[0], len, nullptr, nullptr);
			input_path_c = input_path_utf8.c_str();
		}
	}
	PipelineLogFmt("[VideoPipeline::Initialize] Step 5: Open input file: %s\n", input_path_c ? input_path_c : "(null)");
	if (avformat_open_input(&input_format_ctx_, 
							input_path_c, 
							nullptr, nullptr) < 0) {
		PipelineLog("[VideoPipeline::Initialize] FAILED: Open input file\n");
		return false;
	}
	PipelineLog("[VideoPipeline::Initialize] Input file opened OK\n");

	PipelineLog("[VideoPipeline::Initialize] Step 6: Find stream info\n");
	if (avformat_find_stream_info(input_format_ctx_, nullptr) < 0) {
		PipelineLog("[VideoPipeline::Initialize] FAILED: Find stream info\n");
		return false;
	}
	PipelineLogFmt("[VideoPipeline::Initialize] Stream info OK, streams=%u\n", input_format_ctx_->nb_streams);

	// ビデオストリームとオーディオストリームを検索
	video_stream_index_ = -1;
	audio_stream_index_ = -1;
	input_audio_stream_ = nullptr;

	for (unsigned int i = 0; i < input_format_ctx_->nb_streams; i++) {
		AVStream* stream = input_format_ctx_->streams[i];
		if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_index_ == -1) {
			video_stream_index_ = static_cast<int>(i);
		}
		else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_index_ == -1) {
			audio_stream_index_ = static_cast<int>(i);
			input_audio_stream_ = stream;
		}
	}

	if (config_.disable_audio) {
		audio_stream_index_ = -1;
		input_audio_stream_ = nullptr;
		PipelineLog("[VideoPipeline::Initialize] Audio disabled by option\n");
	}

	if (video_stream_index_ == -1) {
		PipelineLog("[VideoPipeline::Initialize] FAILED: No video stream found\n");
		return false;
	}
	PipelineLogFmt("[VideoPipeline::Initialize] Video stream index=%d audio_stream_index=%d\n", video_stream_index_, audio_stream_index_);

	// 総フレーム数を推定（進捗表示用）
	{
		AVStream* video_stream = input_format_ctx_->streams[video_stream_index_];
		int output_fps = config_.for_x ? 30 : config_.fps;
		int estimated = 0;
		if (video_stream->nb_frames > 0) {
			estimated = static_cast<int>(video_stream->nb_frames);
		} else if (video_stream->duration > 0 && video_stream->time_base.den > 0) {
			double duration_sec = video_stream->duration * av_q2d(video_stream->time_base);
			estimated = static_cast<int>(duration_sec * output_fps);
		} else if (input_format_ctx_->duration > 0) {
			double duration_sec = input_format_ctx_->duration / (double)AV_TIME_BASE;
			estimated = static_cast<int>(duration_sec * output_fps);
		}
		if (estimated <= 0) {
			// フォールバック: コーデックパラメータから推定
			estimated = static_cast<int>(input_format_ctx_->bit_rate > 0 ?
				(input_format_ctx_->duration / (double)AV_TIME_BASE) * output_fps : 0);
		}
		estimated_total_frames_ = estimated;
		PipelineLogFmt("[VideoPipeline::Initialize] Estimated total frames: %d\n", estimated);
	}

	// デコーダー初期化
	AVCodecParameters* codec_params = input_format_ctx_->streams[video_stream_index_]->codecpar;
	const AVCodec* decoder_codec = nullptr;

	// HWデコーダーを優先して検索。D3D11VAは通常のデコーダにhw_device_ctxを設定して有効化。
	// D3D11VAが機能しない場合は、GPUベンダー固有のデコーダにフォールバック。
	switch (codec_params->codec_id) {
		case AV_CODEC_ID_HEVC:
			decoder_codec = avcodec_find_decoder_by_name("hevc");
			if (!decoder_codec && gpu_vendor == GpuVendor::NVIDIA) decoder_codec = avcodec_find_decoder_by_name("hevc_cuvid");
			if (!decoder_codec && gpu_vendor == GpuVendor::Intel) decoder_codec = avcodec_find_decoder_by_name("hevc_qsv");
			if (!decoder_codec && gpu_vendor == GpuVendor::AMD) decoder_codec = avcodec_find_decoder_by_name("hevc_amf");
			break;
		case AV_CODEC_ID_H264:
			decoder_codec = avcodec_find_decoder_by_name("h264");
			if (!decoder_codec && gpu_vendor == GpuVendor::NVIDIA) decoder_codec = avcodec_find_decoder_by_name("h264_cuvid");
			if (!decoder_codec && gpu_vendor == GpuVendor::Intel) decoder_codec = avcodec_find_decoder_by_name("h264_qsv");
			if (!decoder_codec && gpu_vendor == GpuVendor::AMD) decoder_codec = avcodec_find_decoder_by_name("h264_amf");
			break;
		case AV_CODEC_ID_AV1:
			decoder_codec = avcodec_find_decoder_by_name("av1");
			if (!decoder_codec && gpu_vendor == GpuVendor::NVIDIA) decoder_codec = avcodec_find_decoder_by_name("av1_cuvid");
			if (!decoder_codec && gpu_vendor == GpuVendor::Intel) decoder_codec = avcodec_find_decoder_by_name("av1_qsv");
			if (!decoder_codec && gpu_vendor == GpuVendor::AMD) decoder_codec = avcodec_find_decoder_by_name("av1_amf");
			break;
		case AV_CODEC_ID_VP9:
			decoder_codec = avcodec_find_decoder_by_name("vp9");
			if (!decoder_codec && gpu_vendor == GpuVendor::NVIDIA) decoder_codec = avcodec_find_decoder_by_name("vp9_cuvid");
			if (!decoder_codec && gpu_vendor == GpuVendor::Intel) decoder_codec = avcodec_find_decoder_by_name("vp9_qsv");
			if (!decoder_codec && gpu_vendor == GpuVendor::AMD) decoder_codec = avcodec_find_decoder_by_name("vp9_amf");
			break;
		case AV_CODEC_ID_VP8:
			decoder_codec = avcodec_find_decoder_by_name("vp8");
			if (!decoder_codec && gpu_vendor == GpuVendor::NVIDIA) decoder_codec = avcodec_find_decoder_by_name("vp8_cuvid");
			if (!decoder_codec && gpu_vendor == GpuVendor::Intel) decoder_codec = avcodec_find_decoder_by_name("vp8_qsv");
			break;
	}
	if (!decoder_codec) {
		decoder_codec = avcodec_find_decoder(codec_params->codec_id);
		PipelineLog("[VideoPipeline::Initialize] HW decoder not found, falling back to SW decoder\n");
	} else {
		const char* vendor_name = (gpu_vendor == GpuVendor::NVIDIA) ? "NVIDIA" :
			(gpu_vendor == GpuVendor::Intel) ? "Intel" :
			(gpu_vendor == GpuVendor::AMD) ? "AMD" : "Unknown";
		PipelineLogFmt("[VideoPipeline::Initialize] HW decoder found (vendor=%s)\n", vendor_name);
	}
	if (!decoder_codec) {
		std::cerr << "No decoder found." << std::endl;
		return false;
	}

	decoder_ctx_ = avcodec_alloc_context3(decoder_codec);
	if (!decoder_ctx_) {
		std::cerr << "Failed to allocate decoder context." << std::endl;
		return false;
	}

	// HWデコード用コンテキストを設定
	// device_manager_ 所有の参照をそのまま保持すると Cleanup 時に二重 unref になるため、
	// ここで専用参照を取得して管理する。
	hw_device_ctx_ = av_buffer_ref(device_manager_.GetHWDeviceContext());
	if (hw_device_ctx_) {
		decoder_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
	}

	// HWデコードを優先する get_format コールバック
	decoder_ctx_->get_format = [](AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) -> enum AVPixelFormat {
		const enum AVPixelFormat* p;
		// 優先順位: CUDA > QSV > AMF(D3D11VA) > 最初のサポート形式
		for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
			if (*p == AV_PIX_FMT_CUDA) {
				OutputDebugStringA("[get_format] Selected AV_PIX_FMT_CUDA\n");
				return AV_PIX_FMT_CUDA;
			}
		}
		for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
			if (*p == AV_PIX_FMT_QSV) {
				OutputDebugStringA("[get_format] Selected AV_PIX_FMT_QSV\n");
				return AV_PIX_FMT_QSV;
			}
		}
		for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
			if (*p == AV_PIX_FMT_D3D11) {
				OutputDebugStringA("[get_format] Selected AV_PIX_FMT_D3D11\n");
				return AV_PIX_FMT_D3D11;
			}
		}
		OutputDebugStringA("[get_format] HW format not available, using first supported format\n");
		return pix_fmts[0];
	};

	decoder_ctx_->codec_type = AVMEDIA_TYPE_VIDEO;

	if (avcodec_parameters_to_context(decoder_ctx_, codec_params) < 0) {
		std::cerr << "Failed to copy codec parameters to decoder context." << std::endl;
		return false;
	}

	if (avcodec_open2(decoder_ctx_, decoder_codec, nullptr) < 0) {
		PipelineLog("[VideoPipeline::Initialize] FAILED: Open decoder\n");
		return false;
	}
	PipelineLog("[VideoPipeline::Initialize] Decoder opened OK\n");

	// エンコーダー選択（HEVC対応）
	const AVCodec* encoder_codec = nullptr;
	enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_D3D11VA;

	// UIでfor_xがtrueの場合はH.264エンコーダを使用、それ以外はHEVCを基本とする
	bool use_h264 = config_.for_x;

	if (use_h264) {
		switch (encoder_type) {
			case HwEncoderType::NVENC:
				encoder_codec = avcodec_find_encoder_by_name("h264_nvenc");
				hw_type = AV_HWDEVICE_TYPE_D3D11VA;
				break;
			case HwEncoderType::QSV:
				encoder_codec = avcodec_find_encoder_by_name("h264_qsv");
				hw_type = AV_HWDEVICE_TYPE_QSV;
				break;
			case HwEncoderType::AMF:
				encoder_codec = avcodec_find_encoder_by_name("h264_amf");
				hw_type = AV_HWDEVICE_TYPE_VAAPI;
				break;
			default:
				encoder_codec = avcodec_find_encoder_by_name("h264_nvenc");
				hw_type = AV_HWDEVICE_TYPE_D3D11VA;
				break;
		}
		PipelineLog("[VideoPipeline::Initialize] H.264 encoder selected (for_x=true)\n");
	} else {
		switch (encoder_type) {
			case HwEncoderType::NVENC:
				encoder_codec = avcodec_find_encoder_by_name("hevc_nvenc");
				hw_type = AV_HWDEVICE_TYPE_D3D11VA;
				break;
			case HwEncoderType::QSV:
				encoder_codec = avcodec_find_encoder_by_name("hevc_qsv");
				hw_type = AV_HWDEVICE_TYPE_QSV;
				break;
			case HwEncoderType::AMF:
				encoder_codec = avcodec_find_encoder_by_name("hevc_amf");
				hw_type = AV_HWDEVICE_TYPE_VAAPI;
				break;
			default:
				encoder_codec = avcodec_find_encoder_by_name("hevc_nvenc");
				hw_type = AV_HWDEVICE_TYPE_D3D11VA;
				break;
		}
		PipelineLog("[VideoPipeline::Initialize] HEVC encoder selected (default)\n");
	}

	if (!encoder_codec) {
		std::cerr << "No hardware encoder found. Falling back to software encoder." << std::endl;
		encoder_codec = avcodec_find_encoder(use_h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
	}

	// 出力ファイル作成（出力フォーマットを自動推定）
	std::string output_path_utf8;
	if (config_.output_path) {
		int len = WideCharToMultiByte(CP_UTF8, 0, config_.output_path, -1, nullptr, 0, nullptr, nullptr);
		if (len > 0) {
			output_path_utf8.resize(len - 1);
			WideCharToMultiByte(CP_UTF8, 0, config_.output_path, -1, &output_path_utf8[0], len, nullptr, nullptr);
		}
	}

	PipelineLogFmt("[VideoPipeline::Initialize] Step 7: Allocate output context: %s\n", output_path_utf8.c_str());
	if (avformat_alloc_output_context2(&output_format_ctx_, nullptr, nullptr, output_path_utf8.c_str()) < 0 || !output_format_ctx_) {
		PipelineLog("[VideoPipeline::Initialize] FAILED: Allocate output context\n");
		return false;
	}
	PipelineLogFmt("[VideoPipeline::Initialize] Output context OK, format=%s\n", output_format_ctx_->oformat ? output_format_ctx_->oformat->name : "(null)");

	// 出力ストリーム作成
	AVStream* output_video_stream = avformat_new_stream(output_format_ctx_, nullptr);
	if (!output_video_stream) {
		std::cerr << "Failed to create output video stream." << std::endl;
		return false;
	}

	output_video_stream_index_ = static_cast<int>(output_video_stream->index);

	encoder_ctx_ = avcodec_alloc_context3(encoder_codec);
	if (!encoder_ctx_) {
		std::cerr << "Failed to allocate encoder context." << std::endl;
		return false;
	}

	// エンコーダー設定（ForX時は横1280px・30fpsに固定）
	int output_fps = config_.for_x ? 30 : config_.fps;
	int output_width = decoder_ctx_->width;
	int output_height = decoder_ctx_->height;
	if (config_.for_x) {
		output_width = 1280;
		if (decoder_ctx_->width > 0 && decoder_ctx_->height > 0) {
			output_height = static_cast<int>(std::round(decoder_ctx_->height * 1280.0 / std::max(1, decoder_ctx_->width)));
			if (output_height % 2 != 0) {
				output_height += 1;
			}
			if (output_height < 2) {
				output_height = 2;
			}
		} else {
			output_height = 720;
		}
		PipelineLog("[VideoPipeline::Initialize] ForX output override: 1280px wide, 30fps\n");
	}
	encoder_ctx_->width = output_width;
	encoder_ctx_->height = output_height;
	encoder_ctx_->bit_rate = config_.bitrate;
	encoder_ctx_->framerate = { output_fps, 1 };
	encoder_ctx_->time_base = { 1, output_fps };
	encoder_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	// Phase 3A: NVENC / AMF は D3D11 HWフレーム（GPUゼロコピー）を使用
	// QSV は AV_PIX_FMT_QSV 前提のため、現状は SWフレーム パスを維持
	use_d3d11_hw_encode_ = (encoder_type == HwEncoderType::NVENC || encoder_type == HwEncoderType::AMF);
	if (use_d3d11_hw_encode_) {
		encoder_ctx_->pix_fmt = AV_PIX_FMT_D3D11;
		encoder_ctx_->hw_device_ctx = av_buffer_ref(device_manager_.GetHWDeviceContext());
		hw_frames_ctx_ = device_manager_.CreateEncoderFramesContext(encoder_type, encoder_ctx_->width, encoder_ctx_->height);
		if (!hw_frames_ctx_) {
			PipelineLog("[VideoPipeline::Initialize] WARNING: Failed to create D3D11 hw_frames_ctx, falling back to SW\n");
			use_d3d11_hw_encode_ = false;
			encoder_ctx_->pix_fmt = AV_PIX_FMT_NV12;
			av_buffer_unref(&encoder_ctx_->hw_device_ctx);
		} else {
			encoder_ctx_->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);
			PipelineLog("[VideoPipeline::Initialize] Using D3D11 HW frame encoding path (zero-copy)\n");
		}
	} else {
		encoder_ctx_->pix_fmt = AV_PIX_FMT_NV12;
		if (encoder_type == HwEncoderType::QSV) {
			PipelineLog("[VideoPipeline::Initialize] QSV zero-copy is disabled in this path (requires AV_PIX_FMT_QSV / QSV frames context)\n");
		}
		PipelineLog("[VideoPipeline::Initialize] Using SW frame (NV12) encoding path\n");
	}

	// プリセット設定（FFmpegエンコーダーがサポートする場合）
	AVDictionary* opts = nullptr;
	if (encoder_type == HwEncoderType::NVENC) {
		av_dict_set(&opts, "preset", "slow", 0);
		av_dict_set(&opts, "rc", "vbr", 0);
	} else if (encoder_type == HwEncoderType::QSV) {
		av_dict_set(&opts, "preset", "slow", 0);
	} else if (encoder_type == HwEncoderType::AMF) {
		av_dict_set(&opts, "quality", "quality", 0);
	}

	PipelineLog("[VideoPipeline::Initialize] Step 8: Open encoder\n");
	if (avcodec_open2(encoder_ctx_, encoder_codec, &opts) < 0) {
		PipelineLog("[VideoPipeline::Initialize] FAILED: Open encoder\n");
		av_dict_free(&opts);
		return false;
	}
	av_dict_free(&opts);
	PipelineLog("[VideoPipeline::Initialize] Encoder opened OK\n");

	// エンコーダー設定をストリームの codecpar にコピー
	if (avcodec_parameters_from_context(output_video_stream->codecpar, encoder_ctx_) < 0) {
		PipelineLog("[VideoPipeline::Initialize] FAILED: Copy encoder parameters\n");
		return false;
	}
	output_video_stream->time_base = encoder_ctx_->time_base;
	output_video_stream->avg_frame_rate = encoder_ctx_->framerate;
	output_video_stream->r_frame_rate = encoder_ctx_->framerate;
	PipelineLog("[VideoPipeline::Initialize] Encoder parameters copied OK\n");

	// オーディオストリームがある場合はコピー
	if (!config_.disable_audio && audio_stream_index_ >= 0) {
		AVStream* output_audio_stream = avformat_new_stream(output_format_ctx_, nullptr);
		if (output_audio_stream) {
			output_audio_stream_index_ = static_cast<int>(output_audio_stream->index);
			AVCodecParameters* audio_codec_params = output_audio_stream->codecpar;
			AVCodecParameters* input_audio_params = input_format_ctx_->streams[audio_stream_index_]->codecpar;
			avcodec_parameters_copy(audio_codec_params, input_audio_params);
			audio_codec_params->codec_tag = 0;
		}
	}

	// 出力ファイルオープン
	PipelineLog("[VideoPipeline::Initialize] Step 9: Open output file\n");
	if (avio_open(&output_format_ctx_->pb, output_path_utf8.c_str(), AVIO_FLAG_WRITE) < 0) {
		PipelineLog("[VideoPipeline::Initialize] FAILED: Open output file\n");
		return false;
	}
	PipelineLog("[VideoPipeline::Initialize] Output file opened OK\n");

	// マルチメディアファイル書き出しヘッダー
	PipelineLog("[VideoPipeline::Initialize] Step 10: Write header\n");
	if (avformat_write_header(output_format_ctx_, nullptr) < 0) {
		PipelineLog("[VideoPipeline::Initialize] FAILED: Write output header\n");
		return false;
	}

	PipelineLog("[VideoPipeline::Initialize] Pipeline initialized successfully\n");
	return true;
}

PipelineResult VideoPipeline::Run() {
	PipelineResult result{};
	result.success = false;

	PipelineLog("[VideoPipeline::Run] Starting pipeline threads\n");
	auto start_time = std::chrono::high_resolution_clock::now();

	running_ = true;
	stop_requested_ = false;
	ocr_stop_requested_ = false;
	decode_finished_ = false;
	encode_failed_ = false;
	decode_stage_ms_.Reset();
	inference_stage_ms_.Reset();
	ocr_stage_ms_.Reset();
	encode_stage_ms_.Reset();
	decode_queue_wait_ms_.Reset();
	encode_queue_wait_ms_.Reset();

	// 3スレッド開始
	decode_thread_ = std::thread(&VideoPipeline::DecodeThread, this);
	inference_thread_ = std::thread(&VideoPipeline::InferenceThread, this);
	encode_thread_ = std::thread(&VideoPipeline::EncodeThread, this);
	if (config_.exclude_by_name_enabled && ocr_recognizer_.IsReady()) {
		ocr_thread_ = std::thread(&VideoPipeline::OcrThread, this);
	}
	PipelineLog("[VideoPipeline::Run] Threads started, waiting for completion\n");

	// スレッドの終了を待機
	decode_thread_.join();
	PipelineLog("[VideoPipeline::Run] Decode thread joined\n");
	inference_thread_.join();
	PipelineLog("[VideoPipeline::Run] Inference thread joined\n");
	size_t dropped_ocr_jobs = 0;
	size_t dropped_ocr_rois = 0;
	{
		std::lock_guard<std::mutex> lock(ocr_mutex_);
		while (!ocr_queue_.empty()) {
			dropped_ocr_rois += ocr_queue_.front().rois.size();
			ocr_queue_.pop();
			dropped_ocr_jobs++;
		}
	}
	if (dropped_ocr_jobs > 0) {
		std::lock_guard<std::mutex> lock(ocr_result_mutex_);
		ocr_pending_track_ids_.clear();
	}
	PipelineLogFmt("[VideoPipeline::Run] Dropped OCR queue jobs=%zu rois=%zu before shutdown\n", dropped_ocr_jobs, dropped_ocr_rois);
	ocr_stop_requested_ = true;
	ocr_cv_.notify_all();
	if (ocr_thread_.joinable()) {
		ocr_thread_.join();
		PipelineLog("[VideoPipeline::Run] OCR thread joined\n");
	}
	encode_thread_.join();
	PipelineLog("[VideoPipeline::Run] Encode thread joined\n");

	auto end_time = std::chrono::high_resolution_clock::now();
	elapsed_seconds_ = std::chrono::duration<double>(end_time - start_time).count();

	result.success = !encode_failed_.load() && processed_frames_.load() > 0;
	result.processed_frames = processed_frames_.load();
	result.total_frames = estimated_total_frames_.load();
	result.detected_objects = detected_objects_.load();
	result.elapsed_seconds = elapsed_seconds_.load();
	const double pipeline_fps = (result.elapsed_seconds > 0.0)
		? (static_cast<double>(result.processed_frames) / result.elapsed_seconds)
		: 0.0;
	PipelineLogFmt(
		"[PipelineTiming][Summary] fps=%.2f decode(avg=%.2fms,max=%.2fms,n=%llu) infer(avg=%.2fms,max=%.2fms,n=%llu) ocr(avg=%.2fms,max=%.2fms,n=%llu) encode(avg=%.2fms,max=%.2fms,n=%llu) q_decode_wait(avg=%.2fms,max=%.2fms,n=%llu) q_encode_wait(avg=%.2fms,max=%.2fms,n=%llu)\n",
		pipeline_fps,
		decode_stage_ms_.AvgMs(), decode_stage_ms_.max_ms, static_cast<unsigned long long>(decode_stage_ms_.count),
		inference_stage_ms_.AvgMs(), inference_stage_ms_.max_ms, static_cast<unsigned long long>(inference_stage_ms_.count),
		ocr_stage_ms_.AvgMs(), ocr_stage_ms_.max_ms, static_cast<unsigned long long>(ocr_stage_ms_.count),
		encode_stage_ms_.AvgMs(), encode_stage_ms_.max_ms, static_cast<unsigned long long>(encode_stage_ms_.count),
		decode_queue_wait_ms_.AvgMs(), decode_queue_wait_ms_.max_ms, static_cast<unsigned long long>(decode_queue_wait_ms_.count),
		encode_queue_wait_ms_.AvgMs(), encode_queue_wait_ms_.max_ms, static_cast<unsigned long long>(encode_queue_wait_ms_.count));

	PipelineLogFmt("[VideoPipeline::Run] Pipeline completed. Processed: %d frames in %.2f seconds\n", result.processed_frames, result.elapsed_seconds);

	return result;
}

void VideoPipeline::Stop() {
	stop_requested_ = true;
	ocr_stop_requested_ = true;
	video_cv_.notify_all();
	audio_cv_.notify_all();
	ocr_cv_.notify_all();
}

void VideoPipeline::Cleanup() {
	try {
		running_ = false;
		stop_requested_ = true;
		ocr_stop_requested_ = true;
		video_cv_.notify_all();
		audio_cv_.notify_all();
		ocr_cv_.notify_all();

		if (decode_thread_.joinable()) decode_thread_.join();
		if (inference_thread_.joinable()) inference_thread_.join();
		if (ocr_thread_.joinable()) ocr_thread_.join();
		if (encode_thread_.joinable()) encode_thread_.join();

		// FFmpegリソース解放（順序重要）
		if (output_format_ctx_) {
			if (output_format_ctx_->pb) avio_closep(&output_format_ctx_->pb);
			avformat_free_context(output_format_ctx_);
		}

		if (input_format_ctx_) avformat_close_input(&input_format_ctx_);

		// decoder_ctx_->hw_device_ctx をクリアしてから解放
		if (decoder_ctx_) {
			av_buffer_unref(&decoder_ctx_->hw_device_ctx);
			avcodec_free_context(&decoder_ctx_);
		}
		if (encoder_ctx_) {
			av_buffer_unref(&encoder_ctx_->hw_frames_ctx);
			avcodec_free_context(&encoder_ctx_);
		}
		if (audio_encoder_ctx_) avcodec_free_context(&audio_encoder_ctx_);

		if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
		if (hw_frames_ctx_) av_buffer_unref(&hw_frames_ctx_);

		// マスクシェーダー解放
		mask_shader_.Release();
		copyright_srv_.Reset();
		copyright_texture_.Reset();
		copyright_width_ = 0;
		copyright_height_ = 0;
		tracker_.reset();
		{
			std::lock_guard<std::mutex> lock(ocr_result_mutex_);
			ocr_text_by_track_id_.clear();
			ocr_last_frame_by_track_.clear();
			ocr_pending_track_ids_.clear();
		}
		{
			std::lock_guard<std::mutex> lock(ocr_mutex_);
			while (!ocr_queue_.empty()) {
				ocr_queue_.pop();
			}
		}
		mask_exclude_texts_.clear();
		inference_frame_counter_ = 0;

		// キュー解放
		std::lock_guard<std::mutex> lock(audio_mutex_);
		while (!audio_queue_.empty()) {
			AVPacket* pkt = audio_queue_.front();
			audio_queue_.pop();
			av_packet_free(&pkt);
		}

		processed_frames_ = 0;
		total_frames_ = 0;
		detected_objects_ = 0;
		elapsed_seconds_ = 0.0;
		estimated_total_frames_ = 0;
	}
	catch (const winrt::hresult_error& ex) {
		std::string msg = winrt::to_string(ex.message());
		PipelineLogFmt("[VideoPipeline] Cleanup WinRT exception: %s\n", msg.c_str());
	}
	catch (const std::exception& ex) {
		PipelineLogFmt("[VideoPipeline] Cleanup exception: %s\n", ex.what());
	}
}

void VideoPipeline::DecodeThread() {
	PipelineLog("[DecodeThread] Started\n");
	AVFrame* frame = av_frame_alloc();
	AVPacket* packet = av_packet_alloc();
	AVStream* video_stream = (video_stream_index_ >= 0 && input_format_ctx_) ? input_format_ctx_->streams[video_stream_index_] : nullptr;
	double trim_start = std::max(0.0, config_.trim_start_seconds);
	double trim_end = config_.trim_end_seconds;

	while (!stop_requested_.load()) {
		// パケットを読み込む
		int ret = av_read_frame(input_format_ctx_, packet);
		if (ret < 0) {
			// 読み込み終了
			break;
		}

		// オーディオストリームの場合は音声キューに追加
		if (audio_stream_index_ >= 0 && packet->stream_index == audio_stream_index_) {
			AVStream* audio_stream = input_format_ctx_->streams[audio_stream_index_];
			if (audio_stream && (trim_start > 0.0 || trim_end > 0.0)) {
				int64_t pkt_ts = (packet->pts != AV_NOPTS_VALUE) ? packet->pts : packet->dts;
				if (pkt_ts != AV_NOPTS_VALUE) {
					double packet_sec = pkt_ts * av_q2d(audio_stream->time_base);
					if (trim_end > trim_start && packet_sec > trim_end) {
						av_packet_unref(packet);
						break;
					}
					if (packet_sec < trim_start) {
						av_packet_unref(packet);
						continue;
					}
				}
			}

			// 音声パケットをコピーしてキューに追加
			AVPacket* audio_pkt = av_packet_alloc();
			av_packet_ref(audio_pkt, packet);
			{
				std::lock_guard<std::mutex> lock(audio_mutex_);
				audio_queue_.push(audio_pkt);
			}
			audio_cv_.notify_one();
		}
		// ビデオストリームのみの処理
		else if (packet->stream_index == video_stream_index_) {
			if (video_stream && (trim_start > 0.0 || trim_end > 0.0)) {
				int64_t pkt_ts = (packet->pts != AV_NOPTS_VALUE) ? packet->pts : packet->dts;
				if (pkt_ts != AV_NOPTS_VALUE) {
					double packet_sec = pkt_ts * av_q2d(video_stream->time_base);
					if (trim_end > trim_start && packet_sec > trim_end) {
						av_packet_unref(packet);
						break;
					}
					if (packet_sec < trim_start) {
						av_packet_unref(packet);
						continue;
					}
				}
			}

			// デコーダーにパケットを送信
			ret = avcodec_send_packet(decoder_ctx_, packet);
			if (ret >= 0) {
				// フレームを受信
				ret = avcodec_receive_frame(decoder_ctx_, frame);
				if (ret == 0 && frame->width > 0 && frame->height > 0) {
					if (video_stream && (trim_start > 0.0 || trim_end > 0.0)) {
						int64_t frame_ts = frame->best_effort_timestamp;
						if (frame_ts == AV_NOPTS_VALUE) {
							frame_ts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts : frame->pkt_dts;
						}
						if (frame_ts != AV_NOPTS_VALUE) {
							double frame_sec = frame_ts * av_q2d(video_stream->time_base);
							if (trim_end > trim_start && frame_sec > trim_end) {
								break;
							}
							if (frame_sec < trim_start) {
								av_frame_unref(frame);
								continue;
							}
						}
					}

					auto decode_start = std::chrono::high_resolution_clock::now();

					// D3D11テクスチャを保持するGpuFrameを作成
					GpuFrame gpu_frame;
					gpu_frame.texture = Microsoft::WRL::ComPtr<ID3D11Texture2D>();
					
					// D3D11フレームからテクスチャを取得
					PipelineLogFmt("[DecodeThread] Frame format=%d (D3D11=%d), data[0]=%p\n", frame->format, AV_PIX_FMT_D3D11, frame->data[0]);
					if (frame->format == AV_PIX_FMT_D3D11 && frame->data[0]) {
						// D3D11VA HWデコードフレームをそのままGPUでBGRA変換（CPU往復を除去）
						PipelineLog("[DecodeThread] D3D11 HW frame detected, converting NV12 to BGRA (zero-copy path)\n");
						ID3D11Texture2D* nv12_tex = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
						UINT nv12_array_slice = static_cast<UINT>(reinterpret_cast<ULONG_PTR>(frame->data[1]));

						if (nv12_tex) {
							D3D11_TEXTURE2D_DESC bgra_desc = {};
							bgra_desc.Width = frame->width;
							bgra_desc.Height = frame->height;
							bgra_desc.MipLevels = 1;
							bgra_desc.ArraySize = 1;
							bgra_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
							bgra_desc.SampleDesc.Count = 1;
							bgra_desc.Usage = D3D11_USAGE_DEFAULT;
							bgra_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

							Microsoft::WRL::ComPtr<ID3D11Texture2D> bgra_tex;
							HRESULT hr = device_manager_.GetDevice()->CreateTexture2D(&bgra_desc, nullptr, bgra_tex.ReleaseAndGetAddressOf());
							if (SUCCEEDED(hr) && bgra_tex.Get() && device_manager_.ConvertNV12ToBGRA(nv12_tex, bgra_tex.Get(), nv12_array_slice)) {
								gpu_frame.texture = bgra_tex;
								auto decode_end = std::chrono::high_resolution_clock::now();
								auto decode_ms = std::chrono::duration_cast<std::chrono::milliseconds>(decode_end - decode_start).count();
								decode_stage_ms_.Add(static_cast<double>(decode_ms));
								PipelineLogFmt("[DecodeThread] D3D11 HW frame converted to BGRA GPU texture, time=%lld ms\n", decode_ms);
							}
						}
					if (!gpu_frame.texture.Get()) {
						PipelineLog("[DecodeThread] WARNING: Failed to convert D3D11 HW frame, skipping\n");
					}
				} else {
					PipelineLog("[DecodeThread] SW frame detected, uploading to GPU\n");
					AVFrame* sw_frame = av_frame_alloc();
					if (sw_frame) {
						AVFrame* target = frame;
						// HWフレームならSWへ転送
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
						// BGRA変換してテクスチャ作成
						struct SwsContext* sws = sws_getContext(
							frame->width, frame->height, (AVPixelFormat)target->format,
							frame->width, frame->height, AV_PIX_FMT_BGRA,
							SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
						if (sws) {
							std::vector<uint8_t> pixels(frame->width * frame->height * 4);
							uint8_t* dst_data[1] = { pixels.data() };
							int dst_linesize[1] = { (int)frame->width * 4 };
							sws_scale(sws, target->data, target->linesize, 0, frame->height, dst_data, dst_linesize);
							sws_freeContext(sws);

							D3D11_TEXTURE2D_DESC desc = {};
							desc.Width = frame->width;
							desc.Height = frame->height;
							desc.MipLevels = 1;
							desc.ArraySize = 1;
							desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
							desc.SampleDesc.Count = 1;
							desc.Usage = D3D11_USAGE_DEFAULT;
							desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
							D3D11_SUBRESOURCE_DATA init_data = {};
							init_data.pSysMem = pixels.data();
							init_data.SysMemPitch = frame->width * 4;
							Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
							if (SUCCEEDED(device_manager_.GetDevice()->CreateTexture2D(&desc, &init_data, tex.ReleaseAndGetAddressOf()))) {
								gpu_frame.texture = tex;
								auto decode_end = std::chrono::high_resolution_clock::now();
								auto decode_ms = std::chrono::duration_cast<std::chrono::milliseconds>(decode_end - decode_start).count();
								decode_stage_ms_.Add(static_cast<double>(decode_ms));
								PipelineLogFmt("[DecodeThread] SW frame uploaded to GPU texture, time=%lld ms\n", decode_ms);
							}
						}
						av_frame_free(&sw_frame);
					}
					if (!gpu_frame.texture.Get()) {
						PipelineLog("[DecodeThread] WARNING: Failed to upload SW frame, skipping\n");
					}
				}

					gpu_frame.width = frame->width;
					gpu_frame.height = frame->height;
					gpu_frame.pts = frame->pts;

					// キューに追加（テクスチャが有効な場合のみ）
					if (gpu_frame.texture.Get()) {
						{
							std::unique_lock<std::mutex> lock(video_mutex_);
							// キューが溢れないように最大サイズを制限
							auto queue_wait_start = std::chrono::high_resolution_clock::now();
							video_cv_.wait(lock, [this] {
								return video_queue_.size() < 16 || stop_requested_.load();
							});
							auto queue_wait_end = std::chrono::high_resolution_clock::now();
							decode_queue_wait_ms_.Add(ElapsedMs(queue_wait_start, queue_wait_end));
							video_queue_.push(gpu_frame);
						}
						video_cv_.notify_one();

						processed_frames_++;
						if (processed_frames_ % 30 == 0) {
							PipelineLogFmt("[DecodeThread] Decoded %d frames\n", processed_frames_.load());
						}
					}
				}
			}
		}

			av_packet_unref(packet);
			}

			// デコード終了マーカーを推論キューへ送信
			{
				std::lock_guard<std::mutex> lock(video_mutex_);
				video_queue_.push(GpuFrame()); // 空フレームで終了通知
			}
			video_cv_.notify_one();
			PipelineLog("[DecodeThread] Sent end marker to inference queue\n");

			av_frame_free(&frame);
			av_packet_free(&packet);
		}

void VideoPipeline::InferenceThread() {
	PipelineLog("[InferenceThread] Started\n");
	int loop_count = 0;
	while (!stop_requested_.load() || !video_queue_.empty()) {
		loop_count++;
		if (loop_count == 1) {
			PipelineLog("[InferenceThread] Entering main loop\n");
		}
		GpuFrame frame;

		{
			std::unique_lock<std::mutex> lock(video_mutex_);
			video_cv_.wait(lock, [this] {
				return !video_queue_.empty() || stop_requested_.load();
			});

			if (video_queue_.empty()) {
				PipelineLog("[InferenceThread] Video queue empty, continuing\n");
				continue;
			}

					frame = video_queue_.front();
					video_queue_.pop();
				}
				video_cv_.notify_one(); // デコード側の「queue<8」待機を解除

				if (!frame.texture.Get()) {
				PipelineLog("[InferenceThread] Received end marker from decode queue\n");
				break;
			}

			PipelineLogFmt("[InferenceThread] Processing frame %d (texture=%p)\n", loop_count, frame.texture.Get());
			auto inference_stage_start = std::chrono::high_resolution_clock::now();

		// 出力は常に元フレーム解像度を基準にする
		auto current_texture = frame.texture;
		PreProcessResult preproc{};

		// ONNX Runtime推論（PreviewPipelineと同様に元フレームを直接入力）
		if (winml_utils_.IsModelLoaded() && frame.texture.Get()) {
			try {
				// 推論実行（D3D11テクスチャから直接入力）
				auto input_names = winml_utils_.GetInputNames();
				std::vector<float> output_data;
				if (!input_names.empty()) {
					output_data = winml_utils_.EvaluateFromTexture(
						input_names[0],
						frame.texture.Get(),
						frame.width,
						frame.height
					);

					// 検出結果を処理
					auto detections = PostProcessDetections(output_data, preproc, frame.width, frame.height);
					std::vector<Detection> mask_detections = detections;

					if (config_.exclude_by_name_enabled && tracker_) {
						inference_frame_counter_++;

						std::vector<ByteTrackInterop::DetectionPtr> tracker_inputs;
						tracker_inputs.reserve(detections.size());
						for (const auto& det : detections) {
							tracker_inputs.push_back(std::make_shared<ByteTrackInterop::Detection>(det));
						}

						auto tracked = tracker_->update(tracker_inputs);
						PipelineLogFmt("[ExcludeByName][Video] frame=%d det_in=%zu tracked=%zu\n",
							loop_count,
							detections.size(),
							tracked.size());
						size_t ocr_queue_depth_snapshot = 0;
						{
							std::lock_guard<std::mutex> lock(ocr_mutex_);
							ocr_queue_depth_snapshot = ocr_queue_.size();
						}
						const bool ocr_backlogged = ocr_queue_depth_snapshot >= 1;
						const int ocr_batch_limit = std::clamp(config_.ocr_max_rois_per_frame, 1, 16);

						std::vector<OcrTrackRoi> ocr_new_rois;
						std::vector<OcrTrackRoi> ocr_refresh_rois;
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

							bool has_text = false;
							bool need_refresh = false;
							bool is_pending = false;
							{
								std::lock_guard<std::mutex> lock(ocr_result_mutex_);
								const auto last_it = ocr_last_frame_by_track_.find(track_id);
								has_text = ocr_text_by_track_id_.find(track_id) != ocr_text_by_track_id_.end();
								need_refresh = (last_it == ocr_last_frame_by_track_.end()) ||
									((inference_frame_counter_ - last_it->second) >= ocr_refresh_interval_frames_);
								is_pending = ocr_pending_track_ids_.find(track_id) != ocr_pending_track_ids_.end();
							}

							if ((!has_text || need_refresh) && !is_pending) {
								if (!has_text) {
									ocr_new_rois.push_back({ track_id, d });
								} else if (!ocr_backlogged) {
									ocr_refresh_rois.push_back({ track_id, d });
								}
							}
						}

						std::vector<OcrTrackRoi> ocr_rois;
						ocr_rois.reserve(static_cast<size_t>(ocr_batch_limit));
						for (const auto& roi : ocr_new_rois) {
							if (ocr_rois.size() >= static_cast<size_t>(ocr_batch_limit)) {
								break;
							}
							ocr_rois.push_back(roi);
						}
						for (const auto& roi : ocr_refresh_rois) {
							if (ocr_rois.size() >= static_cast<size_t>(ocr_batch_limit)) {
								break;
							}
							ocr_rois.push_back(roi);
						}
						if (ocr_backlogged && !ocr_refresh_rois.empty()) {
							PipelineLogFmt("[ExcludeByName][Video] frame=%d ocr_refresh_deferred=%zu queue_depth=%zu\n",
								loop_count,
								ocr_refresh_rois.size(),
								ocr_queue_depth_snapshot);
						}

						if (!ocr_rois.empty() && ocr_recognizer_.IsReady()) {
							OcrRequest req;
							req.texture = current_texture;
							req.width = frame.width;
							req.height = frame.height;
							req.frame_number = loop_count;
							req.inference_counter = inference_frame_counter_;
							req.rois = std::move(ocr_rois);

							bool queued = false;
							size_t queue_depth_after = 0;
							{
								std::lock_guard<std::mutex> lock(ocr_mutex_);
								if (ocr_queue_.size() < 8) {
									queued = true;
									ocr_queue_.push(req);
									queue_depth_after = ocr_queue_.size();
								} else {
									queue_depth_after = ocr_queue_.size();
								}
							}

							if (queued) {
								{
									std::lock_guard<std::mutex> lock(ocr_result_mutex_);
									for (const auto& roi : req.rois) {
										ocr_pending_track_ids_.insert(roi.track_id);
									}
								}
								ocr_cv_.notify_one();
								PipelineLogFmt("[ExcludeByName][Video] frame=%d ocr_enqueued=%zu queue_depth=%zu\n", loop_count, req.rois.size(), queue_depth_after);
							} else {
								PipelineLogFmt("[ExcludeByName][Video] frame=%d ocr_queue_full drop=%zu queue_depth=%zu\n", loop_count, req.rois.size(), queue_depth_after);
							}
						}

						mask_detections.clear();
						mask_detections.reserve(tracked_entries.size());
						for (const auto& entry : tracked_entries) {
							if (entry.second != 0 && !mask_exclude_texts_.empty()) {
								std::string cached_text;
								{
									std::lock_guard<std::mutex> lock(ocr_result_mutex_);
									auto txt_it = ocr_text_by_track_id_.find(entry.second);
									if (txt_it != ocr_text_by_track_id_.end()) {
										cached_text = txt_it->second;
									}
								}
								if (!cached_text.empty()) {
									const bool excluded = TextMatch::IsExcludedBySimilarity(cached_text, mask_exclude_texts_, config_.text_similarity_threshold);
									PipelineLogFmt("[ExcludeByName][Video][Match] frame=%d track=%llu text='%s' excluded=%d\n",
										loop_count,
										static_cast<unsigned long long>(entry.second),
										cached_text.c_str(),
										excluded ? 1 : 0);
									if (excluded) {
										continue;
									}
								}
							}
							mask_detections.push_back(entry.first);
						}
						PipelineLogFmt("[ExcludeByName][Video] frame=%d mask_after=%zu\n", loop_count, mask_detections.size());
					}

					if (loop_count % 30 == 0) {
						PipelineLogFmt("[InferenceThread] frame=%d detections=%zu\n", loop_count, mask_detections.size());
					}
					detected_objects_ += static_cast<int>(mask_detections.size());

					// マスク処理を適用（blacked_type != MaskType::No_Inferenceの場合）
					if (config_.blacked_type != MaskType::No_Inference && !mask_detections.empty() && current_texture.Get()) {
						if (loop_count % 30 == 0) {
							PipelineLogFmt("[InferenceThread] Applying mask type=%d, det=%zu\n", static_cast<int>(config_.blacked_type), mask_detections.size());
						}
						// マスクテクスチャ生成（元のフレーム解像度で）
						auto mask_texture = mask_shader_.CreateMaskTexture(
							frame.width,
							frame.height,
							mask_detections
						);

								if (mask_texture.Get()) {
									Microsoft::WRL::ComPtr<ID3D11Texture2D> output_texture;
									if (config_.blacked_type == MaskType::Mosaic) {
										mask_shader_.ApplyMosaic(
											current_texture.Get(),
											mask_texture.Get(),
											mosaic_size_,
											output_texture
										);
									} else if (config_.blacked_type == MaskType::Blur) {
										mask_shader_.ApplyBlur(
											current_texture.Get(),
											mask_texture.Get(),
											blur_radius_,
											output_texture
										);
									} else if (config_.blacked_type == MaskType::Inpaint) {
										mask_shader_.ApplyInpaint(
											current_texture.Get(),
											mask_texture.Get(),
											config_.blackedout_param,
											output_texture
										);
									} else {
										float color[4] = {
											config_.name_color.r / 255.0f,
											config_.name_color.g / 255.0f,
											config_.name_color.b / 255.0f,
											1.0f
										};
										mask_shader_.ApplyRectFill(
											current_texture.Get(),
											mask_texture.Get(),
											color,
											output_texture
										);
									}

									if (output_texture.Get()) {
										current_texture = output_texture;
									}
								}
								// FlushはGPUパイプライン全体を停滞させるため常時実行しない
							}
						} // if (!input_names.empty())
			} catch (const winrt::hresult_error& ex) {
				std::cerr << "WinML inference error: " << winrt::to_string(ex.message()) << std::endl;
			}
			catch (const std::exception& ex) {
				std::cerr << "Inference error: " << ex.what() << std::endl;
			}
		}

		// 固定矩形マスクを適用（PreviewPipelineと同等のマスクテクスチャ経路）
		if (config_.fixed_rect_count > 0 && current_texture.Get()) {
			std::vector<Detection> fixed_detections;
			fixed_detections.reserve(config_.fixed_rect_count);
			for (int i = 0; i < config_.fixed_rect_count && i < 64; ++i) {
				const auto& rect = config_.fixed_rects[i];
				if (rect.width < 10 || rect.height < 10) continue;
				Detection d;
				d.class_id = -1;
				d.score = 1.0f;
				d.x1 = static_cast<float>(rect.x);
				d.y1 = static_cast<float>(rect.y);
				d.x2 = static_cast<float>(rect.x + rect.width);
				d.y2 = static_cast<float>(rect.y + rect.height);
				fixed_detections.push_back(d);
			}

			if (!fixed_detections.empty()) {
				auto fixed_mask = mask_shader_.CreateMaskTexture(frame.width, frame.height, fixed_detections);
				if (fixed_mask.Get()) {
					Microsoft::WRL::ComPtr<ID3D11Texture2D> fixed_output;
					bool fixed_result = false;
					switch (config_.fixmask_type) {
						case MaskType::Inpaint:
							fixed_result = mask_shader_.ApplyInpaint(current_texture.Get(), fixed_mask.Get(), config_.fixmask_param, fixed_output);
							break;
						case MaskType::Mosaic:
							fixed_result = mask_shader_.ApplyMosaic(current_texture.Get(), fixed_mask.Get(), config_.fixmask_param, fixed_output);
							break;
						case MaskType::Blur:
							fixed_result = mask_shader_.ApplyBlur(current_texture.Get(), fixed_mask.Get(), config_.fixmask_param, fixed_output);
							break;
						case MaskType::RectFill:
						default: {
							float color[4] = {
								config_.fixframe_color.r / 255.0f,
								config_.fixframe_color.g / 255.0f,
								config_.fixframe_color.b / 255.0f,
								1.0f
							};
							fixed_result = mask_shader_.ApplyRectFill(current_texture.Get(), fixed_mask.Get(), color, fixed_output);
							break;
						}
					}

					if (fixed_result && fixed_output.Get()) {
						current_texture = fixed_output;
					}
					// FlushはGPUパイプライン全体を停滞させるため常時実行しない
				}
			}
		}

			// NOTE: Copyright overlay is deferred to EncodeThread after scaling to avoid position mismatch
			// (InferenceThread applies it on source size, but EncodeThread will rescale to output resolution)

			GpuFrame inference_frame;
			inference_frame.texture = current_texture;
		inference_frame.width = frame.width;
		inference_frame.height = frame.height;
		inference_frame.pts = frame.pts;

		{
			std::unique_lock<std::mutex> lock(inference_mutex_);
			// エンコードキューが溢れないように最大サイズを制限
			inference_cv_.wait(lock, [this] {
				return inference_output_queue_.size() < 16 || stop_requested_.load();
			});
			inference_output_queue_.push(inference_frame);
		}
		inference_cv_.notify_one();
		PipelineLogFmt("[InferenceThread] Pushed frame %d to encode queue (texture=%p)\n", loop_count, inference_frame.texture.Get());
		auto inference_stage_end = std::chrono::high_resolution_clock::now();
		inference_stage_ms_.Add(ElapsedMs(inference_stage_start, inference_stage_end));

		total_frames_++;
		if (total_frames_ % 30 == 0) {
			PipelineLogFmt("[InferenceThread] Processed %d frames\n", total_frames_.load());
		}
	}

	// 推論終了マーク
	ocr_stop_requested_ = true;
	ocr_cv_.notify_all();
	PipelineLog("[InferenceThread] Sending end marker to encode queue\n");
	{
		std::lock_guard<std::mutex> lock(inference_mutex_);
		inference_output_queue_.push(GpuFrame()); // 空のフレームで終了通知
	}
	inference_cv_.notify_one();
	PipelineLog("[InferenceThread] Exiting\n");
}

void VideoPipeline::OcrThread() {
	PipelineLog("[OcrThread] Started\n");
	while (true) {
		OcrRequest req;
		size_t aggregated_jobs = 0;
		size_t aggregated_input_rois = 0;
		{
			std::unique_lock<std::mutex> lock(ocr_mutex_);
			ocr_cv_.wait(lock, [this] {
				return !ocr_queue_.empty() || ocr_stop_requested_.load();
			});
			if (ocr_stop_requested_.load()) {
				size_t dropped_jobs = 0;
				size_t dropped_rois = 0;
				while (!ocr_queue_.empty()) {
					dropped_rois += ocr_queue_.front().rois.size();
					ocr_queue_.pop();
					dropped_jobs++;
				}
				lock.unlock();
				{
					std::lock_guard<std::mutex> result_lock(ocr_result_mutex_);
					ocr_pending_track_ids_.clear();
				}
				PipelineLogFmt("[OcrThread] Stop requested, dropped queued OCR jobs=%zu rois=%zu\n", dropped_jobs, dropped_rois);
				break;
			}

			if (ocr_queue_.empty()) {
				continue;
			}

			req = ocr_queue_.front();
			aggregated_input_rois += req.rois.size();
			aggregated_jobs = 1;
			ocr_queue_.pop();

			const size_t max_aggregate_jobs = 4;
			const size_t max_aggregate_rois = static_cast<size_t>(std::clamp(config_.ocr_max_rois_per_frame * 4, 4, 32));
			std::unordered_set<uint64_t> seen_track_ids;
			seen_track_ids.reserve(max_aggregate_rois);
			for (const auto& roi : req.rois) {
				seen_track_ids.insert(roi.track_id);
			}

			while (!ocr_queue_.empty() &&
				aggregated_jobs < max_aggregate_jobs &&
				req.rois.size() < max_aggregate_rois) {
				OcrRequest extra = ocr_queue_.front();
				ocr_queue_.pop();
				aggregated_jobs++;
				aggregated_input_rois += extra.rois.size();

				if (extra.texture.Get()) {
					req.texture = extra.texture;
					req.width = extra.width;
					req.height = extra.height;
					req.frame_number = extra.frame_number;
					req.inference_counter = extra.inference_counter;
				}

				for (const auto& roi : extra.rois) {
					if (req.rois.size() >= max_aggregate_rois) {
						break;
					}
					if (seen_track_ids.insert(roi.track_id).second) {
						req.rois.push_back(roi);
					}
				}
			}
		}

		if (!req.texture.Get() || req.rois.empty()) {
			continue;
		}

		try {
			auto ocr_start = std::chrono::high_resolution_clock::now();
			auto ocr_results = ocr_recognizer_.Recognize(
				req.texture.Get(),
				req.width,
				req.height,
				req.rois,
				config_.ocr_expand_pixels,
				config_.ocr_max_rois_per_frame);
			auto ocr_end = std::chrono::high_resolution_clock::now();
			auto ocr_ms = ElapsedMs(ocr_start, ocr_end);
			ocr_stage_ms_.Add(ocr_ms);
			PipelineLogFmt("[OcrThread] aggregated_jobs=%zu input_rois=%zu merged_rois=%zu frame=%d\n",
				aggregated_jobs,
				aggregated_input_rois,
				req.rois.size(),
				req.frame_number);
			PipelineLogFmt("[PipelineTiming][OCR] frame=%d rois=%zu time=%.2f ms\n", req.frame_number, req.rois.size(), ocr_ms);

			std::unordered_map<uint64_t, std::string> sanitized_by_track;
			for (const auto& ocr : ocr_results) {
				const std::string sanitized = TextMatch::Sanitize(ocr.text);
				if (!sanitized.empty()) {
					sanitized_by_track[ocr.track_id] = sanitized;
				}
				PipelineLogFmt("[ExcludeByName][Video][OCR] frame=%d track=%llu raw='%s' sanitized='%s' conf=%.3f\n",
					req.frame_number,
					static_cast<unsigned long long>(ocr.track_id),
					ocr.text.c_str(),
					sanitized.c_str(),
					ocr.confidence);
			}

			{
				std::lock_guard<std::mutex> lock(ocr_result_mutex_);
				for (const auto& roi : req.rois) {
					ocr_last_frame_by_track_[roi.track_id] = req.inference_counter;
					ocr_pending_track_ids_.erase(roi.track_id);
				}

				for (const auto& kv : sanitized_by_track) {
					ocr_text_by_track_id_[kv.first] = kv.second;
					ocr_last_frame_by_track_[kv.first] = req.inference_counter;
				}
			}
		} catch (const Ort::Exception& ex) {
			PipelineLogFmt("[OcrThread] ORT exception: %s\n", ex.what());
			std::lock_guard<std::mutex> lock(ocr_result_mutex_);
			for (const auto& roi : req.rois) {
				ocr_pending_track_ids_.erase(roi.track_id);
			}
		} catch (const std::exception& ex) {
			PipelineLogFmt("[OcrThread] std::exception: %s\n", ex.what());
			std::lock_guard<std::mutex> lock(ocr_result_mutex_);
			for (const auto& roi : req.rois) {
				ocr_pending_track_ids_.erase(roi.track_id);
			}
		} catch (...) {
			PipelineLog("[OcrThread] unknown exception\n");
			std::lock_guard<std::mutex> lock(ocr_result_mutex_);
			for (const auto& roi : req.rois) {
				ocr_pending_track_ids_.erase(roi.track_id);
			}
		}
	}

	PipelineLog("[OcrThread] Exiting\n");
}

void VideoPipeline::EncodeThread() {
	PipelineLog("[EncodeThread] Started\n");
	AVPacket* encoded_packet = av_packet_alloc();
	AVRational input_video_time_base = { 1, std::max(1, config_.fps) };
	int64_t trim_start_video_ts = 0;
	if (input_format_ctx_ && video_stream_index_ >= 0 && video_stream_index_ < static_cast<int>(input_format_ctx_->nb_streams)) {
		AVStream* input_video_stream = input_format_ctx_->streams[video_stream_index_];
		if (input_video_stream && input_video_stream->time_base.den > 0) {
			input_video_time_base = input_video_stream->time_base;
			if (config_.trim_start_seconds > 0.0) {
				trim_start_video_ts = av_rescale_q(
					static_cast<int64_t>(std::llround(config_.trim_start_seconds * AV_TIME_BASE)),
					AVRational{ 1, AV_TIME_BASE },
					input_video_time_base);
			}
		}
	}

	// メインエンコードループ
	int encoded_count = 0;
	int written_video_packets = 0;
	int64_t video_pts_counter = 0;
	int64_t last_output_pts = AV_NOPTS_VALUE;
	bool write_failed = false;
	PipelineLog("[EncodeThread] Entering main encode loop\n");
	while (!stop_requested_.load() || !inference_output_queue_.empty()) {
		GpuFrame frame;

		PipelineLog("[EncodeThread] Waiting for frame...\n");
		{
			std::unique_lock<std::mutex> lock(inference_mutex_);
			auto queue_wait_start = std::chrono::high_resolution_clock::now();
			inference_cv_.wait(lock, [this] {
				return !inference_output_queue_.empty() || stop_requested_.load();
			});
			auto queue_wait_end = std::chrono::high_resolution_clock::now();
			encode_queue_wait_ms_.Add(ElapsedMs(queue_wait_start, queue_wait_end));

			if (inference_output_queue_.empty()) {
				PipelineLog("[EncodeThread] Queue empty, continuing\n");
				continue;
			}

			frame = inference_output_queue_.front();
			inference_output_queue_.pop();
			PipelineLogFmt("[EncodeThread] Got frame from queue (texture=%p, queue_size=%zu)\n",
				frame.texture.Get(), inference_output_queue_.size());
		}
		inference_cv_.notify_one(); // 推論側の「queue<8」待機を解除

		// 終了マークの場合
		if (!frame.texture.Get()) {
			PipelineLog("[EncodeThread] Received end marker\n");
			break;
		}

		// D3D11 HWフレームとしてエンコーダに直接送信（GPUゼロコピー）
		D3D11_TEXTURE2D_DESC tex_desc = {};
		frame.texture->GetDesc(&tex_desc);

		int enc_w = encoder_ctx_->width;
		int enc_h = encoder_ctx_->height;
		if ((int)tex_desc.Width != enc_w || (int)tex_desc.Height != enc_h) {
			PipelineLogFmt("[EncodeThread] Input texture size %ux%u differs from encoder %dx%d, scaling will be applied\n",
				tex_desc.Width, tex_desc.Height, enc_w, enc_h);
		}

		// Apply copyright overlay BEFORE scaling to output resolution
		// This ensures the copyright position matches the preview
		auto copyright_texture = frame.texture;
		if (config_.enable_copyright && frame.texture.Get() && copyright_srv_.Get()) {
			float scale = std::clamp(config_.copyright_scale, 0.5f, 3.0f);
			uint32_t target_w = std::max<uint32_t>(1, static_cast<uint32_t>(std::round(copyright_width_ * scale)));
			uint32_t target_h = std::max<uint32_t>(1, static_cast<uint32_t>(std::round(copyright_height_ * scale)));
			if (target_w > 0 && target_h > 0) {
				// Copyright position based on the ORIGINAL frame size (same as preview)
				// with configurable offset from config_
				const int frame_w_i = static_cast<int>(frame.width);
				const int frame_h_i = static_cast<int>(frame.height);
				int pos_x = frame_w_i - static_cast<int>(target_w) + config_.copyright_offset_x;
				int pos_y = frame_h_i - static_cast<int>(target_h) + config_.copyright_offset_y;

				const int max_x = frame_w_i - static_cast<int>(target_w);
				const int max_y = frame_h_i - static_cast<int>(target_h);
				pos_x = std::max(0, std::min(pos_x, std::max(0, max_x)));
				pos_y = std::max(0, std::min(pos_y, std::max(0, max_y)));

				Microsoft::WRL::ComPtr<ID3D11Texture2D> watermark_output;
				if (mask_shader_.ApplyCopyrightOverlay(
					frame.texture.Get(),
					copyright_srv_.Get(),
					target_w,
					target_h,
					pos_x,
					pos_y,
					watermark_output) && watermark_output.Get()) {
					copyright_texture = watermark_output;
				}
			}
		}

		int64_t output_pts = video_pts_counter;
		if (frame.pts != AV_NOPTS_VALUE) {
			int64_t normalized_pts = frame.pts;
			if (trim_start_video_ts > 0) {
				normalized_pts -= trim_start_video_ts;
				if (normalized_pts < 0) {
					normalized_pts = 0;
				}
			}
			output_pts = av_rescale_q(normalized_pts, input_video_time_base, encoder_ctx_->time_base);
			if (output_pts < 0) {
				output_pts = 0;
			}
			if (last_output_pts != AV_NOPTS_VALUE && output_pts <= last_output_pts) {
				continue;
			}
		}
		last_output_pts = output_pts;
		video_pts_counter = std::max(video_pts_counter, output_pts + 1);

		auto encode_start = std::chrono::high_resolution_clock::now();
		int ret = 0;
		if (use_d3d11_hw_encode_ && encoder_ctx_->hw_frames_ctx) {
			// Phase 3A: D3D11 HWフレーム（GPUゼロコピー）パス
			PipelineLog("[EncodeThread] Using D3D11 HW frame encode path\n");

			AVFrame* hw_frame = av_frame_alloc();
			if (!hw_frame) {
				PipelineLog("[EncodeThread] Failed to alloc HW frame\n");
				continue;
			}

			ret = av_hwframe_get_buffer(encoder_ctx_->hw_frames_ctx, hw_frame, 0);
			if (ret < 0) {
				PipelineLogFmt("[EncodeThread] av_hwframe_get_buffer failed: %d, skipping frame\n", ret);
				av_frame_free(&hw_frame);
				continue;
			}

			// hw_frame->data[0] = ID3D11Texture2D*, data[1] = subresource index
			ID3D11Texture2D* nv12_texture = reinterpret_cast<ID3D11Texture2D*>(hw_frame->data[0]);
			if (!nv12_texture) {
				PipelineLog("[EncodeThread] HW frame has no D3D11 texture, skipping frame\n");
				av_frame_free(&hw_frame);
				continue;
			}

			// D3D11VideoProcessorでBGRA→NV12変換（GPUハードウェア）
			if (!device_manager_.ConvertBGRAToNV12(copyright_texture.Get(), nv12_texture)) {
				PipelineLog("[EncodeThread] D3D11VideoProcessor conversion failed, skipping frame\n");
				av_frame_free(&hw_frame);
				continue;
			}

			device_manager_.GetContext()->Flush();
			hw_frame->pts = output_pts;
			PipelineLog("[EncodeThread] Sending D3D11 HW frame to encoder...\n");
			ret = avcodec_send_frame(encoder_ctx_, hw_frame);
			auto encode_end = std::chrono::high_resolution_clock::now();
			auto encode_ms = std::chrono::duration_cast<std::chrono::milliseconds>(encode_end - encode_start).count();
			encode_stage_ms_.Add(static_cast<double>(encode_ms));
			PipelineLogFmt("[EncodeThread] avcodec_send_frame (HW) returned: %d, total_encode_time=%lld ms\n", ret, encode_ms);
			av_frame_free(&hw_frame);
		} else {
			// SWフレームパス（フォールバック）
			PipelineLog("[EncodeThread] Using SW frame encode path\n");

			// GPU内でBGRA→NV12変換
			Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_y;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_uv;
			bool gpu_convert_ok = mask_shader_.ConvertBGRAToNV12(
				copyright_texture.Get(), enc_w, enc_h, nv12_y, nv12_uv);

			if (!gpu_convert_ok || !nv12_y.Get() || !nv12_uv.Get()) {
				PipelineLog("[EncodeThread] GPU BGRA→NV12 conversion failed, skipping frame\n");
				continue;
			}

			// NV12 Y/UVプレーンをCPUメモリ（AVFrame）へ転送
			D3D11_TEXTURE2D_DESC y_desc = {};
			nv12_y->GetDesc(&y_desc);
			D3D11_TEXTURE2D_DESC uv_desc = {};
			nv12_uv->GetDesc(&uv_desc);

			D3D11_TEXTURE2D_DESC y_staging_desc = y_desc;
			y_staging_desc.Usage = D3D11_USAGE_STAGING;
			y_staging_desc.BindFlags = 0;
			y_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> y_staging;
			HRESULT hr = device_manager_.GetDevice()->CreateTexture2D(&y_staging_desc, nullptr, y_staging.ReleaseAndGetAddressOf());
			if (FAILED(hr)) {
				PipelineLog("[EncodeThread] Failed to create Y staging texture\n");
				continue;
			}
			device_manager_.GetContext()->CopyResource(y_staging.Get(), nv12_y.Get());

			D3D11_TEXTURE2D_DESC uv_staging_desc = uv_desc;
			uv_staging_desc.Usage = D3D11_USAGE_STAGING;
			uv_staging_desc.BindFlags = 0;
			uv_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> uv_staging;
			hr = device_manager_.GetDevice()->CreateTexture2D(&uv_staging_desc, nullptr, uv_staging.ReleaseAndGetAddressOf());
			if (FAILED(hr)) {
				PipelineLog("[EncodeThread] Failed to create UV staging texture\n");
				continue;
			}
			device_manager_.GetContext()->CopyResource(uv_staging.Get(), nv12_uv.Get());
			device_manager_.GetContext()->Flush();

			AVFrame* sw_frame = av_frame_alloc();
			if (!sw_frame) {
				PipelineLog("[EncodeThread] Failed to alloc AVFrame\n");
				continue;
			}
			sw_frame->format = AV_PIX_FMT_NV12;
			sw_frame->width = enc_w;
			sw_frame->height = enc_h;
			ret = av_frame_get_buffer(sw_frame, 0);
			if (ret < 0) {
				PipelineLog("[EncodeThread] Failed to alloc SW frame buffer\n");
				av_frame_free(&sw_frame);
				continue;
			}

			D3D11_MAPPED_SUBRESOURCE y_mapped = {};
			hr = device_manager_.GetContext()->Map(y_staging.Get(), 0, D3D11_MAP_READ, 0, &y_mapped);
			if (SUCCEEDED(hr)) {
				for (int y = 0; y < enc_h; ++y) {
					memcpy(sw_frame->data[0] + y * sw_frame->linesize[0],
						static_cast<const uint8_t*>(y_mapped.pData) + y * y_mapped.RowPitch,
						enc_w);
				}
				device_manager_.GetContext()->Unmap(y_staging.Get(), 0);
			}

			D3D11_MAPPED_SUBRESOURCE uv_mapped = {};
			hr = device_manager_.GetContext()->Map(uv_staging.Get(), 0, D3D11_MAP_READ, 0, &uv_mapped);
			if (SUCCEEDED(hr)) {
				int uv_h = (enc_h + 1) / 2;
				int uv_w = (enc_w + 1) / 2;
				for (int y = 0; y < uv_h; ++y) {
					memcpy(sw_frame->data[1] + y * sw_frame->linesize[1],
						static_cast<const uint8_t*>(uv_mapped.pData) + y * uv_mapped.RowPitch,
						uv_w * 2);
				}
				device_manager_.GetContext()->Unmap(uv_staging.Get(), 0);
			}

			sw_frame->pts = output_pts;
			PipelineLog("[EncodeThread] Sending NV12 SW frame to encoder...\n");
			ret = avcodec_send_frame(encoder_ctx_, sw_frame);
			auto encode_end = std::chrono::high_resolution_clock::now();
			auto encode_ms = std::chrono::duration_cast<std::chrono::milliseconds>(encode_end - encode_start).count();
			encode_stage_ms_.Add(static_cast<double>(encode_ms));
			PipelineLogFmt("[EncodeThread] avcodec_send_frame (SW) returned: %d\n", ret);
			PipelineLogFmt("[EncodeThread] total_encode_time=%lld ms\n", encode_ms);
			av_frame_free(&sw_frame);
		}

		// パケット受信（HW/SW共通）
		if (ret >= 0) {
			int packet_count = 0;
			while (true) {
				ret = avcodec_receive_packet(encoder_ctx_, encoded_packet);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
				if (ret < 0) {
					PipelineLog("[EncodeThread] avcodec_receive_packet error\n");
					break;
				}

				encoded_packet->stream_index = output_video_stream_index_;
				av_packet_rescale_ts(encoded_packet,
					encoder_ctx_->time_base,
					output_format_ctx_->streams[output_video_stream_index_]->time_base);
				int write_ret = av_interleaved_write_frame(output_format_ctx_, encoded_packet);
				if (write_ret < 0) {
					write_failed = true;
				} else {
					written_video_packets++;
				}
				av_packet_unref(encoded_packet);
				packet_count++;
			}
			PipelineLogFmt("[EncodeThread] Received %d packets\n", packet_count);
		} else {
			PipelineLogFmt("[EncodeThread] avcodec_send_frame failed: %d\n", ret);
		}

		encoded_count++;
		if (encoded_count % 30 == 0) {
			PipelineLogFmt("[EncodeThread] Encoded %d frames\n", encoded_count);
		}
	}
	PipelineLogFmt("[EncodeThread] Main loop ended. Total encoded: %d\n", encoded_count);

	// フラッシュ
	avcodec_send_frame(encoder_ctx_, nullptr);
	while (avcodec_receive_packet(encoder_ctx_, encoded_packet) == 0) {
		encoded_packet->stream_index = output_video_stream_index_;
		av_packet_rescale_ts(encoded_packet,
			encoder_ctx_->time_base,
			output_format_ctx_->streams[output_video_stream_index_]->time_base);
		int flush_write_ret = av_interleaved_write_frame(output_format_ctx_, encoded_packet);
		if (flush_write_ret < 0) {
			write_failed = true;
		} else {
			written_video_packets++;
		}
		av_packet_unref(encoded_packet);
	}

	// 音声は再エンコードせず、入力パケットをそのまま出力へmuxする
	if (audio_stream_index_ >= 0 && output_audio_stream_index_ >= 0) {
		std::queue<AVPacket*> local_audio_queue;
		{
			std::lock_guard<std::mutex> lock(audio_mutex_);
			std::swap(local_audio_queue, audio_queue_);
		}

		AVStream* in_audio_stream = input_format_ctx_->streams[audio_stream_index_];
		AVStream* out_audio_stream = output_format_ctx_->streams[output_audio_stream_index_];
		const int64_t trim_start_audio_ts = (config_.trim_start_seconds > 0.0)
			? av_rescale_q(static_cast<int64_t>(std::llround(config_.trim_start_seconds * AV_TIME_BASE)), AVRational{1, AV_TIME_BASE}, in_audio_stream->time_base)
			: 0;
		const int64_t trim_end_audio_ts = (config_.trim_end_seconds > config_.trim_start_seconds && config_.trim_end_seconds > 0.0)
			? av_rescale_q(static_cast<int64_t>(std::llround(config_.trim_end_seconds * AV_TIME_BASE)), AVRational{1, AV_TIME_BASE}, in_audio_stream->time_base)
			: AV_NOPTS_VALUE;

		while (!local_audio_queue.empty()) {
			AVPacket* pkt = local_audio_queue.front();
			local_audio_queue.pop();
			if (!pkt) continue;

			if (trim_start_audio_ts > 0 || trim_end_audio_ts != AV_NOPTS_VALUE) {
				int64_t pkt_ts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
				if (pkt_ts != AV_NOPTS_VALUE) {
					if (trim_end_audio_ts != AV_NOPTS_VALUE && pkt_ts > trim_end_audio_ts) {
						av_packet_free(&pkt);
						continue;
					}
					if (pkt_ts < trim_start_audio_ts) {
						av_packet_free(&pkt);
						continue;
					}
				}

				if (pkt->pts != AV_NOPTS_VALUE) {
					pkt->pts -= trim_start_audio_ts;
				}
				if (pkt->dts != AV_NOPTS_VALUE) {
					pkt->dts -= trim_start_audio_ts;
				}
				if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < 0) pkt->pts = 0;
				if (pkt->dts != AV_NOPTS_VALUE && pkt->dts < 0) pkt->dts = 0;
			}

			pkt->stream_index = output_audio_stream_index_;
			av_packet_rescale_ts(pkt, in_audio_stream->time_base, out_audio_stream->time_base);
			int audio_write_ret = av_interleaved_write_frame(output_format_ctx_, pkt);
			if (audio_write_ret < 0) {
				write_failed = true;
			}
			av_packet_free(&pkt);
		}
	}

	int trailer_ret = av_write_trailer(output_format_ctx_);
	PipelineLogFmt("[EncodeThread] av_write_trailer returned: %d\n", trailer_ret);
	if (trailer_ret < 0) {
		write_failed = true;
	}

	encode_failed_ = write_failed || (encoded_count <= 0) || (written_video_packets <= 0);
	if (encode_failed_.load()) {
		PipelineLog("[EncodeThread] Encode failed or no frames were written\n");
	}
	PipelineLogFmt("[EncodeThread] Encoded frames=%d, written video packets=%d\n", encoded_count, written_video_packets);

	av_packet_free(&encoded_packet);
}

PreProcessResult VideoPipeline::LetterboxTransform(const GpuFrame& input) {
	PreProcessResult preproc{};

	// Letterbox変換のパラメータを計算
	float scale_x = static_cast<float>(config_.input_width) / input.width;
	float scale_y = static_cast<float>(config_.input_height) / input.height;
	float scale = std::min(scale_x, scale_y);

	int scaled_width = static_cast<int>(input.width * scale);
	int scaled_height = static_cast<int>(input.height * scale);

	float pad_left = (config_.input_width - scaled_width) / 2.0f;
	float pad_top = (config_.input_height - scaled_height) / 2.0f;

	preproc.scale_x = 1.0f / scale;
	preproc.scale_y = 1.0f / scale;
	preproc.pad_left = pad_left;
	preproc.pad_top = pad_top;

	// Letterboxシェーダー処理
	if (!InitializeLetterboxShader()) {
		// シェーダー初期化失敗時は元のテクスチャを返す
		preproc.input_tensor = input.texture;
		return preproc;
	}

	if (!PerformLetterboxTransform(input, preproc)) {
		// シェーダー処理失敗時は元のテクスチャを返す
		preproc.input_tensor = input.texture;
		return preproc;
	}

	return preproc;
}

bool VideoPipeline::InitializeLetterboxShader() {
	if (letterbox_vertex_shader_.Get() && letterbox_pixel_shader_.Get()) {
		return true; // 既に初期化済み
	}

	ID3D11Device* device = device_manager_.GetDevice();
	ID3D11DeviceContext* context = device_manager_.GetContext();
	if (!device || !context) return false;

	// ヘルパー: HLSLソースをランタイムコンパイル
	auto compileShader = [](const char* src, const char* entry, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& blob) -> HRESULT {
		Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
		HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, blob.ReleaseAndGetAddressOf(), errBlob.ReleaseAndGetAddressOf());
		if (FAILED(hr) && errBlob) {
			PipelineLogFmt("[InitializeLetterboxShader] Compile error (%s/%s): %s\n", entry, target, (char*)errBlob->GetBufferPointer());
		}
		return hr;
	};

	Microsoft::WRL::ComPtr<ID3DBlob> blob;
	HRESULT hr;

	// バーテックスシェーダーのコンパイルと作成
	hr = compileShader(letterbox_vertex_shader_src, "main", "vs_4_0", blob);
	if (FAILED(hr) || !blob) {
		PipelineLogFmt("[InitializeLetterboxShader] Failed to compile vertex shader. HRESULT=0x%08X\n", (unsigned)hr);
		return false;
	}
	hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, letterbox_vertex_shader_.ReleaseAndGetAddressOf());
	if (FAILED(hr) || !letterbox_vertex_shader_.Get()) {
		PipelineLogFmt("[InitializeLetterboxShader] Failed to create vertex shader. HRESULT=0x%08X\n", (unsigned)hr);
		return false;
	}

	// 入力レイアウトの作成
	D3D11_INPUT_ELEMENT_DESC input_layout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}
	};
	hr = device->CreateInputLayout(input_layout, ARRAYSIZE(input_layout), blob->GetBufferPointer(), blob->GetBufferSize(), letterbox_input_layout_.ReleaseAndGetAddressOf());
	if (FAILED(hr) || !letterbox_input_layout_.Get()) {
		PipelineLogFmt("[InitializeLetterboxShader] Failed to create input layout. HRESULT=0x%08X\n", (unsigned)hr);
		return false;
	}

	// ピクセルシェーダーのコンパイルと作成
	hr = compileShader(letterbox_pixel_shader_src, "main", "ps_4_0", blob);
	if (FAILED(hr) || !blob) {
		PipelineLogFmt("[InitializeLetterboxShader] Failed to compile pixel shader. HRESULT=0x%08X\n", (unsigned)hr);
		return false;
	}
	hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, letterbox_pixel_shader_.ReleaseAndGetAddressOf());
	if (FAILED(hr) || !letterbox_pixel_shader_.Get()) {
		PipelineLogFmt("[InitializeLetterboxShader] Failed to create pixel shader. HRESULT=0x%08X\n", (unsigned)hr);
		return false;
	}

	// サンプラー状態の作成
	D3D11_SAMPLER_DESC sampler_desc = {};
	sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sampler_desc.MinLOD = 0;
	sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
	hr = device->CreateSamplerState(&sampler_desc, letterbox_sampler_.ReleaseAndGetAddressOf());
	if (FAILED(hr) || !letterbox_sampler_.Get()) {
		std::cerr << "Failed to create sampler state. HRESULT: 0x" << std::hex << hr << std::endl;
		return false;
	}

	// 定数バッファの作成
	D3D11_BUFFER_DESC buffer_desc = {};
	buffer_desc.ByteWidth = sizeof(LetterboxConstantBuffer);
	buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
	buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = device->CreateBuffer(&buffer_desc, nullptr, letterbox_constant_buffer_.ReleaseAndGetAddressOf());
	if (FAILED(hr) || !letterbox_constant_buffer_.Get()) {
		std::cerr << "Failed to create constant buffer. HRESULT: 0x" << std::hex << hr << std::endl;
		return false;
	}

	return true;
}

bool VideoPipeline::PerformLetterboxTransform(const GpuFrame& input, PreProcessResult& preproc) {
	ID3D11DeviceContext* context = device_manager_.GetContext();
	if (!context || !input.texture.Get()) return false;

	// 出力テクスチャの作成
	D3D11_TEXTURE2D_DESC output_desc = {};
	output_desc.Width = config_.input_width;
	output_desc.Height = config_.input_height;
	output_desc.MipLevels = 1;
	output_desc.ArraySize = 1;
	output_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	output_desc.SampleDesc.Count = 1;
	output_desc.Usage = D3D11_USAGE_DEFAULT;
	output_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	output_desc.CPUAccessFlags = 0;
	output_desc.MiscFlags = 0;

	Microsoft::WRL::ComPtr<ID3D11Texture2D> output_texture;
	HRESULT hr = device_manager_.GetDevice()->CreateTexture2D(&output_desc, nullptr, output_texture.ReleaseAndGetAddressOf());
	if (FAILED(hr) || !output_texture.Get()) {
		std::cerr << "Failed to create output texture. HRESULT: 0x" << std::hex << hr << std::endl;
		return false;
	}

	// 入力/出力シェーダリソースビューの作成
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
	hr = device_manager_.GetDevice()->CreateShaderResourceView(
		input.texture.Get(), nullptr, srv.ReleaseAndGetAddressOf());
	if (FAILED(hr)) {
		std::cerr << "Failed to create source SRV. HRESULT: 0x" << std::hex << hr << std::endl;
		return false;
	}

	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> output_srv;
	hr = device_manager_.GetDevice()->CreateShaderResourceView(
		output_texture.Get(), nullptr, output_srv.ReleaseAndGetAddressOf());
	if (FAILED(hr)) {
		std::cerr << "Failed to create output SRV. HRESULT: 0x" << std::hex << hr << std::endl;
		return false;
	}

	// 出力テクスチャ用レンダーターゲットビューの作成
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output_rtv;
	hr = device_manager_.GetDevice()->CreateRenderTargetView(
		output_texture.Get(), nullptr, output_rtv.ReleaseAndGetAddressOf());
	if (FAILED(hr) || !output_rtv.Get()) {
		std::cerr << "Failed to create output RTV. HRESULT: 0x" << std::hex << hr << std::endl;
		return false;
	}

	// 定数バッファの更新
	LetterboxConstantBuffer constant_buffer{};
	// シェーダー側は「出力UV -> 入力UV」の変換なので、正方向スケールを渡す
	constant_buffer.scale_x = 1.0f / preproc.scale_x;
	constant_buffer.scale_y = 1.0f / preproc.scale_y;
	constant_buffer.pad_left = preproc.pad_left / config_.input_width;
	constant_buffer.pad_top = preproc.pad_top / config_.input_height;
	constant_buffer.input_width = static_cast<float>(input.width);
	constant_buffer.input_height = static_cast<float>(input.height);
	constant_buffer.output_width = static_cast<float>(config_.input_width);
	constant_buffer.output_height = static_cast<float>(config_.input_height);

	D3D11_MAPPED_SUBRESOURCE mapped_resource;
	hr = context->Map(letterbox_constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_resource);
	if (SUCCEEDED(hr)) {
		memcpy(mapped_resource.pData, &constant_buffer, sizeof(constant_buffer));
		context->Unmap(letterbox_constant_buffer_.Get(), 0);
	}

	// シェーダーのセット
	context->VSSetShader(letterbox_vertex_shader_.Get(), nullptr, 0);
	context->VSSetConstantBuffers(0, 1, letterbox_constant_buffer_.GetAddressOf());
	context->PSSetShader(letterbox_pixel_shader_.Get(), nullptr, 0);
	context->PSSetConstantBuffers(0, 1, letterbox_constant_buffer_.GetAddressOf());
	context->PSSetShaderResources(0, 1, srv.GetAddressOf());
	context->PSSetSamplers(0, 1, letterbox_sampler_.GetAddressOf());
	context->IASetInputLayout(letterbox_input_layout_.Get());

	// 全画面四角形の描画
	float quad_vertices[] = {
		-1.0f, 1.0f, 0.0f, 1.0f,   0.0f, 0.0f,
		 1.0f, 1.0f, 0.0f, 1.0f,   1.0f, 0.0f,
		 1.0f,-1.0f, 0.0f, 1.0f,   1.0f, 1.0f,
		-1.0f,-1.0f, 0.0f, 1.0f,   0.0f, 1.0f
	};

	Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer;
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = sizeof(quad_vertices);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	D3D11_SUBRESOURCE_DATA init_data = {};
	init_data.pSysMem = quad_vertices;
	hr = device_manager_.GetDevice()->CreateBuffer(&bd, &init_data, vertex_buffer.ReleaseAndGetAddressOf());
	if (SUCCEEDED(hr) && vertex_buffer.Get()) {
		UINT stride = sizeof(float) * 6;
		UINT offset = 0;
		context->IASetVertexBuffers(0, 1, vertex_buffer.GetAddressOf(), &stride, &offset);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

		// Rasterizer state
		Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state;
		D3D11_RASTERIZER_DESC rs_desc = {};
		rs_desc.FillMode = D3D11_FILL_SOLID;
		rs_desc.CullMode = D3D11_CULL_NONE;
		device_manager_.GetDevice()->CreateRasterizerState(&rs_desc, &rasterizer_state);
		context->RSSetState(rasterizer_state.Get());

		// Viewport
		D3D11_VIEWPORT vp = {};
		vp.Width = static_cast<float>(config_.input_width);
		vp.Height = static_cast<float>(config_.input_height);
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		vp.TopLeftX = 0.0f;
		vp.TopLeftY = 0.0f;
		context->RSSetViewports(1, &vp);

		// Render to output texture
		context->OMSetRenderTargets(1, output_rtv.GetAddressOf(), nullptr);
		context->Draw(4, 0);
		context->Flush(); // GPUコマンド完了を待機（DirectML同期のため）
	}

	// 結果を保存
	preproc.input_tensor = output_texture;
	preproc.texture = output_texture; // 後処理用

	return true;
}

bool VideoPipeline::LoadImageToTexture(
	const wchar_t* file_path,
	Microsoft::WRL::ComPtr<ID3D11Texture2D>& out_texture,
	uint32_t& out_width,
	uint32_t& out_height) {
	HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	bool shouldUninitialize = SUCCEEDED(coInitHr);
	if (coInitHr == RPC_E_CHANGED_MODE) {
		shouldUninitialize = false;
	}

	Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
	HRESULT hr = CoCreateInstance(
		CLSID_WICImagingFactory,
		nullptr,
		CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&wicFactory));
	if (FAILED(hr)) {
		if (shouldUninitialize) CoUninitialize();
		return false;
	}

	Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
	hr = wicFactory->CreateDecoderFromFilename(
		file_path,
		nullptr,
		GENERIC_READ,
		WICDecodeMetadataCacheOnLoad,
		&decoder);
	if (FAILED(hr)) {
		if (shouldUninitialize) CoUninitialize();
		return false;
	}

	Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
	hr = decoder->GetFrame(0, &frame);
	if (FAILED(hr)) {
		if (shouldUninitialize) CoUninitialize();
		return false;
	}

	UINT width = 0;
	UINT height = 0;
	hr = frame->GetSize(&width, &height);
	if (FAILED(hr) || width == 0 || height == 0) {
		if (shouldUninitialize) CoUninitialize();
		return false;
	}
	out_width = width;
	out_height = height;

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
		WICBitmapPaletteTypeMedianCut);
	if (FAILED(hr)) {
		if (shouldUninitialize) CoUninitialize();
		return false;
	}

	std::vector<BYTE> pixelData(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
	hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixelData.size()), pixelData.data());
	if (FAILED(hr)) {
		if (shouldUninitialize) CoUninitialize();
		return false;
	}

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

	hr = device_manager_.GetDevice()->CreateTexture2D(&texDesc, &initData, out_texture.ReleaseAndGetAddressOf());
	bool ok = SUCCEEDED(hr) && out_texture.Get() != nullptr;
	if (shouldUninitialize) CoUninitialize();
	return ok;
}

bool VideoPipeline::EnsureCopyrightWatermarkLoaded() {
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

	auto appendFromDir = [&appendCandidate](const std::wstring& dir) {
		if (dir.empty()) {
			return;
		}
		std::wstring path = dir;
		if (path.back() != L'\\' && path.back() != L'/') {
			path += L"\\";
		}
		path += L"C_SQUARE_ENIX.png";
		appendCandidate(path);
	};

	if (config_.copyright_image_path && config_.copyright_image_path[0] != L'\0') {
		appendCandidate(config_.copyright_image_path);
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

		PipelineLogFmt("[VideoPipeline] Trying watermark: %ls\n", imagePath.c_str());
		if (LoadImageToTexture(imagePath.c_str(), copyright_texture_, copyright_width_, copyright_height_)) {
			loaded = true;
			break;
		}
	}

	if (!loaded) {
		PipelineLog("[VideoPipeline] Failed to load watermark image from all candidates\n");
		return false;
	}

	HRESULT hr = device_manager_.GetDevice()->CreateShaderResourceView(
		copyright_texture_.Get(),
		nullptr,
		copyright_srv_.ReleaseAndGetAddressOf());
	if (FAILED(hr) || !copyright_srv_.Get()) {
		PipelineLogFmt("[VideoPipeline] Failed to create watermark SRV. HRESULT=0x%08X\n", (unsigned)hr);
		copyright_texture_.Reset();
		copyright_width_ = 0;
		copyright_height_ = 0;
		return false;
	}

	return true;
}

std::vector<Detection> VideoPipeline::PostProcessDetections(
	const std::vector<float>& output_data,
	const PreProcessResult& preproc,
	uint32_t original_width,
	uint32_t original_height
) {
	std::vector<Detection> detections;

	if (output_data.empty()) {
		std::cerr << "PostProcessDetections: Empty output data." << std::endl;
		return detections;
	}

	try {
		const float* raw_output = output_data.data();
		size_t num_floats = output_data.size();
		if (num_floats < 6) {
			std::cerr << "PostProcessDetections: Too few floats in tensor. Got " << num_floats << std::endl;
			return detections;
		}

		float score_threshold = config_.conf_threshold;
		int64_t detections_count = 0;
		int64_t per_det = 0;
		bool row_major = true; // [N, detections, 6]

		// PreviewPipelineと同様に、WinMLが返す出力shapeを優先して解釈
		auto output_names = winml_utils_.GetOutputNames();
		if (!output_names.empty()) {
			auto out_shape = winml_utils_.GetOutputShape(output_names[0]);
			if (out_shape.size() >= 3) {
				int64_t dim1 = out_shape[1];
				int64_t dim2 = out_shape[2];
				if (dim2 == 6) {
					detections_count = dim1;
					per_det = dim2;
					row_major = true;
				}
				else if (dim1 == 6) {
					detections_count = dim2;
					per_det = dim1;
					row_major = false; // [N, 6, detections]
				}
				else {
					detections_count = dim1;
					per_det = dim2;
					row_major = true;
				}
			}
		}

		// shapeが取れない場合のフォールバック
		if (detections_count <= 0 || per_det < 6) {
			per_det = 6;
			detections_count = static_cast<int64_t>(num_floats / 6);
			row_major = true;
		}

		for (int64_t i = 0; i < detections_count; ++i) {
			float x1_model = 0.0f;
			float y1_model = 0.0f;
			float x2_model = 0.0f;
			float y2_model = 0.0f;
			float score = 0.0f;
			int class_id = 0;

			if (row_major) {
				const float* det = raw_output + i * per_det;
				score = det[4];
				x1_model = det[0];
				y1_model = det[1];
				x2_model = det[2];
				y2_model = det[3];
				class_id = static_cast<int>(det[5]);
			}
			else {
				x1_model = raw_output[0 * detections_count + i];
				y1_model = raw_output[1 * detections_count + i];
				x2_model = raw_output[2 * detections_count + i];
				y2_model = raw_output[3 * detections_count + i];
				score = raw_output[4 * detections_count + i];
				class_id = static_cast<int>(raw_output[5 * detections_count + i]);
			}

			if (score <= score_threshold) continue;
			if (x1_model >= x2_model || y1_model >= y2_model) continue;

			// WinMLUtils::EvaluateFromTexture は PreviewPipelineと同じ
			// "左上揃えリサイズ（余白は黒）" 前処理なので、同じ逆変換を適用する
			const float resize_scale = std::max(
				static_cast<float>(original_width) / static_cast<float>(config_.input_width),
				static_cast<float>(original_height) / static_cast<float>(config_.input_height));

			float x1 = x1_model * resize_scale;
			float y1 = y1_model * resize_scale;
			float x2 = x2_model * resize_scale;
			float y2 = y2_model * resize_scale;

			x1 = std::clamp(x1, 0.0f, static_cast<float>(original_width));
			y1 = std::clamp(y1, 0.0f, static_cast<float>(original_height));
			x2 = std::clamp(x2, 0.0f, static_cast<float>(original_width));
			y2 = std::clamp(y2, 0.0f, static_cast<float>(original_height));

			int left = std::max(0, static_cast<int>(std::round(std::min(x1, x2))));
			int top = std::max(0, static_cast<int>(std::round(std::min(y1, y2))));
			int width = static_cast<int>(std::round(std::abs(x2 - x1)));
			int height = static_cast<int>(std::round(std::abs(y2 - y1)));
			if (width <= 0 || height <= 0) continue;

			Detection detection;
			detection.x1 = static_cast<float>(left);
			detection.y1 = static_cast<float>(top);
			detection.x2 = static_cast<float>(left + width);
			detection.y2 = static_cast<float>(top + height);
			detection.score = score;
			detection.class_id = class_id;
			detections.push_back(detection);
		}
	}
	catch (const winrt::hresult_error& ex) {
		std::cerr << "PostProcessDetections WinML error: " << winrt::to_string(ex.message()) << std::endl;
	}
	catch (const std::exception& ex) {
		std::cerr << "PostProcessDetections error: " << ex.what() << std::endl;
	}

	return detections;
}

// std::vector<Detection> VideoPipeline::NonMaxSuppression(std::vector<Detection>& detections, float iou_threshold) {
// 	std::vector<Detection> result;

// 	// 信頼度でソート
// 	std::sort(detections.begin(), detections.end(), 
// 		[](const Detection& a, const Detection& b) {
// 			return a.score > b.score;
// 		});

// 	std::vector<bool> suppressed(detections.size(), false);

// 	for (size_t i = 0; i < detections.size(); i++) {
// 		if (suppressed[i]) continue;

// 		result.push_back(detections[i]);

// 		for (size_t j = i + 1; j < detections.size(); j++) {
// 			if (suppressed[j]) continue;

// 			// IoUを計算
// 			float x1 = std::max(detections[i].x1, detections[j].x1);
// 			float y1 = std::max(detections[i].y1, detections[j].y1);
// 			float x2 = std::min(detections[i].x2, detections[j].x2);
// 			float y2 = std::min(detections[i].y2, detections[j].y2);

// 			float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
// 			float area_i = (detections[i].x2 - detections[i].x1) * (detections[i].y2 - detections[i].y1);
// 			float area_j = (detections[j].x2 - detections[j].x1) * (detections[j].y2 - detections[j].y1);
// 			float union_area = area_i + area_j - intersection;

// 			float iou = union_area > 0 ? intersection / union_area : 0.0f;

// 			if (iou > iou_threshold) {
// 				suppressed[j] = true;
// 			}
// 		}
// 	}

// 	return result;
// }

} // namespace WoLNamesBlackedOut::Core

// 注: C#からの呼び出し用エクスポート関数は dllmain.cpp に集約しました。