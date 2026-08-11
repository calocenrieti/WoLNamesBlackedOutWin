#pragma once

// Core types
#include "CoreTypes.h"

// DirectX 11
#include <d3d11.h>
#include <wrl/client.h>

namespace WoLNamesBlackedOut::Core {

/**
 * @brief マスク処理シェーダークラス
 * YOLO検出結果に基づいて、Inpaint/Mosaic/Blur効果をGPUで適用する
 */
class MaskShader {
public:
    MaskShader();
    ~MaskShader();

    // コピーコンストラクタと代入演算子を削除
    MaskShader(const MaskShader&) = delete;
    MaskShader& operator=(const MaskShader&) = delete;

    /**
     * @brief マスクシェーダーを初期化
     * @param device D3D11デバイス
     * @return 成功時 true
     */
    bool Initialize(ID3D11Device* device);

    /**
     * @brief リソースの解放
     */
    void Release();

    /**
     * @brief マスクテクスチャを生成
     * @param width テクスチャ幅
     * @param height テクスチャ高さ
     * @param detections 検出結果のリスト
     * @return 生成されたマスクテクスチャ（白矩形のマスク）
     */
    Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateMaskTexture(
        uint32_t width,
        uint32_t height,
        const std::vector<Detection>& detections
    );

    /**
     * @brief Inpaint効果を適用
     * @param source 元画像テクスチャ
     * @param mask マスクテクスチャ
     * @param inpaint_radius インペイント半径（ピクセル）
     * @param output 出力テクスチャ
     * @return 成功時 true
     */
    bool ApplyInpaint(
        ID3D11Texture2D* source,
        ID3D11Texture2D* mask,
        uint32_t inpaint_radius,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& output
    );

    /**
     * @brief Mosaic効果を適用
     * @param source 元画像テクスチャ
     * @param mask マスクテクスチャ
     * @param mosaic_size モザイクサイズ
     * @param output 出力テクスチャ
     * @return 成功時 true
     */
    bool ApplyMosaic(
        ID3D11Texture2D* source,
        ID3D11Texture2D* mask,
        uint32_t mosaic_size,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& output
    );

    /**
     * @brief Blur効果を適用
     * @param source 元画像テクスチャ
     * @param mask マスクテクスチャ
     * @param blur_radius ブラー半径
     * @param output 出力テクスチャ
     * @return 成功時 true
     */
    bool ApplyBlur(
        ID3D11Texture2D* source,
        ID3D11Texture2D* mask,
        uint32_t blur_radius,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& output
    );

    /**
     * @brief 指定された矩形を単色で塗りつぶし
     * @param source 元画像テクスチャ
     * @param rect 塗りつぶす矩形（ピクセル座標、左上原点）
     * @param color 塗りつぶし色（RGBA）
     * @param output 出力テクスチャ
     * @return 成功時 true
     */
    bool ApplyRectFill(
        ID3D11Texture2D* source,
        const RECT& rect,
        uint8_t r, uint8_t g, uint8_t b, uint8_t a,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& output
    );

    /**
     * @brief マスクテクスチャに基づいて単色で塗りつぶし
     * @param source 元画像テクスチャ
     * @param mask マスクテクスチャ（白い部分を塗りつぶし）
     * @param color 塗りつぶし色（RGBA float[4]）
     * @param output 出力テクスチャ
     * @return 成功時 true
     */
    bool ApplyRectFill(
        ID3D11Texture2D* source,
        ID3D11Texture2D* mask,
        const float color[4],
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& output
    );

    /**
     * @brief 指定された矩形にモザイク効果を適用（マスクなし）
     * @param source 元画像テクスチャ
     * @param rect モザイクを適用する矩形（ピクセル座標、左上原点）
     * @param mosaic_size モザイクサイズ
     * @param output 出力テクスチャ
     * @return 成功時 true
     */
    bool ApplyRectMosaic(
        ID3D11Texture2D* source,
        const RECT& rect,
        uint32_t mosaic_size,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& output
    );

    /**
     * @brief 指定された矩形にブラー効果を適用（マスクなし）
     * @param source 元画像テクスチャ
     * @param rect ブラーを適用する矩形（ピクセル座標、左上原点）
     * @param blur_radius ブラー半径
     * @param output 出力テクスチャ
     * @return 成功時 true
     */
    bool ApplyRectBlur(
        ID3D11Texture2D* source,
        const RECT& rect,
        uint32_t blur_radius,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& output
    );

    /**
     * @brief 2つの画像をマスクに基づいて合成（mask白=overlay、mask黒=base）
     * @param base ベース画像テクスチャ
     * @param overlay 重ね合わせ画像テクスチャ
     * @param mask マスクテクスチャ
     * @param output 出力テクスチャ
     * @return 成功時 true
     */
    bool CompositeWithMask(
        ID3D11Texture2D* base,
        ID3D11Texture2D* overlay,
        ID3D11Texture2D* mask,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& output
    );

    /**
     * @brief コピーライト（透かし画像）を指定位置に重ね合わせ
     * @param source 元画像テクスチャ
     * @param watermark 透かし画像のSRV
     * @param pos 配置位置（ピクセル座標、左上原点）
     * @param output 出力テクスチャ
     * @return 成功時 true
     */
    bool ApplyCopyrightOverlay(
        ID3D11Texture2D* source,
        ID3D11ShaderResourceView* watermark_srv,
        uint32_t wm_width, uint32_t wm_height,
        int pos_x, int pos_y,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& output
    );

    /**
     * @brief NV12テクスチャをBGRAに変換（GPUゼロコピー）
     * @param nv12_y Y平面テクスチャ（R8_UNORM）
     * @param nv12_uv UV平面テクスチャ（R8G8_UNORM）
     * @param width 出力幅
     * @param height 出力高さ
     * @param output BGRA出力テクスチャ
     * @return 成功時 true
     */
    bool ConvertNV12ToBGRA(
        ID3D11Texture2D* nv12_y,
        ID3D11Texture2D* nv12_uv,
        uint32_t width,
        uint32_t height,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& output
    );

    /**
     * @brief BGRAテクスチャをNV12に変換（GPUゼロコピー）
     * @param bgra BGRA入力テクスチャ
     * @param width 出力幅
     * @param height 出力高さ
     * @param output_y Y平面出力テクスチャ（R8_UNORM）
     * @param output_uv UV平面出力テクスチャ（R8G8_UNORM）
     * @return 成功時 true
     */
    bool ConvertBGRAToNV12(
        ID3D11Texture2D* bgra,
        uint32_t width,
        uint32_t height,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& output_y,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& output_uv
    );

    /**
     * @brief Y/UV平面をD3D11 NV12テクスチャに合成（GPUゼロコピー）
     * @param y_plane Y平面テクスチャ（R8_UNORM）
     * @param uv_plane UV平面テクスチャ（R8G8_UNORM）
     * @param nv12_output 出力NV12テクスチャ
     * @param width 幅
     * @param height 高さ
     * @return 成功時 true
     */
    bool CompositeNV12Planes(
        ID3D11Texture2D* y_plane,
        ID3D11Texture2D* uv_plane,
        ID3D11Texture2D* nv12_output,
        uint32_t width,
        uint32_t height
    );

private:
    // 四角形マスクのシェーダー定数バッファ
    struct MaskConstantBuffer {
        float x;      // バウンディングボックス X座標
        float y;      // バウンディングボックス Y座標
        float width;  // バウンディングボックス 幅
        float height; // バウンディングボックス 高さ
        uint32_t count; // マスク矩形数
        uint32_t _padding[3];
    };

    // リソース
    Microsoft::WRL::ComPtr<ID3D11VertexShader> mask_vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> mask_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> mask_input_layout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mask_constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> point_sampler_; // ニアレストネイバー用
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linear_sampler_; // ラインアル用

    // Inpaintシェーダー
    Microsoft::WRL::ComPtr<ID3D11PixelShader> inpaint_pixel_shader_;

    // Mosaicシェーダー
    Microsoft::WRL::ComPtr<ID3D11PixelShader> mosaic_pixel_shader_;
    struct MosaicConstantBuffer {
        float mosaic_size;
        float _pad0;          // HLSL float2 textureSize の8バイトアライン対策
        float texture_width;
        float texture_height;
        uint32_t _padding[2];
    };
    Microsoft::WRL::ComPtr<ID3D11Buffer> mosaic_constant_buffer_;

    // Blurシェーダー（Kawase Blur）
    Microsoft::WRL::ComPtr<ID3D11PixelShader> blur_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> blur_temp_texture_;
    struct BlurConstantBuffer {
        float offset;
        float pixel_size_x;
        float pixel_size_y;
        uint32_t iterations;
        uint32_t _padding[3];
    };
    Microsoft::WRL::ComPtr<ID3D11Buffer> blur_constant_buffer_;

    // 矩形塗りつぶしシェーダー
    Microsoft::WRL::ComPtr<ID3D11PixelShader> rectfill_pixel_shader_;
    struct RectFillConstantBuffer {
        float rect_x;       // 左上X（ピクセル）
        float rect_y;       // 左上Y（ピクセル）
        float rect_width;   // 幅
        float rect_height;  // 高さ
        float fillColor[4]; // RGBA色（0-1）
        float texture_size[2];// xy=width, height
    };
    Microsoft::WRL::ComPtr<ID3D11Buffer> rectfill_constant_buffer_;

    // コピーライト（透かし画像）オーバーレイシェーダー
    Microsoft::WRL::ComPtr<ID3D11PixelShader> copyright_pixel_shader_;
    struct CopyrightConstantBuffer {
        float wm_x;         // 配置X（ピクセル）
        float wm_y;         // 配置Y（ピクセル）
        float wm_width;     // 透かし画像幅
        float wm_height;    // 透かし画像高さ
        float texture_width;// テクスチャ幅（ピクセル）
        float texture_height;// テクスチャ高さ（ピクセル）
        uint32_t _padding[4];
    };
    Microsoft::WRL::ComPtr<ID3D11Buffer> copyright_constant_buffer_;

    // マスク合成シェーダー
    Microsoft::WRL::ComPtr<ID3D11PixelShader> composite_pixel_shader_;

    // NV12→BGRA変換シェーダー
    Microsoft::WRL::ComPtr<ID3D11PixelShader> nv12_to_bgra_pixel_shader_;

    // BGRA→NV12変換シェーダー（Y平面用）
    Microsoft::WRL::ComPtr<ID3D11PixelShader> bgra_to_nv12_y_pixel_shader_;
    // BGRA→NV12変換シェーダー（UV平面用）
    Microsoft::WRL::ComPtr<ID3D11PixelShader> bgra_to_nv12_uv_pixel_shader_;

    // ヘルパーメソッド
    bool CreateFullScreenQuad(Microsoft::WRL::ComPtr<ID3D11Buffer>& vertex_buffer);
    bool CreateOutputTexture(
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT format,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv
    );
};

} // namespace WoLNamesBlackedOut::Core
