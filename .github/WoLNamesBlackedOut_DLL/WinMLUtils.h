#pragma once

// Windows Runtime API for DirectX interop
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Graphics.DirectX.h>

// WinML 2.0 / ONNX Runtime headers (from Microsoft.Windows.AI.MachineLearning NuGet package)
#include <winml/onnxruntime_cxx_api.h>

// WinML EP Catalog (system registered execution providers)
#include <WinMLEpCatalog.h>

// DirectX 11
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

// FFmpeg
extern "C" {
#include <libavutil/hwcontext.h>
}

#include <memory>
#include <string>
#include <vector>

#include "CoreTypes.h"
#include "PreprocessShader.h"

namespace WoLNamesBlackedOut::Core {

/**
 * @brief WinML 2.0 (ONNX Runtime)推論用のユーティリティクラス
 * IDXGISurface ↔ IDirect3DSurface変換、ONNXモデル読み込み、推論実行を管理
 */
class WinMLUtils {
public:
	WinMLUtils();
	~WinMLUtils();

	// コピーコンストラクタと代入演算子を削除
	WinMLUtils(const WinMLUtils&) = delete;
	WinMLUtils& operator=(const WinMLUtils&) = delete;

	/**
	 * @brief D3D11デバイスとコンテキストを設定
	 * @param device D3D11デバイス
	 * @param context D3D11デバイスコンテキスト
	 */
	void SetD3D11Device(ID3D11Device* device, ID3D11DeviceContext* context);

	/**
	 * @brief GPUベンダーを設定（EP選択に使用）
	 * @param vendor GPUベンダー
	 */
	void SetGpuVendor(GpuVendor vendor) { gpu_vendor_ = vendor; }

	/**
	 * @brief COM初期化（RoInitialize）
	 * @return 成功時 true
	 */
	bool Initialize();

	/**
	 * @brief COM解放（RoUninitialize）
	 */
	void Uninitialize();

	/**
	 * @brief ONNXモデルをロード
	 * @param model_path ONNXモデルファイルパス
	 * @return 成功時 true
	 */
	bool LoadModel(const wchar_t* model_path);

	/**
	 * @brief モデルがロードされているか
	 * @return セッション有効 true
	 */
	bool IsModelLoaded() const { return session_ != nullptr; }

	/**
	 * @brief WinML EPカタログに登録された全実行プロバイダーをログ出力
	 */
	void LogEpCatalog() const;

	/**
	 * @brief 指定したEPがNotPresent/NotReadyならダウンロード・インストールし、ONNX Runtimeに登録
	 * @param provider_name EP名（カタログ名と正確に一致させること）
	 * @return ReadyになりONNX Runtime登録できたらtrue
	 */
	bool EnsureEpReady(const char* provider_name);

	/**
	 * @brief ID3D11Texture2Dから推論用入力テンソルを作成
	 * @param texture D3D11テクスチャ
	 * @param width テクスチャ幅
	 * @param height テクスチャ高さ
	 * @return ONNX RuntimeのMemoryInfo（成功時有効）
	 */
	Ort::MemoryInfo CreateInputMemoryInfo(ID3D11Texture2D* texture, uint32_t width, uint32_t height);

	/**
	 * @brief 推論を実行（float配列入力用）
	 * @param input_name 入力名
	 * @param input_tensor_data テンソルデータ（float配列）
	 * @param data_size データサイズ
	 * @param input_shape テンソル形状
	 * @return 出力テンソルデータ（floatベクトル）
	 */
	std::vector<float> Evaluate(
		const std::string& input_name,
		const float* input_tensor_data,
		size_t data_size,
		const std::vector<int64_t>& input_shape
	);

	/**
	 * @brief 推論を実行（ID3D11Texture2D入力用）
	 * @param input_name 入力名
	 * @param texture D3D11テクスチャ
	 * @param width テクスチャ幅
	 * @param height テクスチャ高さ
	 * @return 出力テンソルデータ（floatベクトル）
	 */
	std::vector<float> EvaluateFromTexture(
		const std::string& input_name,
		ID3D11Texture2D* texture,
		uint32_t width,
		uint32_t height
	);

	/**
	 * @brief モデルの入力名一覧を取得（モデルメタデータから）
	 * @return 入力名リスト
	 */
	std::vector<std::string> GetInputNames() const { return input_names_; }

	/**
	 * @brief モデルの出力名一覧を取得（モデルメタデータから）
	 * @return 出力名リスト
	 */
	std::vector<std::string> GetOutputNames() const { return output_names_; }

	/**
	 * @brief モデルの入力形状を取得
	 * @param input_name 入力名
	 * @return 入力形状（dimensions）
	 */
	std::vector<int64_t> GetInputShape(const std::string& input_name) const;

	/**
	 * @brief モデルの出力形状を取得
	 * @param output_name 出力名
	 * @return 出力形状（dimensions）
	 */
	std::vector<int64_t> GetOutputShape(const std::string& output_name) const;

	/**
	 * @brief IoBindingを初期化（推論メモリの事前確保・再利用）
	 * @return 成功時 true
	 */
	bool InitializeIoBinding();

	/**
	 * @brief IoBindingを解放
	 */
	void ReleaseIoBinding();

private:
	Microsoft::WRL::ComPtr<ID3D11Device> device_;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

	// WinML 2.0: ONNX Runtime セッション
	mutable std::shared_ptr<Ort::Session> session_;
	std::shared_ptr<Ort::Env> env_;
	std::vector<std::string> input_names_;
	std::vector<std::string> output_names_;
	bool initialized_; // COM初期化状態
	GpuVendor gpu_vendor_ = GpuVendor::Unknown; // GPUベンダー（EP選択に使用）

	// GPU前処理シェーダー（Compute Shaderでリサイズ+レターボックス+正規化+NCHW変換）
	std::unique_ptr<PreprocessShader> preprocess_shader_;

	// IoBinding: 推論入出力メモリの事前確保・再利用（オーバーヘッド削減）
	std::unique_ptr<Ort::IoBinding> io_binding_;
	std::vector<float> input_buffer_;
	std::vector<int64_t> input_shape_;
	bool io_binding_initialized_ = false;

	// TensorRTエンジンキャッシュ設定
	std::wstring trt_cache_path_;
	bool trt_cache_enabled_ = false;

	/**
	 * @brief ID3D11Texture2DをIDXGISurfaceに変換
	 */
	Microsoft::WRL::ComPtr<IDXGISurface> TextureToDXGISurface(ID3D11Texture2D* texture);

	/**
	 * @brief IDXGISurfaceをIDirect3DSurfaceに変換（WinML 2.0対応）
	 */
	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface DXGISurfaceToDirect3DSurface(
		IDXGISurface* dxgi_surface
	);

	/**
	 * @brief IDXGISurfaceをVideoFrameに変換（WinML 2.0対応）
	 */
	winrt::Windows::Media::VideoFrame DXGISurfaceToVideoFrame(IDXGISurface* dxgi_surface);
};

} // namespace WoLNamesBlackedOut::Core
