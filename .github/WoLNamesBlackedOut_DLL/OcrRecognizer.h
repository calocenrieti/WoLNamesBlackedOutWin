#pragma once

#include "CoreTypes.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <winml/onnxruntime_cxx_api.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace WoLNamesBlackedOut::Core {

struct OcrTrackRoi {
	uint64_t track_id = 0;
	Detection detection{};
};

struct OcrTrackResult {
	uint64_t track_id = 0;
	std::string text;
	float confidence = 0.0f;
};

class OcrRecognizer {
public:
	OcrRecognizer();
	~OcrRecognizer();

	OcrRecognizer(const OcrRecognizer&) = delete;
	OcrRecognizer& operator=(const OcrRecognizer&) = delete;

	void SetD3D11Device(ID3D11Device* device, ID3D11DeviceContext* context);
	void SetGpuVendor(GpuVendor vendor);

	bool LoadDictionary(const wchar_t* dictionary_path);
	bool LoadModel(const wchar_t* model_path, bool prefer_gpu);

	bool IsReady() const;

	std::vector<OcrTrackResult> Recognize(
		ID3D11Texture2D* source_texture,
		uint32_t frame_width,
		uint32_t frame_height,
		const std::vector<OcrTrackRoi>& track_rois,
		int expand_pixels,
		int max_rois_per_frame) const;

private:
	static constexpr int kTargetHeight = 48;
	static constexpr int kMaxWidth = 320;

	Microsoft::WRL::ComPtr<ID3D11Device> device_;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
	GpuVendor gpu_vendor_ = GpuVendor::Unknown;

	std::shared_ptr<Ort::Env> env_;
	std::shared_ptr<Ort::Session> session_;
	std::vector<std::string> input_names_;
	std::vector<std::string> output_names_;
	bool dictionary_has_space_token_ = false;

	std::vector<std::string> dictionary_;

	bool CopyTextureToBgra(
		ID3D11Texture2D* source_texture,
		uint32_t frame_width,
		uint32_t frame_height,
		std::vector<uint8_t>& out_bgra) const;

	std::vector<float> BuildInputTensor(
		const std::vector<uint8_t>& bgra,
		uint32_t frame_width,
		uint32_t frame_height,
		const std::vector<OcrTrackRoi>& track_rois,
		int expand_pixels,
		int max_rois_per_frame,
		std::vector<uint64_t>& out_track_ids) const;

	OcrTrackResult DecodeSingle(
		uint64_t track_id,
		const float* logits,
		int64_t time_steps,
		int64_t vocab_size) const;
};

} // namespace WoLNamesBlackedOut::Core
