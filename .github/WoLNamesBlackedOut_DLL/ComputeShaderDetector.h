#pragma once

#include <d3d11.h>

namespace WoLNamesBlackedOut::Core {

// Compute Shaderの利用可否を判定するクラス
// D3D_FEATURE_LEVEL_11_0 以上をCompute Shader利用可とする
class ComputeShaderDetector {
public:
    // D3D11CreateDeviceの結果からCompute Shaderの利用可否を判定
    // featureLevel_outには実際に有効になったFeature Levelが返る
    static bool IsComputeShaderAvailable(
        HRESULT createDeviceResult,
        D3D_FEATURE_LEVEL& featureLevel_out)
    {
        if (FAILED(createDeviceResult)) {
            featureLevel_out = D3D_FEATURE_LEVEL_9_1;
            return false;
        }

        featureLevel_out = createDeviceResult;
        return featureLevel_out >= D3D_FEATURE_LEVEL_11_0;
    }

    // 既存のID3D11DeviceからFeature Levelを取得して判定
    static bool IsComputeShaderAvailable(ID3D11Device* device)
    {
        if (!device) {
            return false;
        }

        D3D_FEATURE_LEVEL featureLevel = device->GetFeatureLevel();
        return featureLevel >= D3D_FEATURE_LEVEL_11_0;
    }

    // Feature Levelのみで判定
    static bool IsComputeShaderAvailable(D3D_FEATURE_LEVEL featureLevel)
    {
        return featureLevel >= D3D_FEATURE_LEVEL_11_0;
    }
};

} // namespace WoLNamesBlackedOut::Core
