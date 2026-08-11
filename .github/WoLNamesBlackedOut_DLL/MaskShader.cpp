#include "pch.h"
#include "MaskShader.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

// ファイルログ（PreviewPipeline.cpp と同じ仕組み）
static void MaskLogToFile(const char* msg) {
    OutputDebugStringA(msg);
}
static void MaskLogFmt(const char* fmt, ...) {
    char buf[1024] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    MaskLogToFile(buf);
}

namespace WoLNamesBlackedOut::Core {

// ============================================================
// マスクテクスチャ生成シェーダー（四角形描画用）
// ============================================================
static const char* mask_vertex_shader_src = 
    "struct VS_INPUT {\n"
    "    float4 pos : POSITION;\n"
    "};\n"
    "\n"
    "struct PS_INPUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "\n"
    "cbuffer MaskConstantBuffer : register(b0) {\n"
    "    float4 maskRects[16]; // xy=position, zw=size (max 16 rects)\n"
    "    uint maskCount;\n"
    "};\n"
    "\n"
    "// マスク矩形のインデックスを返す（外なら-1）\n"
    "int getMaskIndex(float2 uv) {\n"
    "    for (uint i = 0; i < maskCount; i++) {\n"
    "        float2 pos = maskRects[i].xy;\n"
    "        float2 size = maskRects[i].zw;\n"
    "        float2 halfSize = size * 0.5;\n"
    "        if (uv.x >= (pos.x - halfSize.x) && uv.x < (pos.x + halfSize.x) &&\n"
    "            uv.y >= (pos.y - halfSize.y) && uv.y < (pos.y + halfSize.y)) {\n"
    "            return int(i);\n"
    "        }\n"
    "    }\n"
    "    return -1;\n"
    "}\n"
    "\n"
    "PS_INPUT main(VS_INPUT input) {\n"
    "    PS_INPUT output;\n"
    "    output.pos = input.pos;\n"
    "    // NDC (-1 to 1) -> UV (0 to 1), Y軸はDirectXテクスチャ座標に合わせて反転\n"
    "    output.uv.x =  0.5f * input.pos.x + 0.5f;\n"
    "    output.uv.y = -0.5f * input.pos.y + 0.5f;\n"
    "    return output;\n"
    "}\n";

static const char* mask_pixel_shader_src = 
    "struct PS_INPUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "\n"
    "cbuffer MaskConstantBuffer : register(b0) {\n"
    "    float4 maskRects[16]; // xy=position, zw=size\n"
    "    uint maskCount;\n"
    "};\n"
    "\n"
    "float4 main(PS_INPUT input) : SV_TARGET {\n"
    "    // 背景は黒（0.0）\n"
    "    float4 color = float4(0.0, 0.0, 0.0, 1.0);\n"
    "\n"
    "    // マスク矩形内なら白（1.0）\n"
    "    for (uint i = 0; i < maskCount; i++) {\n"
    "        float2 pos = maskRects[i].xy;\n"
    "        float2 size = maskRects[i].zw;\n"
    "        float2 halfSize = size * 0.5;\n"
    "        if (input.uv.x >= (pos.x - halfSize.x) && input.uv.x < (pos.x + halfSize.x) &&\n"
    "            input.uv.y >= (pos.y - halfSize.y) && input.uv.y < (pos.y + halfSize.y)) {\n"
    "            color = float4(1.0, 1.0, 1.0, 1.0);\n"
    "        }\n"
    "    }\n"
    "\n"
    "    return color;\n"
    "}\n";

// ============================================================
// Inpaintシェーダー（OBS effects inpaint.effect 移植）
// マスク領域を周囲の色で塗りつぶす（境界サーチ + 加重平均）
// ============================================================
static const char* inpaint_pixel_shader_src =
    "struct PS_INPUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "Texture2D inputTexture : register(t0);\n"
    "Texture2D maskTexture : register(t1);\n"
    "SamplerState samplerState : register(s0);\n"
    "\n"
    "cbuffer InpaintConstantBuffer : register(b0) {\n"
    "    float inpaint_radius;\n"
    "    float tex_size;\n"
    "    uint _padding[2];\n"
    "};\n"
    "\n"
    "float4 main(PS_INPUT input) : SV_TARGET {\n"
    "    float2 uv = input.uv;\n"
    "    float maskVal = maskTexture.Sample(samplerState, uv).r;\n"
    "\n"
    "    // マスク外は元の色をそのまま返す\n"
    "    if (maskVal < 0.5) {\n"
    "        return inputTexture.Sample(samplerState, uv);\n"
    "    }\n"
    "\n"
    "    // マスク内：inpaint適用\n"
    "    float radiusPixels = inpaint_radius;\n"
    "    float radiusUV = radiusPixels / tex_size;\n"
    "\n"
    "    const int NUM_DIRS = 24;\n"
    "    const float PI = 3.14159265359;\n"
    "\n"
    "    float3 totalColor = float3(0.0, 0.0, 0.0);\n"
    "    float totalWeight = 0.0;\n"
    "\n"
    "    for (int i = 0; i < NUM_DIRS; i++) {\n"
    "        float angle = (float(i) / float(NUM_DIRS)) * 2.0 * PI;\n"
    "        float2 dir = float2(cos(angle), sin(angle));\n"
    "\n"
    "        float tMin = 0.0;\n"
    "        float tMax = radiusUV * 2.0;\n"
    "        float2 outerUV = uv + dir * tMax;\n"
    "        outerUV = clamp(outerUV, 0.0, 1.0);\n"
    "        float outerMask = maskTexture.Sample(samplerState, outerUV).r;\n"
    "\n"
    "        if (outerMask > 0.5) {\n"
    "            outerUV = uv + dir * radiusUV * 3.0;\n"
    "            outerUV = clamp(outerUV, 0.0, 1.0);\n"
    "            outerMask = maskTexture.Sample(samplerState, outerUV).r;\n"
    "            tMax = radiusUV * 3.0;\n"
    "        }\n"
    "\n"
    "        if (outerMask > 0.5) {\n"
    "            continue;\n"
    "        }\n"
    "\n"
    "        float2 boundaryUV = uv;\n"
    "        for (int step = 0; step < 8; step++) {\n"
    "            float tMid = (tMin + tMax) * 0.5;\n"
    "            float2 midUV = uv + dir * tMid;\n"
    "            midUV = clamp(midUV, 0.0, 1.0);\n"
    "            float midMask = maskTexture.Sample(samplerState, midUV).r;\n"
    "\n"
    "            if (midMask > 0.5) {\n"
    "                tMin = tMid;\n"
    "                boundaryUV = midUV;\n"
    "            } else {\n"
    "                tMax = tMid;\n"
    "            }\n"
    "        }\n"
    "\n"
    "        float2 outsideUV = boundaryUV + dir * (radiusUV * 0.1);\n"
    "        outsideUV = clamp(outsideUV, 0.0, 1.0);\n"
    "        float4 outsideColor = inputTexture.Sample(samplerState, outsideUV);\n"
    "\n"
    "        float2 insideUV = boundaryUV - dir * (radiusUV * 0.05);\n"
    "        insideUV = clamp(insideUV, 0.0, 1.0);\n"
    "        float4 insideColor = inputTexture.Sample(samplerState, insideUV);\n"
    "\n"
    "        float distFromCenter = length(uv - boundaryUV);\n"
    "        float distWeight = 1.0 - smoothstep(0.0, radiusUV * 2.0, distFromCenter);\n"
    "\n"
    "        float3 gradient = outsideColor.rgb - insideColor.rgb;\n"
    "        float3 extrapolatedColor = outsideColor.rgb + gradient * 0.3;\n"
    "        float3 color = lerp(outsideColor.rgb, extrapolatedColor, 0.5);\n"
    "\n"
    "        totalColor += color * distWeight;\n"
    "        totalWeight += distWeight;\n"
    "    }\n"
    "\n"
    "    if (totalWeight > 0.01) {\n"
    "        return float4(totalColor / totalWeight, 1.0);\n"
    "    } else {\n"
    "        return inputTexture.Sample(samplerState, uv);\n"
    "    }\n"
    "}\n";

// ============================================================
// Mosaicシェーダー（OBS effects pixelate.effect 参照）
// ============================================================
static const char* mosaic_pixel_shader_src = 
    "struct PS_INPUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "Texture2D inputTexture : register(t0);\n"
    "Texture2D maskTexture : register(t1);\n"
    "SamplerState samplerState : register(s0);\n"
    "\n"
    "cbuffer MosaicConstantBuffer : register(b0) {\n"
    "    float mosaicSize;\n"
    "    float _pad0;\n"
    "    float textureWidth;\n"
    "    float textureHeight;\n"
    "};\n"
    "\n"
    "float4 main(PS_INPUT input) : SV_TARGET {\n"
    "    // マスク確認：マスク外なら元のピクセルを返す\n"
    "    if (maskTexture.Sample(samplerState, input.uv).r < 0.5f) {\n"
    "        return inputTexture.Sample(samplerState, input.uv);\n"
    "    }\n"
    "\n"
    "    // マスク内：モザイク処理\n"
    "    float2 textureSize = float2(textureWidth, textureHeight);\n"
    "    float2 pixelSize = mosaicSize / textureSize;\n"
    "    if (pixelSize.x <= 0.0f || pixelSize.y <= 0.0f) {\n"
    "        return inputTexture.Sample(samplerState, input.uv);\n"
    "    }\n"
    "    \n"
    "    // モザイクグリッドにスナップ\n"
    "    float2 gridUV = floor(input.uv / pixelSize) * pixelSize + pixelSize * 0.5;\n"
    "    gridUV = clamp(gridUV, 0.0, 1.0);\n"
    "\n"
    "    return inputTexture.Sample(samplerState, gridUV);\n"
    "}\n";

// ============================================================
// Kawase Blurシェーダー（OBS effects kawase_blur.effect 参照）
// ============================================================
static const char* blur_pixel_shader_src = 
    "struct PS_INPUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "Texture2D inputTexture : register(t0);\n"
    "Texture2D maskTexture : register(t1);\n"
    "SamplerState samplerState : register(s0);\n"
    "\n"
    "cbuffer BlurConstantBuffer : register(b0) {\n"
    "    float offset;\n"
    "    float pixelSizeX;\n"
    "    float pixelSizeY;\n"
    "    uint iterations;\n"
    "    uint _padding[3];\n"
    "};\n"
    "\n"
    "// マスクが有効な領域のみにブラーを適用\n"
    "float4 main(PS_INPUT input) : SV_TARGET {\n"
    "    // マスク外なら元のピクセルを返す\n"
    "    if (maskTexture.Sample(samplerState, input.uv).r < 0.5f) {\n"
    "        return inputTexture.Sample(samplerState, input.uv);\n"
    "    }\n"
    "\n"
    "    // Kawase Blur（中心 + 4方向）\n"
    "    float4 color = inputTexture.Sample(samplerState, input.uv);\n"
    "    float2 pixelSize = float2(pixelSizeX, pixelSizeY);\n"
    "    \n"
    "    // 4方向のオフセット\n"
    "    float2 offsets[4] = {\n"
    "        float2(1.0, 1.0),\n"
    "        float2(-1.0, -1.0),\n"
    "        float2(1.0, -1.0),\n"
    "        float2(-1.0, 1.0)\n"
    "    };\n"
    "\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "        float2 sampleUV = clamp(input.uv + offsets[i] * pixelSize, 0.0, 1.0);\n"
    "        color += inputTexture.Sample(samplerState, sampleUV);\n"
    "    }\n"
    "\n"
    "    return color / 5.0;\n"
    "}\n";

// ============================================================
// 矩形塗りつぶしシェーダー
// ============================================================
static const char* rectfill_pixel_shader_src = 
    "struct PS_INPUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "Texture2D inputTexture : register(t0);\n"
    "Texture2D maskTexture  : register(t1);\n"
    "SamplerState samplerState : register(s0);\n"
    "\n"
    "cbuffer RectFillConstantBuffer : register(b0) {\n"
    "    float rect_x;\n"
    "    float rect_y;\n"
    "    float rect_width;\n"
    "    float rect_height;\n"
    "    float4 fillColor;\n"
    "    float2 texture_size;\n"
    "};\n"
    "\n"
    "float4 main(PS_INPUT input) : SV_TARGET {\n"
    "    float4 src  = inputTexture.Sample(samplerState, input.uv);\n"
    "    float  mask = maskTexture.Sample(samplerState, input.uv).r;\n"
    "    if (mask > 0.5f) return fillColor;\n"
    "    return src;\n"
    "}\n";

// ============================================================
// コピーライト（透かし画像）オーバーレイシェーダー
// ============================================================
static const char* copyright_pixel_shader_src = 
    "struct PS_INPUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "Texture2D inputTexture : register(t0);\n"
    "Texture2D watermarkTexture : register(t1);\n"
    "SamplerState samplerState : register(s0);\n"
    "\n"
    "cbuffer CopyrightConstantBuffer : register(b0) {\n"
    "    float wm_pos_x;     // 配置X（ピクセル）\n"
    "    float wm_pos_y;     // 配置Y（ピクセル）\n"
    "    float wm_width;     // 透かし画像幅\n"
    "    float wm_height;    // 透かし画像高さ\n"
    "    float texture_width;// テクスチャ幅（ピクセル）\n"
    "    float texture_height;// テクスチャ高さ（ピクセル）\n"
    "};\n"
    "\n"
    "float4 main(PS_INPUT input) : SV_TARGET {\n"
    "    float2 texSize = float2(texture_width, texture_height);\n"
    "    float2 px = input.uv * texSize;\n"
    "    \n"
    "    // 透かし画像のUV座標を計算\n"
    "    float2 wm_uv = (px - float2(wm_pos_x, wm_pos_y)) / float2(wm_width, wm_height);\n"
    "    \n"
    "    // 透かし画像内かチェック\n"
    "    if (wm_uv.x >= 0.0 && wm_uv.x <= 1.0 && wm_uv.y >= 0.0 && wm_uv.y <= 1.0) {\n"
    "        // PNGアルファを使って合成（透過対応）\n"
    "        float4 src_color = inputTexture.Sample(samplerState, input.uv);\n"
    "        float4 wm_color = watermarkTexture.Sample(samplerState, wm_uv);\n"
    "        float a = saturate(wm_color.a);\n"
    "        float3 rgb = lerp(src_color.rgb, wm_color.rgb, a);\n"
    "        return float4(rgb, src_color.a);\n"
    "    }\n"
    "\n"
    "    // 透かし画像外なら元のピクセルを返す\n"
    "    return inputTexture.Sample(samplerState, input.uv);\n"
    "}\n";

// ============================================================
// NV12 → BGRA 変換シェーダー（GPUゼロコピー）
// BT.601 Limited Range（FFmpeg D3D11VAデコード標準）
// 注意: 一部D3D11VAドライバーはVU順(CrCb)で返すためUV入れ替え対応
// ============================================================
static const char* nv12_to_bgra_pixel_shader_src =
    "struct PS_INPUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "Texture2D yTexture : register(t0);\n"
    "Texture2D uvTexture : register(t1);\n"
    "SamplerState samplerState : register(s0);\n"
    "\n"
    "float4 main(PS_INPUT input) : SV_TARGET {\n"
    "    float y = yTexture.Sample(samplerState, input.uv).r;\n"
    "    float2 uv = uvTexture.Sample(samplerState, input.uv).rg;\n"
    "    float Y_val = (y - 0.062745) * 1.1643;\n"
    "    // VU順対応: 一部ドライバーはx=V(Cr), y=U(Cb)で返す\n"
    "    float V_val = uv.x - 0.501961;\n"
    "    float U_val = uv.y - 0.501961;\n"
    "    float r = saturate(Y_val + 1.5960 * V_val);\n"
    "    float g = saturate(Y_val - 0.3917 * U_val - 0.8129 * V_val);\n"
    "    float b = saturate(Y_val + 2.0172 * U_val);\n"
    "    return float4(b, g, r, 1.0);\n"
    "}\n";

// ============================================================
// BGRA → NV12 Y平面変換シェーダー
// ============================================================
static const char* bgra_to_nv12_y_pixel_shader_src =
    "struct PS_INPUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "Texture2D inputTexture : register(t0);\n"
    "SamplerState samplerState : register(s0);\n"
    "\n"
    "float4 main(PS_INPUT input) : SV_TARGET {\n"
    "    float4 bgra = inputTexture.Sample(samplerState, input.uv);\n"
    "    float y = 0.257 * bgra.r + 0.504 * bgra.g + 0.098 * bgra.b + 0.0625;\n"
    "    return float4(y, 0, 0, 1);\n"
    "}\n";

// ============================================================
// BGRA → NV12 UV平面変換シェーダー（2x2ダウンサンプリング）
// ============================================================
static const char* bgra_to_nv12_uv_pixel_shader_src =
    "struct PS_INPUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "Texture2D inputTexture : register(t0);\n"
    "SamplerState samplerState : register(s0);\n"
    "\n"
    "float4 main(PS_INPUT input) : SV_TARGET {\n"
    "    float4 bgra = inputTexture.Sample(samplerState, input.uv);\n"
    "    float u = -0.148 * bgra.r - 0.291 * bgra.g + 0.439 * bgra.b + 0.5;\n"
    "    float v = 0.439 * bgra.r - 0.368 * bgra.g - 0.071 * bgra.b + 0.5;\n"
    "    return float4(u, v, 0, 1);\n"
    "}\n";

// ============================================================
// マスク合成シェーダー（base + overlayをmaskで合成）
// ============================================================
static const char* composite_pixel_shader_src =
    "struct PS_INPUT {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float2 uv : TEXCOORD0;\n"
    "};\n"
    "Texture2D baseTexture : register(t0);\n"
    "Texture2D overlayTexture : register(t1);\n"
    "Texture2D maskTexture : register(t2);\n"
    "SamplerState samplerState : register(s0);\n"
    "\n"
    "float4 main(PS_INPUT input) : SV_TARGET {\n"
    "    float4 base = baseTexture.Sample(samplerState, input.uv);\n"
    "    float4 overlay = overlayTexture.Sample(samplerState, input.uv);\n"
    "    float mask = maskTexture.Sample(samplerState, input.uv).r;\n"
    "    if (mask > 0.5f) return overlay;\n"
    "    return base;\n"
    "}\n";

// ============================================================// MaskShader クラス実装
// ============================================================

MaskShader::MaskShader()
    : mask_vertex_shader_(nullptr),
      mask_pixel_shader_(nullptr),
      mask_input_layout_(nullptr),
      mask_constant_buffer_(nullptr),
      point_sampler_(nullptr),
      linear_sampler_(nullptr),
      inpaint_pixel_shader_(nullptr),
      mosaic_pixel_shader_(nullptr),
      mosaic_constant_buffer_(nullptr),
      blur_pixel_shader_(nullptr),
      blur_temp_texture_(nullptr),
      blur_constant_buffer_(nullptr),
      rectfill_pixel_shader_(nullptr),
      rectfill_constant_buffer_(nullptr),
      copyright_pixel_shader_(nullptr),
      copyright_constant_buffer_(nullptr),
      composite_pixel_shader_(nullptr) {
}

MaskShader::~MaskShader() {
    Release();
}

void MaskShader::Release() {
    mask_vertex_shader_.Reset();
    mask_pixel_shader_.Reset();
    mask_input_layout_.Reset();
    mask_constant_buffer_.Reset();
    point_sampler_.Reset();
    linear_sampler_.Reset();
    inpaint_pixel_shader_.Reset();
    mosaic_pixel_shader_.Reset();
    mosaic_constant_buffer_.Reset();
    blur_pixel_shader_.Reset();
    blur_temp_texture_.Reset();
    blur_constant_buffer_.Reset();
    rectfill_pixel_shader_.Reset();
    rectfill_constant_buffer_.Reset();
    copyright_pixel_shader_.Reset();
    copyright_constant_buffer_.Reset();
    composite_pixel_shader_.Reset();
    nv12_to_bgra_pixel_shader_.Reset();
    bgra_to_nv12_y_pixel_shader_.Reset();
    bgra_to_nv12_uv_pixel_shader_.Reset();
}

// HLSL ソースをランタイムコンパイルするヘルパー
static HRESULT CompileShaderSource(
    const char* src,
    const char* entry_point,
    const char* target,
    Microsoft::WRL::ComPtr<ID3DBlob>& blob)
{
    Microsoft::WRL::ComPtr<ID3DBlob> err_blob;
    HRESULT hr = D3DCompile(
        src, strlen(src),
        nullptr, nullptr, nullptr,
        entry_point, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
        blob.ReleaseAndGetAddressOf(),
        err_blob.ReleaseAndGetAddressOf()
    );
    if (FAILED(hr)) {
        if (err_blob) {
            MaskLogFmt("[MaskShader] Compile error (%s/%s): %s\n",
                entry_point, target, (char*)err_blob->GetBufferPointer());
        } else {
            MaskLogFmt("[MaskShader] Compile error (%s/%s): HRESULT=0x%08X\n",
                entry_point, target, (unsigned)hr);
        }
    }
    return hr;
}

bool MaskShader::Initialize(ID3D11Device* device) {
    if (mask_vertex_shader_.Get() && mask_pixel_shader_.Get()) {
        MaskLogToFile("[MaskShader] Already initialized\n");
        return true;
    }

    if (!device) {
        MaskLogToFile("[MaskShader] Initialize: device is null\n");
        return false;
    }

    MaskLogToFile("[MaskShader] Initialize start\n");

    HRESULT hr;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;

    // --- マスク生成 頂点シェーダー ---
    MaskLogToFile("[MaskShader] Compiling vertex shader...\n");
    hr = CompileShaderSource(mask_vertex_shader_src, "main", "vs_4_0", blob);
    if (FAILED(hr) || !blob) {
        MaskLogFmt("[MaskShader] FAIL: compile vertex shader HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = device->CreateVertexShader(
        blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, mask_vertex_shader_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !mask_vertex_shader_.Get()) {
        MaskLogFmt("[MaskShader] FAIL: CreateVertexShader HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    MaskLogToFile("[MaskShader] Vertex shader OK\n");

    D3D11_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device->CreateInputLayout(
        input_layout, ARRAYSIZE(input_layout),
        blob->GetBufferPointer(), blob->GetBufferSize(),
        mask_input_layout_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !mask_input_layout_.Get()) {
        MaskLogFmt("[MaskShader] FAIL: CreateInputLayout HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    MaskLogToFile("[MaskShader] InputLayout OK\n");

    // --- マスク生成 ピクセルシェーダー ---
    MaskLogToFile("[MaskShader] Compiling mask pixel shader...\n");
    hr = CompileShaderSource(mask_pixel_shader_src, "main", "ps_4_0", blob);
    if (FAILED(hr) || !blob) {
        MaskLogFmt("[MaskShader] FAIL: compile mask PS HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = device->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, mask_pixel_shader_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !mask_pixel_shader_.Get()) {
        MaskLogFmt("[MaskShader] FAIL: CreatePixelShader(mask) HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    MaskLogToFile("[MaskShader] Mask PS OK\n");

    // --- Inpaintシェーダー ---
    MaskLogToFile("[MaskShader] Compiling inpaint shader...\n");
    hr = CompileShaderSource(inpaint_pixel_shader_src, "main", "ps_4_0", blob);
    if (FAILED(hr) || !blob) {
        MaskLogFmt("[MaskShader] FAIL: compile inpaint PS HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = device->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, inpaint_pixel_shader_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !inpaint_pixel_shader_.Get()) {
        MaskLogFmt("[MaskShader] FAIL: CreatePixelShader(inpaint) HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    MaskLogToFile("[MaskShader] Inpaint PS OK\n");

    // --- Mosaicシェーダー ---
    MaskLogToFile("[MaskShader] Compiling mosaic shader...\n");
    hr = CompileShaderSource(mosaic_pixel_shader_src, "main", "ps_4_0", blob);
    if (FAILED(hr) || !blob) {
        MaskLogFmt("[MaskShader] FAIL: compile mosaic PS HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = device->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, mosaic_pixel_shader_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !mosaic_pixel_shader_.Get()) {
        MaskLogFmt("[MaskShader] FAIL: CreatePixelShader(mosaic) HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    MaskLogToFile("[MaskShader] Mosaic PS OK\n");

    // --- Blurシェーダー ---
    MaskLogToFile("[MaskShader] Compiling blur shader...\n");
    hr = CompileShaderSource(blur_pixel_shader_src, "main", "ps_4_0", blob);
    if (FAILED(hr) || !blob) {
        MaskLogFmt("[MaskShader] FAIL: compile blur PS HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = device->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, blur_pixel_shader_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !blur_pixel_shader_.Get()) {
        MaskLogFmt("[MaskShader] FAIL: CreatePixelShader(blur) HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    MaskLogToFile("[MaskShader] Blur PS OK\n");

    // --- サンプラー状態の作成 ---
    D3D11_SAMPLER_DESC point_desc = {};
    point_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    point_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    point_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    point_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    point_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    point_desc.MinLOD = 0;
    point_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&point_desc, point_sampler_.ReleaseAndGetAddressOf());

    D3D11_SAMPLER_DESC linear_desc = {};
    linear_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    linear_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    linear_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    linear_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    linear_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    linear_desc.MinLOD = 0;
    linear_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&linear_desc, linear_sampler_.ReleaseAndGetAddressOf());

    // --- 定数バッファの作成 ---
    if (mask_constant_buffer_.Get()) mask_constant_buffer_.Reset();
    D3D11_BUFFER_DESC buffer_desc = {};
    buffer_desc.ByteWidth = sizeof(DirectX::XMFLOAT4) * 16 + sizeof(uint32_t);
    buffer_desc.ByteWidth = ((buffer_desc.ByteWidth + 15) & ~15); // 16バイトアラインメント
    buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&buffer_desc, nullptr, mask_constant_buffer_.ReleaseAndGetAddressOf());

    if (mosaic_constant_buffer_.Get()) mosaic_constant_buffer_.Reset();
    buffer_desc.ByteWidth = sizeof(MosaicConstantBuffer);
    buffer_desc.ByteWidth = ((buffer_desc.ByteWidth + 15) & ~15);
    hr = device->CreateBuffer(&buffer_desc, nullptr, mosaic_constant_buffer_.ReleaseAndGetAddressOf());

    if (blur_constant_buffer_.Get()) blur_constant_buffer_.Reset();
    buffer_desc.ByteWidth = sizeof(BlurConstantBuffer);
    buffer_desc.ByteWidth = ((buffer_desc.ByteWidth + 15) & ~15);
    hr = device->CreateBuffer(&buffer_desc, nullptr, blur_constant_buffer_.ReleaseAndGetAddressOf());

    // --- 矩形塗りつぶしシェーダー ---
    MaskLogToFile("[MaskShader] Compiling rectfill shader...\n");
    hr = CompileShaderSource(rectfill_pixel_shader_src, "main", "ps_4_0", blob);
    if (FAILED(hr) || !blob) {
        MaskLogFmt("[MaskShader] FAIL: compile rectfill PS HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = device->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, rectfill_pixel_shader_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !rectfill_pixel_shader_.Get()) {
        MaskLogFmt("[MaskShader] FAIL: CreatePixelShader(rectfill) HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    MaskLogToFile("[MaskShader] RectFill PS OK\n");

    // --- コピーライトシェーダー ---
    MaskLogToFile("[MaskShader] Compiling copyright shader...\n");
    hr = CompileShaderSource(copyright_pixel_shader_src, "main", "ps_4_0", blob);
    if (FAILED(hr) || !blob) {
        MaskLogFmt("[MaskShader] FAIL: compile copyright PS HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = device->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, copyright_pixel_shader_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !copyright_pixel_shader_.Get()) {
        MaskLogFmt("[MaskShader] FAIL: CreatePixelShader(copyright) HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    MaskLogToFile("[MaskShader] Copyright PS OK\n");

    // --- マスク合成シェーダー ---
    MaskLogToFile("[MaskShader] Compiling composite shader...\n");
    hr = CompileShaderSource(composite_pixel_shader_src, "main", "ps_4_0", blob);
    if (FAILED(hr) || !blob) {
        MaskLogFmt("[MaskShader] FAIL: compile composite PS HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = device->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, composite_pixel_shader_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !composite_pixel_shader_.Get()) {
        MaskLogFmt("[MaskShader] FAIL: CreatePixelShader(composite) HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    MaskLogToFile("[MaskShader] Composite PS OK\n");

    // --- NV12→BGRA変換シェーダー ---
    MaskLogToFile("[MaskShader] Compiling NV12→BGRA shader...\n");
    hr = CompileShaderSource(nv12_to_bgra_pixel_shader_src, "main", "ps_4_0", blob);
    if (FAILED(hr) || !blob) {
        MaskLogFmt("[MaskShader] FAIL: compile NV12→BGRA PS HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = device->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, nv12_to_bgra_pixel_shader_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !nv12_to_bgra_pixel_shader_.Get()) {
        MaskLogFmt("[MaskShader] FAIL: CreatePixelShader(NV12→BGRA) HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    MaskLogToFile("[MaskShader] NV12→BGRA PS OK\n");

    // --- BGRA→NV12 Y平面変換シェーダー ---
    MaskLogToFile("[MaskShader] Compiling BGRA→NV12 Y shader...\n");
    hr = CompileShaderSource(bgra_to_nv12_y_pixel_shader_src, "main", "ps_4_0", blob);
    if (FAILED(hr) || !blob) {
        MaskLogFmt("[MaskShader] FAIL: compile BGRA→NV12 Y PS HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = device->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, bgra_to_nv12_y_pixel_shader_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !bgra_to_nv12_y_pixel_shader_.Get()) {
        MaskLogFmt("[MaskShader] FAIL: CreatePixelShader(BGRA→NV12 Y) HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    MaskLogToFile("[MaskShader] BGRA→NV12 Y PS OK\n");

    // --- BGRA→NV12 UV平面変換シェーダー ---
    MaskLogToFile("[MaskShader] Compiling BGRA→NV12 UV shader...\n");
    hr = CompileShaderSource(bgra_to_nv12_uv_pixel_shader_src, "main", "ps_4_0", blob);
    if (FAILED(hr) || !blob) {
        MaskLogFmt("[MaskShader] FAIL: compile BGRA→NV12 UV PS HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    hr = device->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, bgra_to_nv12_uv_pixel_shader_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !bgra_to_nv12_uv_pixel_shader_.Get()) {
        MaskLogFmt("[MaskShader] FAIL: CreatePixelShader(BGRA→NV12 UV) HR=0x%08X\n", (unsigned)hr);
        return false;
    }
    MaskLogToFile("[MaskShader] BGRA→NV12 UV PS OK\n");

    // --- 矩形塗りつぶし定数バッファ ---
    if (rectfill_constant_buffer_.Get()) rectfill_constant_buffer_.Reset();
    buffer_desc.ByteWidth = sizeof(RectFillConstantBuffer);
    buffer_desc.ByteWidth = ((buffer_desc.ByteWidth + 15) & ~15);
    hr = device->CreateBuffer(&buffer_desc, nullptr, rectfill_constant_buffer_.ReleaseAndGetAddressOf());

    // --- コピーライト定数バッファ ---
    if (copyright_constant_buffer_.Get()) copyright_constant_buffer_.Reset();
    buffer_desc.ByteWidth = sizeof(CopyrightConstantBuffer);
    buffer_desc.ByteWidth = ((buffer_desc.ByteWidth + 15) & ~15);
    hr = device->CreateBuffer(&buffer_desc, nullptr, copyright_constant_buffer_.ReleaseAndGetAddressOf());

    MaskLogToFile("[MaskShader] Initialize complete OK\n");
    return true;
}

bool MaskShader::CreateFullScreenQuad(Microsoft::WRL::ComPtr<ID3D11Buffer>& vertex_buffer) {
    // 全画面四角形（NDC座標, TRIANGLESTRIP: TL, TR, BL, BR）
    float quad_vertices[] = {
        -1.0f,  1.0f, 0.0f, 1.0f,  // TL
         1.0f,  1.0f, 0.0f, 1.0f,  // TR
        -1.0f, -1.0f, 0.0f, 1.0f,  // BL
         1.0f, -1.0f, 0.0f, 1.0f   // BR
    };

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(quad_vertices);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init_data = {};
    init_data.pSysMem = quad_vertices;

    ID3D11Device* shader_device = nullptr;
    mask_vertex_shader_->GetDevice(&shader_device);
    HRESULT hr = shader_device->CreateBuffer(&bd, &init_data, vertex_buffer.ReleaseAndGetAddressOf());
    return SUCCEEDED(hr) && vertex_buffer.Get();
}

bool MaskShader::CreateOutputTexture(
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv) {

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    ID3D11Device* dev = nullptr;
    mask_pixel_shader_->GetDevice(&dev);
    if (!dev) return false;
    HRESULT hr = dev->CreateTexture2D(&desc, nullptr, texture.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !texture.Get()) return false;

    hr = dev->CreateShaderResourceView(
        texture.Get(), nullptr, srv.ReleaseAndGetAddressOf());
    return SUCCEEDED(hr) && srv.Get();
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> MaskShader::CreateMaskTexture(
    uint32_t width,
    uint32_t height,
    const std::vector<Detection>& detections) {

    ID3D11Device* device = nullptr;
    mask_pixel_shader_->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    if (device) device->GetImmediateContext(&context);

    if (!device || !context) return nullptr;

    if (detections.empty()) {
        return nullptr;
    }

    // 出力マスクテクスチャ（RTV+SRV）を作成
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mask_texture;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, mask_texture.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !mask_texture.Get()) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> mask_rtv;
    hr = device->CreateRenderTargetView(mask_texture.Get(), nullptr, mask_rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !mask_rtv.Get()) {
        return nullptr;
    }

    // GPUレンダリング用の四角形
    Microsoft::WRL::ComPtr<ID3D11Buffer> quad_vertex_buffer;
    if (!CreateFullScreenQuad(quad_vertex_buffer) || !quad_vertex_buffer.Get()) {
        return nullptr;
    }

    // 既存マスクを保持しつつ白を重ねるために加算ブレンドを使用
    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state;
    hr = device->CreateBlendState(&blend_desc, blend_state.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        return nullptr;
    }

    struct MaskCB {
        float rects[16][4];
        uint32_t maskCount;
        uint32_t padding[3];
    };

    // 描画ターゲット初期化（黒）
    const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context->OMSetRenderTargets(1, mask_rtv.GetAddressOf(), nullptr);
    context->ClearRenderTargetView(mask_rtv.Get(), clear_color);

    float blend_factor[4] = { 0, 0, 0, 0 };
    context->OMSetBlendState(blend_state.Get(), blend_factor, 0xFFFFFFFF);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);

    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->IASetVertexBuffers(0, 1, quad_vertex_buffer.GetAddressOf(), &stride, &offset);
    context->IASetInputLayout(mask_input_layout_.Get());
    context->VSSetShader(mask_vertex_shader_.Get(), nullptr, 0);
    context->PSSetShader(mask_pixel_shader_.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, point_sampler_.GetAddressOf());

    // 16件を超える場合は複数パスで合成
    size_t index = 0;
    while (index < detections.size()) {
        MaskCB cb = {};
        uint32_t count = 0;
        for (; index < detections.size() && count < 16; ++index, ++count) {
            float x1 = detections[index].x1;
            float y1 = detections[index].y1;
            float x2 = detections[index].x2;
            float y2 = detections[index].y2;

            // 正規化座標 [0,1] ならピクセルへ変換
            if (x1 <= 1.0f && x2 <= 1.0f && y1 <= 1.0f && y2 <= 1.0f) {
                x1 *= width;
                y1 *= height;
                x2 *= width;
                y2 *= height;
            }

            x1 = std::clamp(x1, 0.0f, static_cast<float>(width));
            y1 = std::clamp(y1, 0.0f, static_cast<float>(height));
            x2 = std::clamp(x2, 0.0f, static_cast<float>(width));
            y2 = std::clamp(y2, 0.0f, static_cast<float>(height));

            float w = std::max(0.0f, x2 - x1);
            float h = std::max(0.0f, y2 - y1);
            if (w <= 0.0f || h <= 0.0f) {
                continue;
            }

            float cx = (x1 + x2) * 0.5f / static_cast<float>(width);
            float cy = (y1 + y2) * 0.5f / static_cast<float>(height);
            float nw = w / static_cast<float>(width);
            float nh = h / static_cast<float>(height);

            cb.rects[count][0] = cx;
            cb.rects[count][1] = cy;
            cb.rects[count][2] = nw;
            cb.rects[count][3] = nh;
        }

        cb.maskCount = count;
        if (cb.maskCount == 0) {
            continue;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = context->Map(mask_constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            continue;
        }
        memcpy(mapped.pData, &cb, sizeof(MaskCB));
        context->Unmap(mask_constant_buffer_.Get(), 0);

        context->VSSetConstantBuffers(0, 1, mask_constant_buffer_.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, mask_constant_buffer_.GetAddressOf());
        context->Draw(4, 0);
    }

    // 状態を軽く戻す
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetRenderTargets(0, nullptr, nullptr);

    return mask_texture;
}

bool MaskShader::ApplyInpaint(
    ID3D11Texture2D* source,
    ID3D11Texture2D* mask,
    uint32_t inpaint_radius,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output) {

    if (!source || !mask) return false;

    ID3D11Device* device = nullptr;
    mask_pixel_shader_->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    if (device) device->GetImmediateContext(&context);
    if (!device || !context) return false;

    // 出力テクスチャの作成
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> output_srv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output_rtv;

    D3D11_TEXTURE2D_DESC source_desc, output_desc;
    source->GetDesc(&source_desc);

    output_desc = source_desc;
    output_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> temp_output;
    HRESULT hr = device->CreateTexture2D(&output_desc, nullptr, temp_output.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !temp_output.Get()) return false;

    hr = device->CreateShaderResourceView(temp_output.Get(), nullptr, output_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreateRenderTargetView(temp_output.Get(), nullptr, output_rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !output_rtv.Get()) return false;

    // 入力SRVの作成
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mask_srv;

    hr = device->CreateShaderResourceView(source, nullptr, source_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreateShaderResourceView(mask, nullptr, mask_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    // 定数バッファの更新（InpaintConstantBuffer: inpaint_radius, tex_size, _padding[2]）
    struct InpaintCB {
        float inpaint_radius;
        float tex_size;
        uint32_t _padding[2];
    };
    InpaintCB inpaint_const{};
    inpaint_const.inpaint_radius = static_cast<float>(inpaint_radius);
    inpaint_const.tex_size = static_cast<float>(source_desc.Width);
    inpaint_const._padding[0] = 0;
    inpaint_const._padding[1] = 0;

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(blur_constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &inpaint_const, sizeof(inpaint_const));
        context->Unmap(blur_constant_buffer_.Get(), 0);
    }

    // シェーダーをセット
    context->OMSetRenderTargets(1, output_rtv.GetAddressOf(), nullptr);

    context->PSSetShader(inpaint_pixel_shader_.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, source_srv.GetAddressOf());
    context->PSSetShaderResources(1, 1, mask_srv.GetAddressOf());
    context->PSSetSamplers(0, 1, linear_sampler_.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, blur_constant_buffer_.GetAddressOf());
    context->VSSetConstantBuffers(0, 1, blur_constant_buffer_.GetAddressOf());

    // 全画面描画
    D3D11_VIEWPORT vp = {};
    vp.Width  = static_cast<float>(source_desc.Width);
    vp.Height = static_cast<float>(source_desc.Height);
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> quad;
    CreateFullScreenQuad(quad);
    context->IASetVertexBuffers(0, 1, quad.GetAddressOf(), &stride, &offset);
    context->IASetInputLayout(mask_input_layout_.Get());
    context->VSSetShader(mask_vertex_shader_.Get(), nullptr, 0);

    context->Draw(4, 0);

    // SRVを解除してoutputをバインド可能にする
    ID3D11ShaderResourceView* null_srvs[2] = { nullptr, nullptr };
    context->PSSetShaderResources(0, 2, null_srvs);
    context->Flush();

    output = temp_output;
    return true;
}

bool MaskShader::ApplyMosaic(
    ID3D11Texture2D* source,
    ID3D11Texture2D* mask,
    uint32_t mosaic_size,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output) {

    if (!source || !mask) return false;

    ID3D11Device* device = nullptr;
    mosaic_pixel_shader_.Get()->GetDevice(&device);
    if (!device) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    // 出力テクスチャの作成
    D3D11_TEXTURE2D_DESC source_desc;
    source->GetDesc(&source_desc);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> temp_output;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> output_srv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output_rtv;

    D3D11_TEXTURE2D_DESC mosaic_out_desc = source_desc;
    mosaic_out_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&mosaic_out_desc, nullptr, temp_output.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !temp_output.Get()) return false;

    hr = device->CreateShaderResourceView(temp_output.Get(), nullptr, output_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = mosaic_out_desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = device->CreateRenderTargetView(temp_output.Get(), &rtv_desc, output_rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !output_rtv.Get()) return false;

    // 入力SRVの作成
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mask_srv;

    hr = device->CreateShaderResourceView(source, nullptr, source_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreateShaderResourceView(mask, nullptr, mask_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !mask_srv.Get()) {
        MaskLogToFile("[ApplyMosaic] FAIL: mask SRV is null\n");
        return false;
    }

    // 定数バッファの更新（パラメータ1でも効果が見えるようスケーリング）
    MosaicConstantBuffer mosaic_const{};
    mosaic_const.mosaic_size = static_cast<float>(mosaic_size) * 1.25f + 1.25f;
    mosaic_const.texture_width = static_cast<float>(source_desc.Width);
    mosaic_const.texture_height = static_cast<float>(source_desc.Height);

    MaskLogFmt("[ApplyMosaic] mosaic_size=%u (scaled=%.1f) texture=%.1fx%.1f\n",
        mosaic_size, mosaic_const.mosaic_size, mosaic_const.texture_width, mosaic_const.texture_height);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(mosaic_constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &mosaic_const, sizeof(mosaic_const));
        context->Unmap(mosaic_constant_buffer_.Get(), 0);
    }

    // シェーダーをセット
    context->OMSetRenderTargets(1, output_rtv.GetAddressOf(), nullptr);

    context->PSSetShader(mosaic_pixel_shader_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* mosaic_srvs[2] = { source_srv.Get(), mask_srv.Get() };
    context->PSSetShaderResources(0, 2, mosaic_srvs);
    context->PSSetSamplers(0, 1, point_sampler_.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, mosaic_constant_buffer_.GetAddressOf());
    context->VSSetConstantBuffers(0, 1, mosaic_constant_buffer_.GetAddressOf());

    D3D11_VIEWPORT vp = {};
    vp.Width  = static_cast<float>(source_desc.Width);
    vp.Height = static_cast<float>(source_desc.Height);
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    Microsoft::WRL::ComPtr<ID3D11Buffer> quad;
    CreateFullScreenQuad(quad);
    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, quad.GetAddressOf(), &stride, &offset);
    context->IASetInputLayout(mask_input_layout_.Get());
    context->VSSetShader(mask_vertex_shader_.Get(), nullptr, 0);

    context->Draw(4, 0);

    // 描画結果が反映されるのを待つ
    context->Flush();

    output = temp_output;
    return true;
}

bool MaskShader::ApplyBlur(
    ID3D11Texture2D* source,
    ID3D11Texture2D* mask,
    uint32_t blur_radius,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output) {

    if (!source || !mask) return false;

    ID3D11Device* device = nullptr;
    blur_pixel_shader_.Get()->GetDevice(&device);
    if (!device) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    // 出力テクスチャの作成
    D3D11_TEXTURE2D_DESC source_desc;
    source->GetDesc(&source_desc);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> temp_output_a;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> temp_output_b;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_a;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_b;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_a;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_b;

    D3D11_TEXTURE2D_DESC blur_out_desc = source_desc;
    blur_out_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&blur_out_desc, nullptr, temp_output_a.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !temp_output_a.Get()) return false;
    hr = device->CreateTexture2D(&blur_out_desc, nullptr, temp_output_b.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !temp_output_b.Get()) return false;

    hr = device->CreateShaderResourceView(temp_output_a.Get(), nullptr, srv_a.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    hr = device->CreateShaderResourceView(temp_output_b.Get(), nullptr, srv_b.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = blur_out_desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = device->CreateRenderTargetView(temp_output_a.Get(), &rtv_desc, rtv_a.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !rtv_a.Get()) return false;
    hr = device->CreateRenderTargetView(temp_output_b.Get(), &rtv_desc, rtv_b.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !rtv_b.Get()) return false;

    // 入力SRVの作成
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mask_srv;

    hr = device->CreateShaderResourceView(source, nullptr, source_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreateShaderResourceView(mask, nullptr, mask_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !mask_srv.Get()) {
        MaskLogToFile("[ApplyBlur] FAIL: mask SRV is null\n");
        return false;
    }

    // ブラー半径0ならsourceをそのまま返す
    // パラメータ1でも効果が見えるようイテレーション数を+1スケーリング
    uint32_t iterations = blur_radius + 1;
    MaskLogFmt("[ApplyBlur] blur_radius=%u iterations=%u texture=%ux%u\n",
        blur_radius, iterations, source_desc.Width, source_desc.Height);
    if (blur_radius == 0) {
        output = source;
        output->AddRef();
        return true;
    }

    // 複数パスでブラーを適用（Kawase Blur方式）
    // パス0: source -> A, パス1: A -> B, パス2: B -> A, ...
    ID3D11ShaderResourceView* input_srv = source_srv.Get();
    ID3D11RenderTargetView* current_rtv = rtv_a.Get();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> last_output = temp_output_a;

    for (uint32_t i = 0; i < iterations; ++i) {
        BlurConstantBuffer blur_const{};
        float step = static_cast<float>(i + 1);
        blur_const.offset = step;
        blur_const.pixel_size_x = step / source_desc.Width;
        blur_const.pixel_size_y = step / source_desc.Height;
        blur_const.iterations = 1;

        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = context->Map(blur_constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, &blur_const, sizeof(blur_const));
            context->Unmap(blur_constant_buffer_.Get(), 0);
        }

        // 前回のRTV/SRVバインドを解除してから再バインド（同じリソースをRTVとSRVに同時バインドしない）
        ID3D11ShaderResourceView* null_srvs[2] = { nullptr, nullptr };
        context->PSSetShaderResources(0, 2, null_srvs);
        context->OMSetRenderTargets(1, &current_rtv, nullptr);

        context->PSSetShader(blur_pixel_shader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* blur_srvs[2] = { input_srv, mask_srv.Get() };
        context->PSSetShaderResources(0, 2, blur_srvs);
        context->PSSetSamplers(0, 1, linear_sampler_.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, blur_constant_buffer_.GetAddressOf());
        context->VSSetConstantBuffers(0, 1, blur_constant_buffer_.GetAddressOf());

        Microsoft::WRL::ComPtr<ID3D11Buffer> quad;
        CreateFullScreenQuad(quad);
        UINT stride = sizeof(float) * 4;
        UINT offset = 0;
        D3D11_VIEWPORT blur_vp = {};
        blur_vp.Width  = static_cast<float>(source_desc.Width);
        blur_vp.Height = static_cast<float>(source_desc.Height);
        blur_vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &blur_vp);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->IASetVertexBuffers(0, 1, quad.GetAddressOf(), &stride, &offset);
        context->IASetInputLayout(mask_input_layout_.Get());
        context->VSSetShader(mask_vertex_shader_.Get(), nullptr, 0);

        context->Draw(4, 0);

        // 今書き込んだテクスチャを最終出力として記録
        if (current_rtv == rtv_a.Get()) {
            last_output = temp_output_a;
        } else {
            last_output = temp_output_b;
        }

        // 次のパスの入力・出力を切り替え
        if (i % 2 == 0) {
            input_srv = srv_a.Get();
            current_rtv = rtv_b.Get();
        } else {
            input_srv = srv_b.Get();
            current_rtv = rtv_a.Get();
        }
    }

    // 最終結果をoutputに設定
    output = last_output;

    // 描画結果が反映されるのを待つ
    context->Flush();

    return true;
}

bool MaskShader::ApplyRectFill(
    ID3D11Texture2D* source,
    const RECT& rect,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output) {

    if (!source) return false;

    ID3D11Device* device = nullptr;
    rectfill_pixel_shader_.Get()->GetDevice(&device);
    if (!device) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) return false;

    D3D11_TEXTURE2D_DESC source_desc = {};
    source->GetDesc(&source_desc);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_srv;
    HRESULT hr = device->CreateShaderResourceView(source, nullptr, source_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> temp_output;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output_rtv;

    D3D11_TEXTURE2D_DESC out_desc = source_desc;
    out_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    hr = device->CreateTexture2D(&out_desc, nullptr, temp_output.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !temp_output.Get()) return false;

    hr = device->CreateRenderTargetView(temp_output.Get(), nullptr, output_rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !output_rtv.Get()) return false;

    // 定数バッファの設定（ピクセル座標 → UV座標に変換）
    RectFillConstantBuffer const_data{};
    const_data.rect_x = static_cast<float>(rect.left);
    const_data.rect_y = static_cast<float>(rect.top);
    const_data.rect_width = static_cast<float>(rect.right - rect.left);
    const_data.rect_height = static_cast<float>(rect.bottom - rect.top);
    
    // RGBA色を0-1の範囲で設定
    const_data.fillColor[0] = r / 255.0f;
    const_data.fillColor[1] = g / 255.0f;
    const_data.fillColor[2] = b / 255.0f;
    const_data.fillColor[3] = a / 255.0f;

    // テクスチャサイズも設定
    const_data.texture_size[0] = static_cast<float>(source_desc.Width);
    const_data.texture_size[1] = static_cast<float>(source_desc.Height);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(rectfill_constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &const_data, sizeof(const_data));
        context->Unmap(rectfill_constant_buffer_.Get(), 0);
    }

    // シェーダーをセット
    context->OMSetRenderTargets(1, output_rtv.GetAddressOf(), nullptr);
    
    context->PSSetShader(rectfill_pixel_shader_.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, source_srv.GetAddressOf());
    context->PSSetSamplers(0, 1, point_sampler_.GetAddressOf());
    context->VSSetConstantBuffers(0, 1, rectfill_constant_buffer_.GetAddressOf());

    // 四角形を描画
    Microsoft::WRL::ComPtr<ID3D11Buffer> quad;
    CreateFullScreenQuad(quad);
    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, quad.GetAddressOf(), &stride, &offset);
    context->IASetInputLayout(mask_input_layout_.Get());
    context->VSSetShader(mask_vertex_shader_.Get(), nullptr, 0);

    context->Draw(4, 0);

    output = temp_output;
    return true;
}

bool MaskShader::ApplyRectFill(
    ID3D11Texture2D* source,
    ID3D11Texture2D* mask,
    const float color[4],
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output) {

    if (!source || !mask) return false;

    ID3D11Device* device = nullptr;
    rectfill_pixel_shader_.Get()->GetDevice(&device);
    if (!device) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) return false;

    D3D11_TEXTURE2D_DESC source_desc = {};
    source->GetDesc(&source_desc);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mask_srv;
    HRESULT hr = device->CreateShaderResourceView(source, nullptr, source_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    hr = device->CreateShaderResourceView(mask, nullptr, mask_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> temp_output;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output_rtv;

    D3D11_TEXTURE2D_DESC out_desc = source_desc;
    out_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    hr = device->CreateTexture2D(&out_desc, nullptr, temp_output.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreateRenderTargetView(temp_output.Get(), nullptr, output_rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    // 定数バッファの設定
    RectFillConstantBuffer const_data{};
    const_data.rect_x = 0.0f;
    const_data.rect_y = 0.0f;
    const_data.rect_width = 0.0f;
    const_data.rect_height = 0.0f;
    const_data.fillColor[0] = color[0];
    const_data.fillColor[1] = color[1];
    const_data.fillColor[2] = color[2];
    const_data.fillColor[3] = color[3];
    const_data.texture_size[0] = static_cast<float>(source_desc.Width);
    const_data.texture_size[1] = static_cast<float>(source_desc.Height);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(rectfill_constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &const_data, sizeof(const_data));
        context->Unmap(rectfill_constant_buffer_.Get(), 0);
    }

    // マスク対応のピクセルシェーダーが必要な場合は、rectfill_pixel_shader_srcを修正
    // 現状のシェーダーは矩形判定のみなので、マスクテクスチャを追加リソースとしてバインド
    context->OMSetRenderTargets(1, output_rtv.GetAddressOf(), nullptr);
    context->PSSetShader(rectfill_pixel_shader_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { source_srv.Get(), mask_srv.Get() };
    context->PSSetShaderResources(0, 2, srvs);
    context->PSSetSamplers(0, 1, point_sampler_.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, rectfill_constant_buffer_.GetAddressOf());
    context->VSSetConstantBuffers(0, 1, rectfill_constant_buffer_.GetAddressOf());

    Microsoft::WRL::ComPtr<ID3D11Buffer> quad;
    CreateFullScreenQuad(quad);
    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    D3D11_VIEWPORT vp = {};
    vp.Width  = static_cast<float>(source_desc.Width);
    vp.Height = static_cast<float>(source_desc.Height);
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->IASetVertexBuffers(0, 1, quad.GetAddressOf(), &stride, &offset);
    context->IASetInputLayout(mask_input_layout_.Get());
    context->VSSetShader(mask_vertex_shader_.Get(), nullptr, 0);

    context->Draw(4, 0);

    output = temp_output;
    return true;
}

bool MaskShader::ApplyRectMosaic(
    ID3D11Texture2D* source,
    const RECT& rect,
    uint32_t mosaic_size,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output) {

    if (!source) return false;

    ID3D11Device* device = nullptr;
    mosaic_pixel_shader_.Get()->GetDevice(&device);
    if (!device) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    // 出力テクスチャの作成
    D3D11_TEXTURE2D_DESC source_desc;
    source->GetDesc(&source_desc);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> temp_output;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> output_srv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output_rtv;

    D3D11_TEXTURE2D_DESC mosaic_out_desc = source_desc;
    mosaic_out_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&mosaic_out_desc, nullptr, temp_output.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !temp_output.Get()) return false;

    hr = device->CreateShaderResourceView(temp_output.Get(), nullptr, output_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = mosaic_out_desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = device->CreateRenderTargetView(temp_output.Get(), &rtv_desc, output_rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !output_rtv.Get()) return false;

    // 入力SRVの作成
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_srv;
    hr = device->CreateShaderResourceView(source, nullptr, source_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    // 定数バッファの更新（矩形情報を設定）
    RectFillConstantBuffer const_data{};
    const_data.rect_x = static_cast<float>(rect.left);
    const_data.rect_y = static_cast<float>(rect.top);
    const_data.rect_width = static_cast<float>(rect.right - rect.left);
    const_data.rect_height = static_cast<float>(rect.bottom - rect.top);
    const_data.fillColor[0] = 1.0f; // モザイク用（無視される）
    const_data.fillColor[1] = 1.0f;
    const_data.fillColor[2] = 1.0f;
    const_data.fillColor[3] = 1.0f;
    const_data.texture_size[0] = static_cast<float>(source_desc.Width);
    const_data.texture_size[1] = static_cast<float>(source_desc.Height);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(rectfill_constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &const_data, sizeof(const_data));
        context->Unmap(rectfill_constant_buffer_.Get(), 0);
    }

    // モザイク定数バッファも更新（パラメータ1でも効果が見えるようスケーリング）
    MosaicConstantBuffer mosaic_const{};
    mosaic_const.mosaic_size = static_cast<float>(mosaic_size) * 1.25f + 1.25f;
    mosaic_const.texture_width = static_cast<float>(source_desc.Width);
    mosaic_const.texture_height = static_cast<float>(source_desc.Height);

    hr = context->Map(mosaic_constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &mosaic_const, sizeof(mosaic_const));
        context->Unmap(mosaic_constant_buffer_.Get(), 0);
    }

    // シェーダーをセット（四角形内のみモザイク）
    context->OMSetRenderTargets(1, output_rtv.GetAddressOf(), nullptr);
    
    context->PSSetShader(mosaic_pixel_shader_.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, source_srv.GetAddressOf());
    context->PSSetSamplers(0, 1, point_sampler_.GetAddressOf());
    context->VSSetConstantBuffers(0, 1, rectfill_constant_buffer_.GetAddressOf());

    // 四角形を描画
    Microsoft::WRL::ComPtr<ID3D11Buffer> quad;
    CreateFullScreenQuad(quad);
    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, quad.GetAddressOf(), &stride, &offset);
    context->IASetInputLayout(mask_input_layout_.Get());
    context->VSSetShader(mask_vertex_shader_.Get(), nullptr, 0);

    context->Draw(4, 0);

    output = temp_output;
    return true;
}

bool MaskShader::ApplyRectBlur(
    ID3D11Texture2D* source,
    const RECT& rect,
    uint32_t blur_radius,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output) {

    if (!source) return false;

    ID3D11Device* device = nullptr;
    blur_pixel_shader_.Get()->GetDevice(&device);
    if (!device) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    // 出力テクスチャの作成
    D3D11_TEXTURE2D_DESC source_desc;
    source->GetDesc(&source_desc);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> temp_output;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> output_srv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output_rtv;

    D3D11_TEXTURE2D_DESC blur_out_desc = source_desc;
    blur_out_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&blur_out_desc, nullptr, temp_output.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !temp_output.Get()) return false;

    hr = device->CreateShaderResourceView(temp_output.Get(), nullptr, output_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = blur_out_desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = device->CreateRenderTargetView(temp_output.Get(), &rtv_desc, output_rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !output_rtv.Get()) return false;

    // 入力SRVの作成
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_srv;
    hr = device->CreateShaderResourceView(source, nullptr, source_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    // 一時テクスチャの確保
    if (!blur_temp_texture_.Get()) {
        D3D11_TEXTURE2D_DESC temp_desc = source_desc;
        temp_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        device->CreateTexture2D(&temp_desc, nullptr, blur_temp_texture_.ReleaseAndGetAddressOf());
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> temp_rtv;
    D3D11_RENDER_TARGET_VIEW_DESC temp_rtv_desc = rtv_desc;
    device->CreateRenderTargetView(blur_temp_texture_.Get(), &temp_rtv_desc, temp_rtv.ReleaseAndGetAddressOf());

    // 定数バッファの更新（矩形情報）
    RectFillConstantBuffer const_data{};
    const_data.rect_x = static_cast<float>(rect.left);
    const_data.rect_y = static_cast<float>(rect.top);
    const_data.rect_width = static_cast<float>(rect.right - rect.left);
    const_data.rect_height = static_cast<float>(rect.bottom - rect.top);
    const_data.fillColor[0] = 1.0f;
    const_data.fillColor[1] = 1.0f;
    const_data.fillColor[2] = 1.0f;
    const_data.fillColor[3] = 1.0f;
    const_data.texture_size[0] = static_cast<float>(source_desc.Width);
    const_data.texture_size[1] = static_cast<float>(source_desc.Height);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(rectfill_constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &const_data, sizeof(const_data));
        context->Unmap(rectfill_constant_buffer_.Get(), 0);
    }

    // Kawase Blurを適用（Kawase Blurは全画面必要のため、矩形クリッピング付きで実行）
    uint32_t iterations = blur_radius;
    
    ID3D11ShaderResourceView* input_srv = source_srv.Get();
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>* output_target = &output_rtv;

    for (uint32_t i = 0; i < iterations; ++i) {
        BlurConstantBuffer blur_const{};
        float step = static_cast<float>(i + 1);
        blur_const.offset = step;
        blur_const.pixel_size_x = step / source_desc.Width;
        blur_const.pixel_size_y = step / source_desc.Height;
        blur_const.iterations = 1;

        hr = context->Map(blur_constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, &blur_const, sizeof(blur_const));
            context->Unmap(blur_constant_buffer_.Get(), 0);
        }

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>* current_rtv = (i % 2 == 0) ? &output_rtv : &temp_rtv;
        
        context->OMSetRenderTargets(1, current_rtv->GetAddressOf(), nullptr);
        
        context->PSSetShader(blur_pixel_shader_.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &input_srv);
        context->PSSetSamplers(0, 1, linear_sampler_.GetAddressOf());
        context->VSSetConstantBuffers(0, 1, blur_constant_buffer_.GetAddressOf());

        Microsoft::WRL::ComPtr<ID3D11Buffer> quad;
        CreateFullScreenQuad(quad);
        UINT stride = sizeof(float) * 4;
        UINT offset = 0;
        context->IASetVertexBuffers(0, 1, quad.GetAddressOf(), &stride, &offset);
        context->IASetInputLayout(mask_input_layout_.Get());
        context->VSSetShader(mask_vertex_shader_.Get(), nullptr, 0);

        context->Draw(4, 0);

        if (i % 2 == 0) {
            output_srv.Reset();
            device->CreateShaderResourceView(blur_temp_texture_.Get(), nullptr, output_srv.ReleaseAndGetAddressOf());
            input_srv = output_srv.Get();
        } else {
            source_srv.Reset();
            device->CreateShaderResourceView(temp_output.Get(), nullptr, source_srv.ReleaseAndGetAddressOf());
            input_srv = source_srv.Get();
        }
    }

    if (iterations % 2 == 1) {
        context->CopyResource(temp_output.Get(), blur_temp_texture_.Get());
    }

    output = temp_output;
    return true;
}

bool MaskShader::ApplyCopyrightOverlay(
    ID3D11Texture2D* source,
    ID3D11ShaderResourceView* watermark_srv,
    uint32_t wm_width, uint32_t wm_height,
    int pos_x, int pos_y,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output) {

    if (!source || !watermark_srv) return false;

    ID3D11Device* device = nullptr;
    copyright_pixel_shader_.Get()->GetDevice(&device);
    if (!device) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) return false;

    D3D11_TEXTURE2D_DESC source_desc = {};
    source->GetDesc(&source_desc);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_srv;
    HRESULT hr = device->CreateShaderResourceView(source, nullptr, source_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> temp_output;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output_rtv;

    D3D11_TEXTURE2D_DESC out_desc = source_desc;
    out_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    hr = device->CreateTexture2D(&out_desc, nullptr, temp_output.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !temp_output.Get()) return false;

    hr = device->CreateRenderTargetView(temp_output.Get(), nullptr, output_rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !output_rtv.Get()) return false;

    // 定数バッファの設定（ピクセル座標）
    CopyrightConstantBuffer const_data{};
    const_data.wm_x = static_cast<float>(pos_x);
    const_data.wm_y = static_cast<float>(pos_y);
    const_data.wm_width = static_cast<float>(wm_width);
    const_data.wm_height = static_cast<float>(wm_height);
    const_data.texture_width = static_cast<float>(source_desc.Width);
    const_data.texture_height = static_cast<float>(source_desc.Height);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(copyright_constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &const_data, sizeof(const_data));
        context->Unmap(copyright_constant_buffer_.Get(), 0);
    }

    // シェーダーをセット
    context->OMSetRenderTargets(1, output_rtv.GetAddressOf(), nullptr);
    
    context->PSSetShader(copyright_pixel_shader_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* watermark_resource = watermark_srv;
    context->PSSetShaderResources(0, 1, source_srv.GetAddressOf());      // t0: 元画像
    context->PSSetShaderResources(1, 1, &watermark_resource);             // t1: 透かし画像
    context->PSSetSamplers(0, 1, linear_sampler_.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, copyright_constant_buffer_.GetAddressOf());
    context->VSSetConstantBuffers(0, 1, copyright_constant_buffer_.GetAddressOf());

    // 四角形を描画
    Microsoft::WRL::ComPtr<ID3D11Buffer> quad;
    CreateFullScreenQuad(quad);
    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, quad.GetAddressOf(), &stride, &offset);
    context->IASetInputLayout(mask_input_layout_.Get());
    context->VSSetShader(mask_vertex_shader_.Get(), nullptr, 0);

    context->Draw(4, 0);

    output = temp_output;
    return true;
}

bool MaskShader::CompositeWithMask(
    ID3D11Texture2D* base,
    ID3D11Texture2D* overlay,
    ID3D11Texture2D* mask,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output) {

    if (!base || !overlay || !mask) return false;

    ID3D11Device* device = nullptr;
    composite_pixel_shader_->GetDevice(&device);
    if (!device) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) return false;

    D3D11_TEXTURE2D_DESC base_desc = {};
    base->GetDesc(&base_desc);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> temp_output;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output_rtv;

    D3D11_TEXTURE2D_DESC out_desc = base_desc;
    out_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&out_desc, nullptr, temp_output.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !temp_output.Get()) return false;

    hr = device->CreateRenderTargetView(temp_output.Get(), nullptr, output_rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !output_rtv.Get()) return false;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> base_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> overlay_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mask_srv;

    hr = device->CreateShaderResourceView(base, nullptr, base_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    hr = device->CreateShaderResourceView(overlay, nullptr, overlay_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    hr = device->CreateShaderResourceView(mask, nullptr, mask_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    context->OMSetRenderTargets(1, output_rtv.GetAddressOf(), nullptr);

    context->PSSetShader(composite_pixel_shader_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* composite_srvs[3] = { base_srv.Get(), overlay_srv.Get(), mask_srv.Get() };
    context->PSSetShaderResources(0, 3, composite_srvs);
    context->PSSetSamplers(0, 1, linear_sampler_.GetAddressOf());

    Microsoft::WRL::ComPtr<ID3D11Buffer> quad;
    CreateFullScreenQuad(quad);
    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(base_desc.Width);
    vp.Height = static_cast<float>(base_desc.Height);
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->IASetVertexBuffers(0, 1, quad.GetAddressOf(), &stride, &offset);
    context->IASetInputLayout(mask_input_layout_.Get());
    context->VSSetShader(mask_vertex_shader_.Get(), nullptr, 0);

    context->Draw(4, 0);

    // SRVを解除
    ID3D11ShaderResourceView* null_srvs[3] = { nullptr, nullptr, nullptr };
    context->PSSetShaderResources(0, 3, null_srvs);
    context->Flush();

    output = temp_output;
    return true;
}

bool MaskShader::ConvertNV12ToBGRA(
    ID3D11Texture2D* nv12_y,
    ID3D11Texture2D* nv12_uv,
    uint32_t width,
    uint32_t height,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output) {

    if (!nv12_y || !nv12_uv) return false;

    ID3D11Device* device = nullptr;
    nv12_to_bgra_pixel_shader_->GetDevice(&device);
    if (!device) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) return false;

    // 出力テクスチャの作成（BGRA）
    Microsoft::WRL::ComPtr<ID3D11Texture2D> temp_output;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output_rtv;

    D3D11_TEXTURE2D_DESC out_desc = {};
    out_desc.Width = width;
    out_desc.Height = height;
    out_desc.MipLevels = 1;
    out_desc.ArraySize = 1;
    out_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    out_desc.SampleDesc.Count = 1;
    out_desc.Usage = D3D11_USAGE_DEFAULT;
    out_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    HRESULT hr = device->CreateTexture2D(&out_desc, nullptr, temp_output.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !temp_output.Get()) return false;

    hr = device->CreateRenderTargetView(temp_output.Get(), nullptr, output_rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !output_rtv.Get()) return false;

    // 入力SRVの作成
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uv_srv;

    D3D11_SHADER_RESOURCE_VIEW_DESC y_srv_desc = {};
    y_srv_desc.Format = DXGI_FORMAT_R8_UNORM;
    y_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    y_srv_desc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(nv12_y, &y_srv_desc, y_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC uv_srv_desc = {};
    uv_srv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    uv_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    uv_srv_desc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(nv12_uv, &uv_srv_desc, uv_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    // シェーダーをセット
    context->OMSetRenderTargets(1, output_rtv.GetAddressOf(), nullptr);
    context->PSSetShader(nv12_to_bgra_pixel_shader_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { y_srv.Get(), uv_srv.Get() };
    context->PSSetShaderResources(0, 2, srvs);
    context->PSSetSamplers(0, 1, point_sampler_.GetAddressOf());

    Microsoft::WRL::ComPtr<ID3D11Buffer> quad;
    CreateFullScreenQuad(quad);
    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->IASetVertexBuffers(0, 1, quad.GetAddressOf(), &stride, &offset);
    context->IASetInputLayout(mask_input_layout_.Get());
    context->VSSetShader(mask_vertex_shader_.Get(), nullptr, 0);

    context->Draw(4, 0);

    // SRVを解除
    ID3D11ShaderResourceView* null_srvs[2] = { nullptr, nullptr };
    context->PSSetShaderResources(0, 2, null_srvs);
    context->Flush();

    output = temp_output;
    return true;
}

bool MaskShader::ConvertBGRAToNV12(
    ID3D11Texture2D* bgra,
    uint32_t width,
    uint32_t height,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output_y,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& output_uv) {

    if (!bgra) return false;

    ID3D11Device* device = nullptr;
    bgra_to_nv12_y_pixel_shader_->GetDevice(&device);
    if (!device) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) return false;

    // 入力SRVの作成
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> bgra_srv;
    HRESULT hr = device->CreateShaderResourceView(bgra, nullptr, bgra_srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    // Y平面出力テクスチャ（R8_UNORM）
    Microsoft::WRL::ComPtr<ID3D11Texture2D> y_texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> y_rtv;

    D3D11_TEXTURE2D_DESC y_desc = {};
    y_desc.Width = width;
    y_desc.Height = height;
    y_desc.MipLevels = 1;
    y_desc.ArraySize = 1;
    y_desc.Format = DXGI_FORMAT_R8_UNORM;
    y_desc.SampleDesc.Count = 1;
    y_desc.Usage = D3D11_USAGE_DEFAULT;
    y_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    hr = device->CreateTexture2D(&y_desc, nullptr, y_texture.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !y_texture.Get()) return false;

    hr = device->CreateRenderTargetView(y_texture.Get(), nullptr, y_rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !y_rtv.Get()) return false;

    // UV平面出力テクスチャ（R8G8_UNORM、半解像度）
    Microsoft::WRL::ComPtr<ID3D11Texture2D> uv_texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> uv_rtv;

    D3D11_TEXTURE2D_DESC uv_desc = {};
    uv_desc.Width = (width + 1) / 2;
    uv_desc.Height = (height + 1) / 2;
    uv_desc.MipLevels = 1;
    uv_desc.ArraySize = 1;
    uv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    uv_desc.SampleDesc.Count = 1;
    uv_desc.Usage = D3D11_USAGE_DEFAULT;
    uv_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    hr = device->CreateTexture2D(&uv_desc, nullptr, uv_texture.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !uv_texture.Get()) return false;

    hr = device->CreateRenderTargetView(uv_texture.Get(), nullptr, uv_rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !uv_rtv.Get()) return false;

    // Y平面レンダリング
    context->OMSetRenderTargets(1, y_rtv.GetAddressOf(), nullptr);
    context->PSSetShader(bgra_to_nv12_y_pixel_shader_.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, bgra_srv.GetAddressOf());
    context->PSSetSamplers(0, 1, point_sampler_.GetAddressOf());

    Microsoft::WRL::ComPtr<ID3D11Buffer> quad;
    CreateFullScreenQuad(quad);
    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    D3D11_VIEWPORT vp_y = {};
    vp_y.Width = static_cast<float>(width);
    vp_y.Height = static_cast<float>(height);
    vp_y.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp_y);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->IASetVertexBuffers(0, 1, quad.GetAddressOf(), &stride, &offset);
    context->IASetInputLayout(mask_input_layout_.Get());
    context->VSSetShader(mask_vertex_shader_.Get(), nullptr, 0);

    context->Draw(4, 0);

    // UV平面レンダリング（半解像度ビューポート）
    context->OMSetRenderTargets(1, uv_rtv.GetAddressOf(), nullptr);
    context->PSSetShader(bgra_to_nv12_uv_pixel_shader_.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, bgra_srv.GetAddressOf());
    context->PSSetSamplers(0, 1, linear_sampler_.GetAddressOf());

    D3D11_VIEWPORT vp_uv = {};
    vp_uv.Width = static_cast<float>(uv_desc.Width);
    vp_uv.Height = static_cast<float>(uv_desc.Height);
    vp_uv.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp_uv);

    context->Draw(4, 0);

    // SRVを解除
    ID3D11ShaderResourceView* null_srv = nullptr;
    context->PSSetShaderResources(0, 1, &null_srv);
    context->Flush();

    output_y = y_texture;
    output_uv = uv_texture;
    return true;
}

bool MaskShader::CompositeNV12Planes(
    ID3D11Texture2D* y_plane,
    ID3D11Texture2D* uv_plane,
    ID3D11Texture2D* nv12_output,
    uint32_t width,
    uint32_t height) {

    if (!y_plane || !uv_plane || !nv12_output) return false;

    ID3D11Device* device = nullptr;
    mask_vertex_shader_->GetDevice(&device);
    if (!device) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) return false;

    // Y平面をNV12テクスチャの先頭にコピー
    context->CopySubresourceRegion(nv12_output, 0, 0, 0, 0, y_plane, 0, nullptr);

    // UV平面をNV12テクスチャの下半分にコピー
    // NV12: Y = 0..(height-1), UV = height..(height*1.5-1)
    D3D11_BOX uv_box = {};
    uv_box.left = 0;
    uv_box.top = 0;
    uv_box.front = 0;
    uv_box.right = (width + 1) / 2;
    uv_box.bottom = (height + 1) / 2;
    uv_box.back = 1;

    context->CopySubresourceRegion(nv12_output, 0, 0, height, 0, uv_plane, 0, &uv_box);
    context->Flush();

    return true;
}

} // namespace WoLNamesBlackedOut::Core
