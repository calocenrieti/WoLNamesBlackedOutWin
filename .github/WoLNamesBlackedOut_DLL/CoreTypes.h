#pragma once

// DirectX 11 ヘッダー
#include <d3d11.h>
#include <dxgi.h>

// Windows Runtime API for WinML 2.0
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Microsoft.Windows.AI.MachineLearning.h>
#include <winrt/Windows.Media.h>

#include <wrl/client.h>
#include <winrt/base.h>

// FFmpeg types (Assuming they are available from include paths)
extern "C" {
	struct AVBufferRef;
	struct AVFrame;
}

// C#からの呼び出し用構造体（RectInfo, ColorInfo）
// C#側: public struct RectInfo { public int x, y, width, height; }
// C#側: [StructLayout(LayoutKind.Sequential)] public struct ColorInfo { public byte r, g, b; }

struct RectInfo {
    int x;
    int y;
    int width;
    int height;

    RectInfo() : x(0), y(0), width(0), height(0) {}
    RectInfo(int _x, int _y, int _w, int _h) : x(_x), y(_y), width(_w), height(_h) {}
};

struct ColorInfo {
    uint8_t r;
    uint8_t g;
    uint8_t b;

    ColorInfo() : r(0), g(0), b(0) {}
    ColorInfo(uint8_t _r, uint8_t _g, uint8_t _b) : r(_r), g(_g), b(_b) {}
};

// C#側への状態通知コールバック（EPダウンロード・コンパイル進捗用）
// C#側: [UnmanagedFunctionPointer(CallingConvention.StdCall)]
//        public delegate void StatusCallback([MarshalAs(UnmanagedType.LPUTF8Str)] string message);
typedef void (__stdcall* StatusCallback)(const char* message);

namespace WoLNamesBlackedOut::Core {

	/**
	 * @brief GPU上のフレーム情報を保持する構造体
	 */
	struct GpuFrame {
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture; // Direct3D 11 テクスチャ (RGBA or NV12)
		uint32_t width;
		uint32_t height;
		int64_t pts;       // Presentation timestamp
		AVBufferRef* ref;  // FFmpeg/HW関係の参照カウント用

		GpuFrame() : texture(nullptr), width(0), height(0), pts(0), ref(nullptr) {}
	};

	/**
	 * @brief 推論結果を表す構造体 (NMS済み、座標はVideo解像度への逆変換前)
	 */
	struct Detection {
		int class_id;
		float score;
		float x1, y1, x2, y2; // Normalized [0, 1] or pixel coordinates
	};

	/**
	 * @brief 前処理結果（Letterbox情報を含む）
	 */
	struct PreProcessResult {
		Microsoft::WRL::ComPtr<ID3D11Texture2D> input_tensor; // AIモデルへの入力用テクスチャ
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;      // 後処理用出力テクスチャ
		float scale_x;   // 座標逆変換用: 原本 = x * scale + offset
		float scale_y;
		float pad_left;
		float pad_top;
	};

	/**
	 * @brief GPUベンダー種別
	 */
	enum class GpuVendor : int {
		Unknown = 0,
		NVIDIA = 1,
		Intel = 2,
		AMD = 3
	};

	/**
	 * @brief FFmpeg HWエンコーダー種別
	 */
	enum class HwEncoderType {
		None,        // HWエンコード無効
		NVENC,       // NVIDIA
		QSV,         // Intel Quick Sync
		AMF,         // AMD
		Auto         // 自動選択
	};

	/**
	 * @brief マスク種類
	 */
	enum class MaskType : int {
		Inpaint = 0,
		Mosaic = 1,
		Blur = 2,
		RectFill = 3,
		No_Inference = 4
	};

	/**
	 * @brief パイプライン設定
	 */
	struct PipelineConfig {
		// 入出力
		const wchar_t* input_path;
		const wchar_t* output_path;

		// AIモデル
		const wchar_t* model_path;       // ONNXモデルファイルパス
		const wchar_t* labels_path;      // ラベルファイルパス（オプション）

		// 推論設定
		int input_width;
		int input_height;
		float conf_threshold;
		float iou_threshold;

		// エンコード設定
		HwEncoderType encoder_type;
		int bitrate;
		int fps;
		bool for_x;  // trueの場合はH.264エンコーダを使用（X/Twitter等向け）
		bool disable_audio; // trueの場合は音声ストリームを出力しない
		double trim_start_seconds; // 処理開始秒（0以上）
		double trim_end_seconds;   // 処理終了秒（0以下で無効）

		// マスク設定（検出箇所）
		MaskType blacked_type;
		int blackedout_param;
		// マスク色（単色塗りつぶし時）
		ColorInfo name_color;
		ColorInfo fixframe_color;

		// 固定矩形マスク設定
		MaskType fixmask_type;
		int fixmask_param;

		// 固定矩形データ（InitializePipelineからコピー）
		int fixed_rect_count;
		RectInfo fixed_rects[64];

		// 透かし設定
		bool enable_copyright;
		const wchar_t* copyright_image_path;
		int copyright_offset_x;  // X position offset for copyright (right-aligned base)
		int copyright_offset_y;  // Y position offset for copyright (bottom-aligned base)
		float copyright_scale;   // Copyright scale (1.0 = original)

		// 名前除外設定（ByteTrack + OCR）
		bool exclude_by_name_enabled;
		int ocr_expand_pixels;          // 0-5
		int ocr_max_rois_per_frame;     // 1-32
		float text_similarity_threshold; // 0.50f - 1.00f
		const wchar_t* ocr_model_path;
		const wchar_t* ocr_dict_path;
		const wchar_t* mask_exclude_text_csv;

		// デフォルトコンストラクタ
		PipelineConfig()
			: input_path(nullptr), output_path(nullptr), model_path(nullptr), labels_path(nullptr),
			  input_width(640), input_height(640), conf_threshold(0.25f), iou_threshold(0.45f),
			  encoder_type(HwEncoderType::Auto), bitrate(5'000'000), fps(30), for_x(false), disable_audio(false),
			  trim_start_seconds(0.0), trim_end_seconds(0.0),
			  blacked_type(MaskType::RectFill), blackedout_param(3),
			  name_color(), fixframe_color(),
			  fixmask_type(MaskType::RectFill), fixmask_param(3),
			  fixed_rect_count(0),
			  enable_copyright(false), copyright_image_path(nullptr),
			  copyright_offset_x(0), copyright_offset_y(0), copyright_scale(1.0f),
			  exclude_by_name_enabled(false),
			  ocr_expand_pixels(2),
			  ocr_max_rois_per_frame(6),
			  text_similarity_threshold(0.85f),
			  ocr_model_path(nullptr),
			  ocr_dict_path(nullptr),
			  mask_exclude_text_csv(nullptr)
		{}

	};

	/**
	 * @brief パイプライン実行結果
	 */
	struct PipelineResult {
		bool success = false;
		int processed_frames = 0;
		int total_frames = 0;
		int detected_objects = 0;
		double elapsed_seconds = 0.0;
		const wchar_t* error_message = nullptr;
	};

} // namespace WoLNamesBlackedOut::Core

/**
 * @brief C#互換の処理オプション構造体
 * C#側: [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
 *       public struct ProcessOptions {
 *           public IntPtr InputPath;
 *           public IntPtr OutputPath;
 *           public IntPtr ModelPath;
 *           public int InputWidth;
 *           public int InputHeight;
 *           public float ConfThreshold;
 *           public float IouThreshold;
 *           public int Bitrate;
 *           public int Fps;
 *           public int EncoderType; // 0=None, 1=NVENC, 2=QSV, 3=AMF, 4=Auto
 *           public int EnableMask;
 *           public ColorInfo MaskColor;
 *       }
 */
struct ProcessOptions {
    const wchar_t* input_path;
    const wchar_t* output_path;
    const wchar_t* model_path;
    int input_width;
    int input_height;
    float conf_threshold;
    float iou_threshold;
    int bitrate;
    int fps;
    int encoder_type;
    int enable_mask;
    ColorInfo mask_color;
};

/**
 * @brief C#互換の処理結果構造体
 * C#側: [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
 *       public struct ProcessResult {
 *           public int Success;
 *           public int ProcessedFrames;
 *           public int TotalFrames;
 *           public int DetectedObjects;
 *           public double ElapsedSeconds;
 *           public IntPtr ErrorMessage;
 *       }
 */
struct ProcessResult {
    int success;
    int processed_frames;
    int total_frames;
    int detected_objects;
    double elapsed_seconds;
    const wchar_t* error_message;
};

// ============================================================
// C互換コールバック型定義（C# P/Invoke用）
// ============================================================

/**
 * @brief 進捗コールバック関数ポインタ型
 * C#側: [UnmanagedFunctionPointer(CallingConvention.StdCall)]
 *       public delegate void ProgressCallback(int processedFrames, int totalFrames, double elapsedSeconds);
 */
typedef void (__stdcall *ProgressCallback)(int processed_frames, int total_frames, double elapsed_seconds);

/**
 * @brief プレビュー用パラメータ構造体（C#互換）
 * C#側: [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
 */
struct PreviewParams {
    const wchar_t* file_path;        // 入力ファイルパス
    const wchar_t* model_path;       // ONNXモデルパス
	const wchar_t* copyright_image_path; // Copyright画像パス（任意）
    int input_width;                 // 推論入力幅（通常640）
    int input_height;                // 推論入力高さ（通常640）
    float conf_threshold;            // 信頼度閾値
    float iou_threshold;             // NMS IoU閾値
};

/**
 * @brief プレビュー用マスク更新パラメータ構造体（C#互換）
 */
struct PreviewMaskParams {
    // 検出箇所マスク設定
    int blacked_type;                // 0=Inpaint, 1=Mosaic, 2=Blur, 3=RectFill, 4=No_Inference
    ColorInfo name_color;            // マスク色（RectFill時）
    int blackedout_param;            // Mosaic/Blurパラメータ

    // 固定矩形マスク設定
    int fixmask_type;                // 0=Inpaint, 1=Mosaic, 2=Blur, 3=RectFill
    ColorInfo fixframe_color;        // 固定矩形マスク色
    int fixedFrame_param;            // Mosaic/Blurパラメータ

    // 固定矩形データ
    int fixed_rect_count;
    RectInfo fixed_rects[64];

    // 透かし設定
    bool enable_copyright;
    int copyright_offset_x;  // X position offset for copyright
    int copyright_offset_y;  // Y position offset for copyright
	float copyright_scale;   // Copyright scale (1.0 = original)

	// 名前除外設定（ByteTrack + OCR）
	bool exclude_by_name_enabled;
	int ocr_expand_pixels;          // 0-5
	int ocr_max_rois_per_frame;     // 1-32
	float text_similarity_threshold; // 0.50f - 1.00f
	wchar_t mask_exclude_text_csv[512];

    // 予約領域（将来拡張用）
	int reserved[4];
};