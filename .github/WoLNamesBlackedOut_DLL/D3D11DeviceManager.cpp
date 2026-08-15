#include "pch.h"
#include "D3D11DeviceManager.h"
#include "CoreTypes.h"
#include <iostream>
#include <exception>
#include <algorithm>

// DXGIアダプターからGPU情報を取得
static WoLNamesBlackedOut::Core::GpuVendor DetectGpuVendor(IDXGIAdapter* adapter) {
	if (!adapter) return WoLNamesBlackedOut::Core::GpuVendor::Unknown;

	DXGI_ADAPTER_DESC desc;
	if (FAILED(adapter->GetDesc(&desc))) {
		return WoLNamesBlackedOut::Core::GpuVendor::Unknown;
	}

	// wchar_t から UTF-8 へ変換（WideCharToMultiByte使用）
	int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
	std::string vendorLower;
	if (len > 0) {
		vendorLower.resize(len - 1);
		WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, &vendorLower[0], len, nullptr, nullptr);
		// 小文字に変換
		std::transform(vendorLower.begin(), vendorLower.end(), vendorLower.begin(), ::tolower);
	}

	if (vendorLower.find("nvidia") != std::string::npos ||
		vendorLower.find("nvdia") != std::string::npos) {
		return WoLNamesBlackedOut::Core::GpuVendor::NVIDIA;
	}
	else if (vendorLower.find("intel") != std::string::npos) {
		return WoLNamesBlackedOut::Core::GpuVendor::Intel;
	}
	else if (vendorLower.find("amd") != std::string::npos ||
			 vendorLower.find("advanced") != std::string::npos) {
		return WoLNamesBlackedOut::Core::GpuVendor::AMD;
	}

	return WoLNamesBlackedOut::Core::GpuVendor::Unknown;
}

static std::string GpuVendorToString(WoLNamesBlackedOut::Core::GpuVendor vendor) {
	switch (vendor) {
		case WoLNamesBlackedOut::Core::GpuVendor::NVIDIA: return "NVIDIA";
		case WoLNamesBlackedOut::Core::GpuVendor::Intel: return "Intel";
		case WoLNamesBlackedOut::Core::GpuVendor::AMD: return "AMD";
		default: return "Unknown";
	}
}

static bool IsVerboseVideoConvertLogEnabled() {
	static int cached = -1;
	if (cached < 0) {
		char value[8] = {};
		DWORD len = GetEnvironmentVariableA("WOL_VERBOSE_VIDEO_CONVERT_LOG", value, static_cast<DWORD>(sizeof(value)));
		cached = (len > 0 && value[0] != '0') ? 1 : 0;
	}
	return cached == 1;
}

static void VerboseVideoConvertLog(const char* msg) {
	if (IsVerboseVideoConvertLogEnabled()) {
		OutputDebugStringA(msg);
	}
}

D3D11DeviceManager::D3D11DeviceManager()
	: hw_device_ctx_(nullptr), multithread_protected_(false) {
}

D3D11DeviceManager::~D3D11DeviceManager() {
	Release();
}

bool D3D11DeviceManager::Initialize(D3D_DRIVER_TYPE device_type, IDXGIAdapter* adapter) {
	// すでに初期化済みの場合は解放してからやり直す
	if (d3d11_device_.Get() != nullptr) {
		Release();
	}

	UINT createFlags = 0;
#ifdef _DEBUG
	createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_3,
		D3D_FEATURE_LEVEL_9_2,
		D3D_FEATURE_LEVEL_9_1
	};

	HRESULT hr = D3D11CreateDevice(
		adapter,
		device_type,
		nullptr,
		createFlags,
		featureLevels,
		ARRAYSIZE(featureLevels),
		D3D11_SDK_VERSION,
		&d3d11_device_,
		nullptr,
		&d3d11_context_
	);

	if (FAILED(hr) || !d3d11_device_.Get() || !d3d11_context_.Get()) {
		std::cerr << "Failed to create D3D11 Device/Context. HRESULT: 0x" << std::hex << hr << std::endl;
		return false;
	}

	// GPU情報を取得
	dxgi_adapter_ = adapter;
	if (!dxgi_adapter_) {
		Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
		if (SUCCEEDED(d3d11_device_.As(&dxgiDevice)) && dxgiDevice.Get()) {
			Microsoft::WRL::ComPtr<IDXGIAdapter> resolvedAdapter;
			if (SUCCEEDED(dxgiDevice->GetAdapter(&resolvedAdapter)) && resolvedAdapter.Get()) {
				dxgi_adapter_ = resolvedAdapter;
			}
		}
	}

	gpu_vendor_ = DetectGpuVendor(dxgi_adapter_.Get());
	if (dxgi_adapter_.Get()) {
		DXGI_ADAPTER_DESC desc;
		if (SUCCEEDED(dxgi_adapter_->GetDesc(&desc))) {
			// wchar_t から UTF-8 へ変換（WideCharToMultiByte使用）
			int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
			if (len > 0) {
				gpu_name_.resize(len - 1);
				WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, &gpu_name_[0], len, nullptr, nullptr);
			}
		}
	}

	// 3.1 共通基盤: ID3D11Multithread の有効化
	// D3D11DeviceContext を複数スレッドから安全に使用するため、マルチスレッド保護を有効化
	Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
	if (SUCCEEDED(d3d11_device_.As(&multithread)) && multithread.Get()) {
		multithread->SetMultithreadProtected(TRUE);
		multithread_ = multithread;
		multithread_protected_ = true;
	}

	// 3.1 共通基盤: FFmpeg D3D11VA HWデバイスコンテキストの生成と共有
	// FFmpeg 6.0+ の推奨アプローチ：av_hwdevice_ctx_create + AVHWDeviceContext経由でのデバイス共有

	// 仕様書3.1準拠: D3D11VA用DeviceInfo構造体を確保
	AVBufferRef* temp_hw_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
	if (!temp_hw_ctx) {
		std::cerr << "av_hwdevice_ctx_alloc failed." << std::endl;
		Release();
		return false;
	}

	// 生成されたコンテキストからD3D11用情報へアクセスし、DeviceInfoをセット
	AVHWDeviceContext* dev_ctx = (AVHWDeviceContext*)temp_hw_ctx->data;
	if (!dev_ctx || !dev_ctx->hwctx) {
		std::cerr << "Failed to get AVHWDeviceContext or hwctx." << std::endl;
		av_buffer_unref(&temp_hw_ctx);
		Release();
		return false;
	}

	// D3D11VAデバイス情報へアクセス
	AVD3D11VADeviceContext* d3d11_ctx = (AVD3D11VADeviceContext*)dev_ctx->hwctx;
	if (d3d11_ctx) {
		d3d11_ctx->device = d3d11_device_.Get();
		d3d11_ctx->device_context = d3d11_context_.Get();
		// AddRef を呼び出してリファレンスを共有
		d3d11_device_->AddRef();
		d3d11_context_->AddRef();
	}

	// HWデバイスコンテキストを初期化（必須）
	int ret = av_hwdevice_ctx_init(temp_hw_ctx);
	if (ret < 0) {
		char err_buf[256] = {};
		av_strerror(ret, err_buf, sizeof(err_buf));
		std::cerr << "av_hwdevice_ctx_init failed: " << err_buf << std::endl;
		av_buffer_unref(&temp_hw_ctx);
		Release();
		return false;
	}

	// hw_device_ctxを保持（後でav_buffer_refして使用）
	hw_device_ctx_ = temp_hw_ctx;

	std::cout << "D3D11 Device initialized. GPU: " << (gpu_vendor_ == WoLNamesBlackedOut::Core::GpuVendor::NVIDIA ? "NVIDIA" : gpu_vendor_ == WoLNamesBlackedOut::Core::GpuVendor::AMD ? "AMD" : "Intel") << std::endl;

	return true;
}

void D3D11DeviceManager::Release() {
	ReleaseVideoProcessor();

	if (hw_device_ctx_) {
		av_buffer_unref(&hw_device_ctx_);
	}
	hw_device_ctx_ = nullptr;

	// COMオブジェクトは WRL::ComPtr が解放時に自動処理する
	// ただし、FFmpeg内部でAddRefされている場合は、FFmpeg側が解放するまで
	// デバイス自体は生存している必要がある場合がある。
	// このマネージャーがデバイスの「オーナー」であることを前提とする。
	d3d11_device_.Reset();
	d3d11_context_.Reset();
	multithread_.Reset();
	dxgi_adapter_.Reset();
	multithread_protected_ = false;
	gpu_vendor_ = WoLNamesBlackedOut::Core::GpuVendor::Unknown;
	gpu_name_.clear();
}

WoLNamesBlackedOut::Core::GpuVendor D3D11DeviceManager::GetGpuVendor() const {
	return gpu_vendor_;
}

std::string D3D11DeviceManager::GetGpuName() const {
	return gpu_name_;
}

WoLNamesBlackedOut::Core::HwEncoderType D3D11DeviceManager::AutoDetectEncoder() const {
	switch (gpu_vendor_) {
		case WoLNamesBlackedOut::Core::GpuVendor::NVIDIA:
			return WoLNamesBlackedOut::Core::HwEncoderType::NVENC;
		case WoLNamesBlackedOut::Core::GpuVendor::Intel:
			return WoLNamesBlackedOut::Core::HwEncoderType::QSV;
		case WoLNamesBlackedOut::Core::GpuVendor::AMD:
			return WoLNamesBlackedOut::Core::HwEncoderType::AMF;
		default:
			return WoLNamesBlackedOut::Core::HwEncoderType::None;
	}
}

ID3D11Device* D3D11DeviceManager::GetDevice() const {
	return d3d11_device_.Get();
}

ID3D11DeviceContext* D3D11DeviceManager::GetContext() const {
	return d3d11_context_.Get();
}

AVBufferRef* D3D11DeviceManager::CreateHWFramesContext() const {
	if (!hw_device_ctx_) {
		return nullptr;
	}

	// エンコーダー用のフレームコンテキストを生成
	// 仕様書に従い、デコーダーが持つ hw_device_ctx を src として使用
	AVBufferRef* hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx_);
	if (!hw_frames_ctx) {
		std::cerr << "av_hwframe_ctx_alloc failed." << std::endl;
		return nullptr;
	}

	// フレームプールの初期化（NV12形式を想定）
	int ret = av_hwframe_ctx_init(hw_frames_ctx);
	if (ret < 0) {
		std::cerr << "av_hwframe_ctx_init failed." << std::endl;
		av_buffer_unref(&hw_frames_ctx);
		return nullptr;
	}

	return hw_frames_ctx;
}

AVBufferRef* D3D11DeviceManager::CreateEncoderFramesContext(WoLNamesBlackedOut::Core::HwEncoderType encoder_type, int width, int height) const {
	if (!hw_device_ctx_ || width <= 0 || height <= 0) {
		return nullptr;
	}

	// エンコーダータイプに応じたHWデバイスタイプを取得
	enum AVHWDeviceType hw_device_type = AV_HWDEVICE_TYPE_NONE;

	switch (encoder_type) {
		case WoLNamesBlackedOut::Core::HwEncoderType::NVENC:
			hw_device_type = AV_HWDEVICE_TYPE_D3D11VA;
			break;
		case WoLNamesBlackedOut::Core::HwEncoderType::QSV:
			hw_device_type = AV_HWDEVICE_TYPE_QSV;
			break;
		case WoLNamesBlackedOut::Core::HwEncoderType::AMF:
			hw_device_type = AV_HWDEVICE_TYPE_VAAPI;
			break;
		default:
			hw_device_type = AV_HWDEVICE_TYPE_D3D11VA;
			break;
	}

	// D3D11VAベースのフレームコンテキストを生成（全GPU共通）
	AVBufferRef* hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx_);
	if (!hw_frames_ctx) {
		std::cerr << "av_hwframe_ctx_alloc failed." << std::endl;
		return nullptr;
	}

	// フレームフォーマットをD3D11に設定（GPUゼロコピー用）
	AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hw_frames_ctx->data;
	frames_ctx->format = AV_PIX_FMT_D3D11;
	frames_ctx->sw_format = AV_PIX_FMT_NV12;
	frames_ctx->width = width;
	frames_ctx->height = height;

	// フレームプールの初期化
	int ret = av_hwframe_ctx_init(hw_frames_ctx);
	if (ret < 0) {
		std::cerr << "av_hwframe_ctx_init failed." << std::endl;
		av_buffer_unref(&hw_frames_ctx);
		return nullptr;
	}

	return hw_frames_ctx;
}

AVBufferRef* D3D11DeviceManager::GetHWDeviceContext() const {
	return hw_device_ctx_;
}

bool D3D11DeviceManager::InitializeVideoProcessor(int in_width, int in_height, int out_width, int out_height) {
	return InitializeEncodeVideoProcessor(in_width, in_height, out_width, out_height);
}

bool D3D11DeviceManager::InitializeDecodeVideoProcessor(int in_width, int in_height, int out_width, int out_height) {
	if (out_width <= 0) out_width = in_width;
	if (out_height <= 0) out_height = in_height;

	if (decode_video_processor_.Get() && decode_vp_width_ == out_width && decode_vp_height_ == out_height) {
		return true; // 既に初期化済み
	}

	ReleaseDecodeVideoProcessor();

	HRESULT hr = d3d11_device_.As(&decode_video_device_);
	if (FAILED(hr) || !decode_video_device_.Get()) {
		std::cerr << "D3D11VideoDevice not supported." << std::endl;
		return false;
	}

	d3d11_context_->QueryInterface(IID_PPV_ARGS(&decode_video_context_));
	if (!decode_video_context_.Get()) {
		std::cerr << "D3D11VideoContext not supported." << std::endl;
		return false;
	}

	D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc = {};
	content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	content_desc.InputFrameRate.Numerator = 30;
	content_desc.InputFrameRate.Denominator = 1;
	content_desc.InputWidth = in_width;
	content_desc.InputHeight = in_height;
	content_desc.OutputFrameRate.Numerator = 30;
	content_desc.OutputFrameRate.Denominator = 1;
	content_desc.OutputWidth = out_width;
	content_desc.OutputHeight = out_height;
	content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

	hr = decode_video_device_->CreateVideoProcessorEnumerator(&content_desc, &decode_video_proc_enum_);
	if (FAILED(hr)) {
		std::cerr << "CreateVideoProcessorEnumerator failed." << std::endl;
		return false;
	}

	hr = decode_video_device_->CreateVideoProcessor(decode_video_proc_enum_.Get(), 0, &decode_video_processor_);
	if (FAILED(hr)) {
		std::cerr << "CreateVideoProcessor failed." << std::endl;
		return false;
	}

	decode_vp_width_ = out_width;
	decode_vp_height_ = out_height;
	return true;
}

bool D3D11DeviceManager::InitializeEncodeVideoProcessor(int in_width, int in_height, int out_width, int out_height) {
	if (out_width <= 0) out_width = in_width;
	if (out_height <= 0) out_height = in_height;

	if (encode_video_processor_.Get() && encode_vp_width_ == out_width && encode_vp_height_ == out_height) {
		return true; // 既に初期化済み
	}

	ReleaseEncodeVideoProcessor();

	HRESULT hr = d3d11_device_.As(&encode_video_device_);
	if (FAILED(hr) || !encode_video_device_.Get()) {
		std::cerr << "D3D11VideoDevice not supported." << std::endl;
		return false;
	}

	d3d11_context_->QueryInterface(IID_PPV_ARGS(&encode_video_context_));
	if (!encode_video_context_.Get()) {
		std::cerr << "D3D11VideoContext not supported." << std::endl;
		return false;
	}

	D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc = {};
	content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	content_desc.InputFrameRate.Numerator = 30;
	content_desc.InputFrameRate.Denominator = 1;
	content_desc.InputWidth = in_width;
	content_desc.InputHeight = in_height;
	content_desc.OutputFrameRate.Numerator = 30;
	content_desc.OutputFrameRate.Denominator = 1;
	content_desc.OutputWidth = out_width;
	content_desc.OutputHeight = out_height;
	content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

	hr = encode_video_device_->CreateVideoProcessorEnumerator(&content_desc, &encode_video_proc_enum_);
	if (FAILED(hr)) {
		std::cerr << "CreateVideoProcessorEnumerator failed." << std::endl;
		return false;
	}

	hr = encode_video_device_->CreateVideoProcessor(encode_video_proc_enum_.Get(), 0, &encode_video_processor_);
	if (FAILED(hr)) {
		std::cerr << "CreateVideoProcessor failed." << std::endl;
		return false;
	}

	encode_vp_width_ = out_width;
	encode_vp_height_ = out_height;
	return true;
}

void D3D11DeviceManager::ReleaseVideoProcessor() {
	ReleaseDecodeVideoProcessor();
	ReleaseEncodeVideoProcessor();
}

void D3D11DeviceManager::ReleaseDecodeVideoProcessor() {
	decode_video_processor_.Reset();
	decode_video_proc_enum_.Reset();
	decode_video_context_.Reset();
	decode_video_device_.Reset();
	decode_vp_width_ = 0;
	decode_vp_height_ = 0;
}

void D3D11DeviceManager::ReleaseEncodeVideoProcessor() {
	encode_video_processor_.Reset();
	encode_video_proc_enum_.Reset();
	encode_video_context_.Reset();
	encode_video_device_.Reset();
	encode_nv12_intermediate_rt_.Reset();
	encode_nv12_intermediate_width_ = 0;
	encode_nv12_intermediate_height_ = 0;
	encode_nv12_intermediate_format_ = DXGI_FORMAT_UNKNOWN;
	encode_vp_width_ = 0;
	encode_vp_height_ = 0;
}

bool D3D11DeviceManager::ConvertBGRAToNV12(ID3D11Texture2D* bgra, ID3D11Texture2D* nv12) {
	std::lock_guard<std::mutex> lock(encode_video_processor_mutex_);

	if (!bgra || !nv12) {
		OutputDebugStringA("[ConvertBGRAToNV12] NULL texture\n");
		return false;
	}

	D3D11_TEXTURE2D_DESC bgra_desc = {};
	bgra->GetDesc(&bgra_desc);
	D3D11_TEXTURE2D_DESC nv12_desc = {};
	nv12->GetDesc(&nv12_desc);

	char dbg[512];
	snprintf(dbg, sizeof(dbg), "[ConvertBGRAToNV12] BGRA: %ux%u fmt=%u bind=0x%X usage=%u | NV12: %ux%u fmt=%u bind=0x%X usage=%u\n",
		bgra_desc.Width, bgra_desc.Height, bgra_desc.Format, bgra_desc.BindFlags, bgra_desc.Usage,
		nv12_desc.Width, nv12_desc.Height, nv12_desc.Format, nv12_desc.BindFlags, nv12_desc.Usage);
	VerboseVideoConvertLog(dbg);

	if (!InitializeEncodeVideoProcessor(bgra_desc.Width, bgra_desc.Height, nv12_desc.Width, nv12_desc.Height)) {
		OutputDebugStringA("[ConvertBGRAToNV12] InitializeVideoProcessor failed\n");
		return false;
	}
	if (!encode_video_device_.Get() || !encode_video_context_.Get() || !encode_video_proc_enum_.Get() || !encode_video_processor_.Get()) {
		OutputDebugStringA("[ConvertBGRAToNV12] VideoProcessor resources are not ready\n");
		return false;
	}

	// VideoProcessorInputViewを作成
	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc = {};
	input_view_desc.FourCC = 0;
	input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	input_view_desc.Texture2D.MipSlice = 0;
	input_view_desc.Texture2D.ArraySlice = 0;

	Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
	HRESULT hr = encode_video_device_->CreateVideoProcessorInputView(bgra, encode_video_proc_enum_.Get(), &input_view_desc, &input_view);
	if (FAILED(hr)) {
		snprintf(dbg, sizeof(dbg), "[ConvertBGRAToNV12] CreateVideoProcessorInputView failed: 0x%08X\n", hr);
		OutputDebugStringA(dbg);
		return false;
	}
	VerboseVideoConvertLog("[ConvertBGRAToNV12] InputView created OK\n");

	// VideoProcessorOutputViewはD3D11_BIND_RENDER_TARGETが必要。
	// FFmpegのav_hwframe_get_bufferテクスチャはこのフラグを持たないため、
	// 中間レンダーターゲットテクスチャへ書いてからCopyResourceする。
	bool need_recreate_intermediate =
		!encode_nv12_intermediate_rt_.Get() ||
		encode_nv12_intermediate_width_ != nv12_desc.Width ||
		encode_nv12_intermediate_height_ != nv12_desc.Height ||
		encode_nv12_intermediate_format_ != nv12_desc.Format;

	if (need_recreate_intermediate) {
		D3D11_TEXTURE2D_DESC rt_desc = nv12_desc;
		rt_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		rt_desc.Usage = D3D11_USAGE_DEFAULT;
		rt_desc.CPUAccessFlags = 0;
		hr = d3d11_device_->CreateTexture2D(&rt_desc, nullptr, encode_nv12_intermediate_rt_.ReleaseAndGetAddressOf());
		if (FAILED(hr) || !encode_nv12_intermediate_rt_.Get()) {
			snprintf(dbg, sizeof(dbg), "[ConvertBGRAToNV12] Create intermediate RT failed: 0x%08X\n", hr);
			OutputDebugStringA(dbg);
			return false;
		}
		encode_nv12_intermediate_width_ = nv12_desc.Width;
		encode_nv12_intermediate_height_ = nv12_desc.Height;
		encode_nv12_intermediate_format_ = nv12_desc.Format;
		snprintf(dbg, sizeof(dbg), "[ConvertBGRAToNV12] Intermediate RT (re)created: fmt=%u bind=0x%X\n", rt_desc.Format, rt_desc.BindFlags);
		VerboseVideoConvertLog(dbg);
	}

	// VideoProcessorOutputViewを中間テクスチャに対して作成
	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc = {};
	output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	output_view_desc.Texture2D.MipSlice = 0;

	Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view;
	hr = encode_video_device_->CreateVideoProcessorOutputView(encode_nv12_intermediate_rt_.Get(), encode_video_proc_enum_.Get(), &output_view_desc, &output_view);
	if (FAILED(hr)) {
		snprintf(dbg, sizeof(dbg), "[ConvertBGRAToNV12] CreateVideoProcessorOutputView failed: 0x%08X\n", hr);
		OutputDebugStringA(dbg);
		return false;
	}
	VerboseVideoConvertLog("[ConvertBGRAToNV12] OutputView created OK\n");

	// VideoProcessorStream設定
	D3D11_VIDEO_PROCESSOR_STREAM stream = {};
	stream.Enable = TRUE;
	stream.pInputSurface = input_view.Get();

	// 入力ソース矩形は常に入力全面を使用する。
	// 出力サイズが異なる場合はDestRect/TargetRect側で等比スケーリングされる。
	// ここで入力を出力サイズへ切り詰めると、ForX + crop時に「さらにクロップ」された見え方になる。
	RECT src_rect = { 0, 0, static_cast<LONG>(bgra_desc.Width), static_cast<LONG>(bgra_desc.Height) };
	encode_video_context_->VideoProcessorSetStreamSourceRect(encode_video_processor_.Get(), 0, TRUE, &src_rect);

	// ストリーム出力先矩形を明示（暗黙のアスペクト補正/パディング回避）
	RECT stream_dest_rect = { 0, 0, static_cast<LONG>(nv12_desc.Width), static_cast<LONG>(nv12_desc.Height) };
	encode_video_context_->VideoProcessorSetStreamDestRect(encode_video_processor_.Get(), 0, TRUE, &stream_dest_rect);

	// 出力ターゲット矩形を設定（NV12テクスチャのサイズ）
	RECT dest_rect = { 0, 0, static_cast<LONG>(nv12_desc.Width), static_cast<LONG>(nv12_desc.Height) };
	encode_video_context_->VideoProcessorSetOutputTargetRect(encode_video_processor_.Get(), TRUE, &dest_rect);

	// 入力色空間設定（sRGB Full Range）
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_cs = {};
	input_cs.Usage = 0;
	input_cs.RGB_Range = 1;       // full range
	input_cs.YCbCr_Matrix = 1;    // BT.709
	input_cs.YCbCr_xvYCC = 0;
	input_cs.Nominal_Range = 0;   // 0-255
	encode_video_context_->VideoProcessorSetStreamColorSpace(encode_video_processor_.Get(), 0, &input_cs);

	// 出力色空間設定（BT.709 Limited Range）
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_cs = {};
	output_cs.Usage = 0;
	output_cs.RGB_Range = 0;      // limited range
	output_cs.YCbCr_Matrix = 1;   // BT.709
	output_cs.YCbCr_xvYCC = 0;
	output_cs.Nominal_Range = 1;  // 16-235
	encode_video_context_->VideoProcessorSetOutputColorSpace(encode_video_processor_.Get(), &output_cs);

	// 色変換実行（中間テクスチャに書き込み）
	hr = encode_video_context_->VideoProcessorBlt(encode_video_processor_.Get(), output_view.Get(), 0, 1, &stream);
	if (FAILED(hr)) {
		snprintf(dbg, sizeof(dbg), "[ConvertBGRAToNV12] VideoProcessorBlt failed: 0x%08X\n", hr);
		OutputDebugStringA(dbg);
		return false;
	}
	VerboseVideoConvertLog("[ConvertBGRAToNV12] VideoProcessorBlt OK\n");

	// 中間テクスチャから最終テクスチャへコピー
	d3d11_context_->CopyResource(nv12, encode_nv12_intermediate_rt_.Get());
	VerboseVideoConvertLog("[ConvertBGRAToNV12] CopyResource OK\n");

	return true;
}

bool D3D11DeviceManager::ConvertNV12ToBGRA(ID3D11Texture2D* nv12, ID3D11Texture2D* bgra, UINT nv12_array_slice) {
	std::lock_guard<std::mutex> lock(decode_video_processor_mutex_);

	if (!nv12 || !bgra) {
		OutputDebugStringA("[ConvertNV12ToBGRA] NULL texture\n");
		return false;
	}

	D3D11_TEXTURE2D_DESC nv12_desc = {};
	nv12->GetDesc(&nv12_desc);
	D3D11_TEXTURE2D_DESC bgra_desc = {};
	bgra->GetDesc(&bgra_desc);

	char dbg[512];
	snprintf(dbg, sizeof(dbg), "[ConvertNV12ToBGRA] NV12: %ux%u fmt=%u bind=0x%X | BGRA: %ux%u fmt=%u bind=0x%X\n",
		nv12_desc.Width, nv12_desc.Height, nv12_desc.Format, nv12_desc.BindFlags,
		bgra_desc.Width, bgra_desc.Height, bgra_desc.Format, bgra_desc.BindFlags);
	VerboseVideoConvertLog(dbg);

	if (!InitializeDecodeVideoProcessor(nv12_desc.Width, nv12_desc.Height, bgra_desc.Width, bgra_desc.Height)) {
		OutputDebugStringA("[ConvertNV12ToBGRA] InitializeVideoProcessor failed\n");
		return false;
	}
	if (!decode_video_device_.Get() || !decode_video_context_.Get() || !decode_video_proc_enum_.Get() || !decode_video_processor_.Get()) {
		OutputDebugStringA("[ConvertNV12ToBGRA] VideoProcessor resources are not ready\n");
		return false;
	}

	// VideoProcessorInputViewをNV12テクスチャから作成
	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc = {};
	input_view_desc.FourCC = 0; // 自動フォーマット検出
	input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	input_view_desc.Texture2D.MipSlice = 0;
	input_view_desc.Texture2D.ArraySlice = nv12_array_slice;

	Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
	HRESULT hr = decode_video_device_->CreateVideoProcessorInputView(nv12, decode_video_proc_enum_.Get(), &input_view_desc, &input_view);
	if (FAILED(hr)) {
		snprintf(dbg, sizeof(dbg), "[ConvertNV12ToBGRA] CreateVideoProcessorInputView failed: 0x%08X\n", hr);
		OutputDebugStringA(dbg);
		return false;
	}

	// VideoProcessorOutputViewをBGRAテクスチャに対して作成
	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc = {};
	output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	output_view_desc.Texture2D.MipSlice = 0;

	Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view;
	hr = decode_video_device_->CreateVideoProcessorOutputView(bgra, decode_video_proc_enum_.Get(), &output_view_desc, &output_view);
	if (FAILED(hr)) {
		snprintf(dbg, sizeof(dbg), "[ConvertNV12ToBGRA] CreateVideoProcessorOutputView failed: 0x%08X\n", hr);
		OutputDebugStringA(dbg);
		return false;
	}

	// VideoProcessorStream設定
	D3D11_VIDEO_PROCESSOR_STREAM stream = {};
	stream.Enable = TRUE;
	stream.pInputSurface = input_view.Get();

	// 入力ソース矩形を設定。
	// 1088 などのパディング行が含まれるケースでは、出力サイズに合わせてクロップし
	// 余剰領域のスケーリング混入を避ける。
	LONG src_w = static_cast<LONG>(nv12_desc.Width);
	LONG src_h = static_cast<LONG>(nv12_desc.Height);
	if (nv12_desc.Width >= bgra_desc.Width) {
		src_w = static_cast<LONG>(bgra_desc.Width);
	}
	if (nv12_desc.Height >= bgra_desc.Height) {
		src_h = static_cast<LONG>(bgra_desc.Height);
	}
	RECT src_rect = { 0, 0, src_w, src_h };
	decode_video_context_->VideoProcessorSetStreamSourceRect(decode_video_processor_.Get(), 0, TRUE, &src_rect);

	// ストリーム出力先矩形を明示（暗黙のアスペクト補正/パディング回避）
	RECT stream_dest_rect = { 0, 0, static_cast<LONG>(bgra_desc.Width), static_cast<LONG>(bgra_desc.Height) };
	decode_video_context_->VideoProcessorSetStreamDestRect(decode_video_processor_.Get(), 0, TRUE, &stream_dest_rect);

	// 出力ターゲット矩形を設定（BGRAテクスチャのサイズにクロップ/スケール）
	RECT dest_rect = { 0, 0, static_cast<LONG>(bgra_desc.Width), static_cast<LONG>(bgra_desc.Height) };
	decode_video_context_->VideoProcessorSetOutputTargetRect(decode_video_processor_.Get(), TRUE, &dest_rect);

	// 入力色空間設定（BT.709 Limited Range、標準的なビデオ）
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_cs = {};
	input_cs.Usage = 0;           // playback
	input_cs.RGB_Range = 0;       // limited range
	input_cs.YCbCr_Matrix = 1;    // BT.709
	input_cs.YCbCr_xvYCC = 0;
	input_cs.Nominal_Range = 1;   // 16-235
	decode_video_context_->VideoProcessorSetStreamColorSpace(decode_video_processor_.Get(), 0, &input_cs);

	// 出力色空間設定（sRGB Full Range）
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_cs = {};
	output_cs.Usage = 0;
	output_cs.RGB_Range = 1;      // full range
	output_cs.YCbCr_Matrix = 1;   // BT.709
	output_cs.YCbCr_xvYCC = 0;
	output_cs.Nominal_Range = 0;  // 0-255
	decode_video_context_->VideoProcessorSetOutputColorSpace(decode_video_processor_.Get(), &output_cs);

	// 色変換実行
	hr = decode_video_context_->VideoProcessorBlt(decode_video_processor_.Get(), output_view.Get(), 0, 1, &stream);
	if (FAILED(hr)) {
		snprintf(dbg, sizeof(dbg), "[ConvertNV12ToBGRA] VideoProcessorBlt failed: 0x%08X\n", hr);
		OutputDebugStringA(dbg);
		return false;
	}

	VerboseVideoConvertLog("[ConvertNV12ToBGRA] VideoProcessorBlt OK\n");
	return true;
}
