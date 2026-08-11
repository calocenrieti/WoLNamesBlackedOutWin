#include "pch.h"
#include "PreprocessShader.h"
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#include <algorithm>
#include <cmath>

namespace WoLNamesBlackedOut::Core {

// ============================================================
// Compute Shader: BGRA → NCHW float (resize + letterbox + normalize)
// 参考: microsoft/DirectML/Samples/yolov4/Assets/ImageToTensor.hlsl
// ============================================================
static const char* preprocess_compute_shader_src =
    "Texture2D<float4> srcImage : register(t0);\n"
    "SamplerState samLinear : register(s0);\n"
    "RWBuffer<float> dstTensor : register(u0);\n"
    "\n"
    "cbuffer PreprocessParams : register(b0) {\n"
    "    uint srcWidth;\n"
    "    uint srcHeight;\n"
    "    uint dstWidth;\n"
    "    uint dstHeight;\n"
    "    float padLeft;\n"
    "    float padTop;\n"
    "    float resizeRatio;\n"
    "    float inv255;\n"
    "};\n"
    "\n"
    "[numthreads(16, 16, 1)]\n"
    "void CSMain(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstWidth || id.y >= dstHeight)\n"
    "        return;\n"
    "\n"
    "    float dstX = float(id.x);\n"
    "    float dstY = float(id.y);\n"
    "\n"
    "    // Letterbox: map output pixel to source image coordinates\n"
    "    float srcX = (dstX - padLeft) / resizeRatio;\n"
    "    float srcY = (dstY - padTop) / resizeRatio;\n"
    "\n"
    "    float4 color;\n"
    "    if (srcX < 0.0f || srcX >= float(srcWidth) || srcY < 0.0f || srcY >= float(srcHeight)) {\n"
    "        // Black padding to match CPU fallback path\n"
    "        color = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
    "    } else {\n"
    "        // Bilinear sample from source (direct mapping to match swscale)\n"
    "        float2 uv = float2(srcX / float(srcWidth), srcY / float(srcHeight));\n"
    "        color = srcImage.SampleLevel(samLinear, uv, 0.0f);\n"
    "    }\n"
    "\n"
    "    // BGRA input → RGB (UNORM texture already returns [0,1])\n"
    "    float r = color.b;\n"
    "    float g = color.g;\n"
    "    float b = color.r;\n"
    "\n"
    "    // Write NCHW layout: [batch=0][channel][y][x]\n"
    "    uint planeSize = dstWidth * dstHeight;\n"
    "    uint baseIdx = id.y * dstWidth + id.x;\n"
    "\n"
    "    dstTensor[baseIdx]              = r;\n"
    "    dstTensor[baseIdx + planeSize]  = g;\n"
    "    dstTensor[baseIdx + planeSize * 2] = b;\n"
    "}\n";

PreprocessShader::PreprocessShader() = default;
PreprocessShader::~PreprocessShader() {
    Release();
}

bool PreprocessShader::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return false;
    device_ = device;
    context_ = context;

    // Compile compute shader
    Microsoft::WRL::ComPtr<ID3DBlob> csBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(
        preprocess_compute_shader_src,
        strlen(preprocess_compute_shader_src),
        "PreprocessCS",
        nullptr,
        nullptr,
        "CSMain",
        "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &csBlob,
        &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA("[PreprocessShader] CS compile error: ");
            OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
            OutputDebugStringA("\n");
        }
        return false;
    }

    hr = device_->CreateComputeShader(
        csBlob->GetBufferPointer(),
        csBlob->GetBufferSize(),
        nullptr,
        &cs_);
    if (FAILED(hr)) {
        OutputDebugStringA("[PreprocessShader] CreateComputeShader failed\n");
        return false;
    }

    // Create constant buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = ((sizeof(UINT) * 4 + sizeof(float) * 4) + 15) & ~15; // 16-byte aligned
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&cbDesc, nullptr, &constantBuffer_);
    if (FAILED(hr)) {
        OutputDebugStringA("[PreprocessShader] CreateBuffer (CB) failed\n");
        return false;
    }

    // Create linear sampler (BORDER with black to match CPU fallback padding)
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sampDesc.BorderColor[0] = 0.0f;
    sampDesc.BorderColor[1] = 0.0f;
    sampDesc.BorderColor[2] = 0.0f;
    sampDesc.BorderColor[3] = 1.0f;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sampDesc, &samplerState_);
    if (FAILED(hr)) {
        OutputDebugStringA("[PreprocessShader] CreateSamplerState failed\n");
        return false;
    }

    initialized_ = true;
    OutputDebugStringA("[PreprocessShader] Initialized OK\n");
    return true;
}

void PreprocessShader::Release() {
    cs_.Reset();
    constantBuffer_.Reset();
    samplerState_.Reset();
    uavBuffer_.Reset();
    uav_.Reset();
    stagingBuffer_.Reset();
    device_.Reset();
    context_.Reset();
    uavBufferSize_ = 0;
    stagingBufferSize_ = 0;
    initialized_ = false;
}

bool PreprocessShader::EnsureUavBuffer(uint32_t dstWidth, uint32_t dstHeight) {
    uint32_t requiredFloats = dstWidth * dstHeight * 3; // NCHW: 3 channels
    uint32_t requiredBytes = requiredFloats * sizeof(float);

    if (uavBuffer_ && uavBufferSize_ >= requiredBytes) {
        return true;
    }

    D3D11_BUFFER_DESC bufDesc = {};
    bufDesc.ByteWidth = requiredBytes;
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = device_->CreateBuffer(&bufDesc, nullptr, &uavBuffer_);
    if (FAILED(hr)) {
        OutputDebugStringA("[PreprocessShader] CreateBuffer (UAV) failed\n");
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = requiredFloats;

    hr = device_->CreateUnorderedAccessView(uavBuffer_.Get(), &uavDesc, &uav_);
    if (FAILED(hr)) {
        OutputDebugStringA("[PreprocessShader] CreateUnorderedAccessView failed\n");
        return false;
    }

    uavBufferSize_ = requiredBytes;
    return true;
}

bool PreprocessShader::EnsureStagingBuffer(uint32_t dstWidth, uint32_t dstHeight) {
    uint32_t requiredBytes = dstWidth * dstHeight * 3 * sizeof(float);

    if (stagingBuffer_ && stagingBufferSize_ >= requiredBytes) {
        return true;
    }

    D3D11_BUFFER_DESC bufDesc = {};
    bufDesc.ByteWidth = requiredBytes;
    bufDesc.Usage = D3D11_USAGE_STAGING;
    bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = device_->CreateBuffer(&bufDesc, nullptr, &stagingBuffer_);
    if (FAILED(hr)) {
        OutputDebugStringA("[PreprocessShader] CreateBuffer (staging) failed\n");
        return false;
    }

    stagingBufferSize_ = requiredBytes;
    return true;
}

bool PreprocessShader::Process(
    ID3D11Texture2D* srcTexture,
    uint32_t srcWidth,
    uint32_t srcHeight,
    uint32_t dstWidth,
    uint32_t dstHeight,
    std::vector<float>& outputBuffer) {

    if (!initialized_ || !srcTexture) return false;

    // Create SRV for source texture
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device_->CreateShaderResourceView(srcTexture, &srvDesc, &srv);
    if (FAILED(hr)) {
        OutputDebugStringA("[PreprocessShader] CreateShaderResourceView failed\n");
        return false;
    }

    // Ensure output buffers
    if (!EnsureUavBuffer(dstWidth, dstHeight)) return false;
    if (!EnsureStagingBuffer(dstWidth, dstHeight)) return false;

    // Calculate resize parameters (top-left aligned to match CPU fallback)
    float ratioW = static_cast<float>(dstWidth) / static_cast<float>(srcWidth);
    float ratioH = static_cast<float>(dstHeight) / static_cast<float>(srcHeight);
    float ratio = std::min(ratioW, ratioH);
    float padX = 0.0f;
    float padY = 0.0f;

    // Update constant buffer
    struct Params {
        UINT srcWidth;
        UINT srcHeight;
        UINT dstWidth;
        UINT dstHeight;
        float padLeft;
        float padTop;
        float resizeRatio;
        float inv255;
    } params = {
        srcWidth, srcHeight,
        dstWidth, dstHeight,
        padX, padY,
        ratio,
        1.0f / 255.0f
    };

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context_->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &params, sizeof(params));
        context_->Unmap(constantBuffer_.Get(), 0);
    }

    // Bind and dispatch
    context_->CSSetShader(cs_.Get(), nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
    context_->CSSetSamplers(0, 1, samplerState_.GetAddressOf());
    context_->CSSetShaderResources(0, 1, srv.GetAddressOf());
    context_->CSSetUnorderedAccessViews(0, 1, uav_.GetAddressOf(), nullptr);

    UINT groupsX = (dstWidth + 15) / 16;
    UINT groupsY = (dstHeight + 15) / 16;
    context_->Dispatch(groupsX, groupsY, 1);

    // Unbind UAV
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    context_->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);

    // Copy to staging buffer
    context_->CopyResource(stagingBuffer_.Get(), uavBuffer_.Get());

    // Map staging buffer and copy to CPU vector
    hr = context_->Map(stagingBuffer_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        OutputDebugStringA("[PreprocessShader] Map staging buffer failed\n");
        return false;
    }

    size_t requiredFloats = static_cast<size_t>(dstWidth) * dstHeight * 3;
    outputBuffer.resize(requiredFloats);
    memcpy(outputBuffer.data(), mapped.pData, requiredFloats * sizeof(float));
    context_->Unmap(stagingBuffer_.Get(), 0);

    return true;
}

bool PreprocessShader::ProcessToGpuBuffer(
    ID3D11Texture2D* srcTexture,
    uint32_t srcWidth,
    uint32_t srcHeight,
    uint32_t dstWidth,
    uint32_t dstHeight,
    Microsoft::WRL::ComPtr<ID3D11Buffer>& outputBuffer,
    size_t& outputSize) {

    if (!initialized_ || !srcTexture) return false;

    // Create SRV for source texture
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device_->CreateShaderResourceView(srcTexture, &srvDesc, &srv);
    if (FAILED(hr)) {
        OutputDebugStringA("[PreprocessShader] CreateShaderResourceView failed\n");
        return false;
    }

    // Ensure/create output GPU buffer
    uint32_t requiredFloats = dstWidth * dstHeight * 3;
    uint32_t requiredBytes = requiredFloats * sizeof(float);

    if (!outputBuffer) {
        D3D11_BUFFER_DESC bufDesc = {};
        bufDesc.ByteWidth = requiredBytes;
        bufDesc.Usage = D3D11_USAGE_DEFAULT;
        bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

        hr = device_->CreateBuffer(&bufDesc, nullptr, &outputBuffer);
        if (FAILED(hr)) {
            OutputDebugStringA("[PreprocessShader] CreateBuffer (output) failed\n");
            return false;
        }
    }

    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputUav;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = requiredFloats;

    hr = device_->CreateUnorderedAccessView(outputBuffer.Get(), &uavDesc, &outputUav);
    if (FAILED(hr)) {
        OutputDebugStringA("[PreprocessShader] CreateUnorderedAccessView (output) failed\n");
        return false;
    }

    // Calculate resize parameters (top-left aligned to match CPU fallback)
    float ratioW = static_cast<float>(dstWidth) / static_cast<float>(srcWidth);
    float ratioH = static_cast<float>(dstHeight) / static_cast<float>(srcHeight);
    float ratio = std::min(ratioW, ratioH);
    float padX = 0.0f;
    float padY = 0.0f;

    // Update constant buffer
    struct Params {
        UINT srcWidth;
        UINT srcHeight;
        UINT dstWidth;
        UINT dstHeight;
        float padLeft;
        float padTop;
        float resizeRatio;
        float inv255;
    } params = {
        srcWidth, srcHeight,
        dstWidth, dstHeight,
        padX, padY,
        ratio,
        1.0f / 255.0f
    };

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context_->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &params, sizeof(params));
        context_->Unmap(constantBuffer_.Get(), 0);
    }

    // Bind and dispatch
    context_->CSSetShader(cs_.Get(), nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
    context_->CSSetSamplers(0, 1, samplerState_.GetAddressOf());
    context_->CSSetShaderResources(0, 1, srv.GetAddressOf());
    context_->CSSetUnorderedAccessViews(0, 1, outputUav.GetAddressOf(), nullptr);

    UINT groupsX = (dstWidth + 15) / 16;
    UINT groupsY = (dstHeight + 15) / 16;
    context_->Dispatch(groupsX, groupsY, 1);

    // Unbind UAV
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    context_->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);

    outputSize = requiredBytes;
    return true;
}

} // namespace WoLNamesBlackedOut::Core
