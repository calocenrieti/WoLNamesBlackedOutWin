#include "pch.h"
#include "HardwareAcceleratorChecker.h"
#include <dxgi1_4.h>
#include <string>
#include <algorithm>
#include <iostream>

namespace WoLNamesBlackedOut::Core {

static std::string WideToUtf8Local(const wchar_t* wstr) {
    if (!wstr || wstr[0] == L'\0') return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], len, nullptr, nullptr);
    return result;
}

HardwareAcceleratorChecker::HardwareAcceleratorChecker() = default;
HardwareAcceleratorChecker::~HardwareAcceleratorChecker() = default;

void HardwareAcceleratorChecker::InitializeIfNeeded() const {
    if (!initialized_) {
        std::lock_guard<std::mutex> lock(init_mutex_);
        if (!initialized_) {
            const_cast<HardwareAcceleratorChecker*>(this)->CheckGpuCapabilities();
            const_cast<HardwareAcceleratorChecker*>(this)->CheckNpuCapabilities();
            initialized_ = true;
        }
    }
}

void HardwareAcceleratorChecker::CheckGpuCapabilities() {
    // DXGIアダプターを取得してGPU情報を取得
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        for (int i = 0; ; i++) {
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            if (FAILED(factory->EnumAdapters(i, &adapter))) break;

            DXGI_ADAPTER_DESC desc;
            adapter->GetDesc(&desc);

            // ソフトウェアアダプター（WARP等）を除外
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter1;
            if (SUCCEEDED(adapter.As(&adapter1))) {
                DXGI_ADAPTER_DESC1 desc1;
                adapter1->GetDesc1(&desc1);
                if ((desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                    continue;
                }
            }

            gpu_available_ = true;

            // GPU名からベンダーを判定（既存のGpuVendorと整合性を取る）
            wchar_t vendorLower[128];
            size_t len = wcslen(desc.Description);
            if (len < _countof(vendorLower)) {
                wcsncpy_s(vendorLower, desc.Description, _countof(vendorLower));
                for (auto& c : vendorLower) c = towlower(c);
            }

            if (wcsstr(vendorLower, L"nvidia")) {
                gpu_vendor_name_ = "NVIDIA";
                break;
            } else if (wcsstr(vendorLower, L"amd") || wcsstr(vendorLower, L"advanced micro devices")) {
                gpu_vendor_name_ = "AMD";
                break;
            } else if (wcsstr(vendorLower, L"intel")) {
                gpu_vendor_name_ = "Intel";
                break;
            } else if (gpu_vendor_name_.empty()) {
                gpu_vendor_name_ = WideToUtf8Local(desc.Description);
            }
        }
    }

    // D3D11デバイス作成を試みる（Compute Shader利用可否の判定）
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };
    int numFeatureLevels = _countof(featureLevels);

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_9_1;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevels,
        numFeatureLevels,
        D3D11_SDK_VERSION,
        &device,
        &featureLevel,
        &context
    );

    if (SUCCEEDED(hr) && device) {
        compute_shader_available_ = (featureLevel >= D3D_FEATURE_LEVEL_11_0);
    } else {
        compute_shader_available_ = false;
    }
}

void HardwareAcceleratorChecker::CheckNpuCapabilities() {
    // 簡易判定: 実際の EP 有効化は WinML EP catalog の EnsureEpReady で行う
    npu_available_ = false;
}

HardwareAcceleratorChecker::Engine HardwareAcceleratorChecker::GetRecommendedEngine() const {
    InitializeIfNeeded();

    if (compute_shader_available_) {
        return Engine::GPU;
    }
    if (npu_available_) {
        return Engine::NPU;
    }
    return Engine::CPU;
}

std::string HardwareAcceleratorChecker::GetGpuVendorName() const {
    InitializeIfNeeded();
    return gpu_vendor_name_;
}

bool HardwareAcceleratorChecker::IsNpuAvailable() const {
    InitializeIfNeeded();
    return npu_available_;
}

} // namespace WoLNamesBlackedOut::Core

