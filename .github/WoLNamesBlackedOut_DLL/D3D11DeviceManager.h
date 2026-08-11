#pragma once

// DirectX 11 ヘッダー
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <wrl/client.h> // Microsoft::WRL::ComPtr

// FFmpeg ヘッダー
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavcodec/avcodec.h>
}

#include <memory>
#include <mutex>
#include <string>
#include <vector>

// FFmpegの構造体はC言語由来のため、extern "C" で囲む必要がありますが、
// このヘッダーは.cpp内で使用することを想定しています。

#include "CoreTypes.h"

class D3D11DeviceManager {
public:
	D3D11DeviceManager();
	~D3D11DeviceManager();

	// コピーコンストラクタと代入演算子を削除（単一所有権のため）
	D3D11DeviceManager(const D3D11DeviceManager&) = delete;
	D3D11DeviceManager& operator=(const D3D11DeviceManager&) = delete;

	/**
	 * @brief D3D11デバイスの作成とFFmpeg用HWコンテキストの初期化
	 * @param device_type グラフィックデバイスタイプ (通常 D3D_DRIVER_TYPE_HARDWARE)
	 * @param adapter アダプターポインター (nullptrで既定のGPU)
	 * @return 初期化成功時 true
	 */
	bool Initialize(D3D_DRIVER_TYPE device_type = D3D_DRIVER_TYPE_HARDWARE, 
					IDXGIAdapter* adapter = nullptr);

	/**
	 * @brief リソースの解放
	 */
	void Release();

	/**
	 * @brief 共有ID3D11Deviceの取得
	 * @return ID3D11Deviceポインター (所有権は移らない)
	 */
	ID3D11Device* GetDevice() const;

	/**
	 * @brief 共有ID3D11DeviceContextの取得
	 * @return ID3D11DeviceContextポインター (所有権は移らない)
	 */
	ID3D11DeviceContext* GetContext() const;

	/**
	 * @brief FFmpeg用HWデバイスコンテキストの取得
	 * @return AVBufferRefポインター (参照カウントが増加しているため、
	 *         使用後は av_buffer_unref() で解放する必要がある場合があるが、
	 *         ここでは内部管理を簡略化するため参照返しのみの場合もある。
	 *         仕様書通り、デコーダ等に渡す場合は av_buffer_ref() して渡すこと。)
	 */
	AVBufferRef* GetHWDeviceContext() const;

	/**
	 * @brief マルチスレッド保護が有効かどうか
	 */
	bool IsMultithreadProtected() const { return multithread_protected_; }

	/**
	 * @brief エンコーダー用のHWフレームコンテキスト生成
	 * @return hw_frames_ctx (使用後は av_buffer_unref で解放)
	 */
	AVBufferRef* CreateHWFramesContext() const;

	/**
	 * @brief GPUベンダーの取得
	 * @return GPUベンダー (NVIDIA/Intel/AMD/Unknown)
	 */
	WoLNamesBlackedOut::Core::GpuVendor GetGpuVendor() const;

	/**
	 * @brief GPU名を取得
	 * @return GPU名
	 */
	std::string GetGpuName() const;

	/**
	 * @brief 利用可能なHWエンコーダーを自動判別
	 * @return 最適なエンコーダー種別
	 */
	WoLNamesBlackedOut::Core::HwEncoderType AutoDetectEncoder() const;

	/**
	 * @brief エンコーダー用hw_frames_ctxを生成（指定されたエンコーダータイプで）
	 * @param encoder_type エンコーダー種別
	 * @param width フレーム幅
	 * @param height フレーム高さ
	 * @return hw_frames_ctx (使用後は av_buffer_unref で解放)
	 */
	AVBufferRef* CreateEncoderFramesContext(WoLNamesBlackedOut::Core::HwEncoderType encoder_type, int width, int height) const;

	/**
	 * @brief D3D11VideoProcessorを初期化（色変換用）
	 * @param in_width 入力フレーム幅
	 * @param in_height 入力フレーム高さ
	 * @param out_width 出力フレーム幅（0でin_widthと同じ）
	 * @param out_height 出力フレーム高さ（0でin_heightと同じ）
	 * @return 成功時 true
	 */
	bool InitializeVideoProcessor(int in_width, int in_height, int out_width = 0, int out_height = 0);

	/**
	 * @brief D3D11VideoProcessorを解放
	 */
	void ReleaseVideoProcessor();

	/**
	 * @brief BGRAテクスチャをNV12テクスチャに変換（GPUハードウェア）
	 * @param bgra BGRA入力テクスチャ
	 * @param nv12 NV12出力テクスチャ
	 * @return 成功時 true
	 */
	bool ConvertBGRAToNV12(ID3D11Texture2D* bgra, ID3D11Texture2D* nv12);

	/**
	 * @brief NV12テクスチャをBGRAテクスチャに変換（GPUハードウェア）
	 * @param nv12 NV12入力テクスチャ
	 * @param bgra BGRA出力テクスチャ
	 * @param nv12_array_slice NV12入力テクスチャの配列スライス（FFmpeg D3D11VA向け）
	 * @return 成功時 true
	 */
	bool ConvertNV12ToBGRA(ID3D11Texture2D* nv12, ID3D11Texture2D* bgra, UINT nv12_array_slice = 0);

private:
	bool InitializeDecodeVideoProcessor(int in_width, int in_height, int out_width = 0, int out_height = 0);
	bool InitializeEncodeVideoProcessor(int in_width, int in_height, int out_width = 0, int out_height = 0);
	void ReleaseDecodeVideoProcessor();
	void ReleaseEncodeVideoProcessor();

	Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device_;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_context_;
	Microsoft::WRL::ComPtr<IUnknown> multithread_;
	Microsoft::WRL::ComPtr<IDXGIAdapter> dxgi_adapter_;

	AVBufferRef* hw_device_ctx_;

	bool multithread_protected_;
	WoLNamesBlackedOut::Core::GpuVendor gpu_vendor_;
	std::string gpu_name_;

	// D3D11 Video Processor for Decode thread (NV12 -> BGRA)
	Microsoft::WRL::ComPtr<ID3D11VideoDevice> decode_video_device_;
	Microsoft::WRL::ComPtr<ID3D11VideoContext> decode_video_context_;
	Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> decode_video_proc_enum_;
	Microsoft::WRL::ComPtr<ID3D11VideoProcessor> decode_video_processor_;
	int decode_vp_width_ = 0;
	int decode_vp_height_ = 0;
	std::mutex decode_video_processor_mutex_;

	// D3D11 Video Processor for Encode thread (BGRA -> NV12)
	Microsoft::WRL::ComPtr<ID3D11VideoDevice> encode_video_device_;
	Microsoft::WRL::ComPtr<ID3D11VideoContext> encode_video_context_;
	Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> encode_video_proc_enum_;
	Microsoft::WRL::ComPtr<ID3D11VideoProcessor> encode_video_processor_;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> encode_nv12_intermediate_rt_;
	UINT encode_nv12_intermediate_width_ = 0;
	UINT encode_nv12_intermediate_height_ = 0;
	DXGI_FORMAT encode_nv12_intermediate_format_ = DXGI_FORMAT_UNKNOWN;
	int encode_vp_width_ = 0;
	int encode_vp_height_ = 0;
	std::mutex encode_video_processor_mutex_;
};
