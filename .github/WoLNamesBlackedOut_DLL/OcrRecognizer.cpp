#include "pch.h"
#include "OcrRecognizer.h"
#include "ModelCompilationCache.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>

// OpenCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>

#include <winml/dml_provider_factory.h>
#include <WinMLEpCatalog.h>

void ReportStatus(const char* fmt, ...);

namespace WoLNamesBlackedOut::Core {

namespace {

struct OcrSessionCacheEntry {
	std::shared_ptr<Ort::Session> session;
	std::vector<std::string> input_names;
	std::vector<std::string> output_names;
	std::string ep_display_name;
};

std::string NormalizeEpDisplayName(const std::string& ep_name)
{
	if (ep_name.empty()) {
		return "CPU";
	}

	if (ep_name == "NvTensorRTRTXExecutionProvider") {
		return "TensorRT-RTX";
	}

	if (ep_name == "DmlExecutionProvider") {
		return "DirectML";
	}

	if (ep_name == "DmlExecutionProvider(NPU)") {
		return "DirectML (NPU)";
	}

	if (ep_name == "MIGraphXExecutionProvider") {
		return "MIGraphX";
	}

	if (ep_name == "OpenVINOExecutionProvider") {
		return "OpenVINO";
	}

	if (ep_name == "QNNExecutionProvider") {
		return "QNN";
	}

	if (ep_name == "VitisAIExecutionProvider") {
		return "VitisAI";
	}

	return ep_name;
}

const char* OcrReadyStateToString(WinMLEpReadyState state)
{
	switch (state) {
	case WinMLEpReadyState_Ready:
		return "Ready";
	case WinMLEpReadyState_NotReady:
		return "NotReady";
	case WinMLEpReadyState_NotPresent:
		return "NotPresent";
	default:
		return "Unknown";
	}
}

BOOL CALLBACK OcrEpCatalogEnumCallback(
	_In_ WinMLEpHandle ep,
	_In_ const WinMLEpInfo* info,
	_In_opt_ void* context)
{
	(void)ep;
	(void)context;

	const char* name = (info && info->name) ? info->name : "(null)";
	const char* version = (info && info->version) ? info->version : "(null)";
	const char* library_path = (info && info->libraryPath) ? info->libraryPath : "(null)";
	const char* ready_state = info ? OcrReadyStateToString(info->readyState) : "Unknown";

	char dbg[1024] = {};
	snprintf(dbg, sizeof(dbg),
		"[OcrRecognizer][EP Catalog] name=%s version=%s ready=%s lib=%s\n",
		name,
		version,
		ready_state,
		library_path);
	OutputDebugStringA(dbg);

	return TRUE;
}

void LogOcrEpCatalog()
{
	OutputDebugStringA("[OcrRecognizer] Enumerating OCR WinML EP catalog...\n");
	WinMLEpCatalogHandle catalog = nullptr;
	HRESULT hr = WinMLEpCatalogCreate(&catalog);
	if (FAILED(hr) || !catalog) {
		char dbg[256] = {};
		snprintf(dbg, sizeof(dbg), "[OcrRecognizer] WinMLEpCatalogCreate failed: 0x%08X\n", static_cast<unsigned>(hr));
		OutputDebugStringA(dbg);
		return;
	}

	hr = WinMLEpCatalogEnumProviders(catalog, OcrEpCatalogEnumCallback, nullptr);
	if (FAILED(hr)) {
		char dbg[256] = {};
		snprintf(dbg, sizeof(dbg), "[OcrRecognizer] WinMLEpCatalogEnumProviders failed: 0x%08X\n", static_cast<unsigned>(hr));
		OutputDebugStringA(dbg);
	}

	WinMLEpCatalogRelease(catalog);
}

void LogOcrAvailableProviders(const std::vector<std::string>& providers)
{
	std::string provider_list;
	for (const auto& provider : providers) {
		if (!provider_list.empty()) {
			provider_list += ", ";
		}
		provider_list += provider;
	}

	char dbg[768] = {};
	snprintf(dbg, sizeof(dbg), "[OcrRecognizer] Available providers: %s\n", provider_list.empty() ? "(none)" : provider_list.c_str());
	OutputDebugStringA(dbg);
}

bool IsDiscreteD3D11Device(ID3D11Device* device)
{
	if (!device) {
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) || !dxgi_device) {
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
	if (FAILED(dxgi_device->GetAdapter(&adapter)) || !adapter) {
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter1;
	if (FAILED(adapter.As(&adapter1)) || !adapter1) {
		return false;
	}

	DXGI_ADAPTER_DESC1 desc{};
	if (FAILED(adapter1->GetDesc1(&desc))) {
		return false;
	}

	if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
		return false;
	}

	return desc.DedicatedVideoMemory > 0;
}

bool IsInvalidGraphException(const Ort::Exception& ex)
{
	if (ex.GetOrtErrorCode() == ORT_INVALID_GRAPH) {
		return true;
	}

	std::string message = ex.what();
	std::transform(message.begin(), message.end(), message.begin(), [](unsigned char c) {
		return static_cast<char>(std::toupper(c));
	});

	return message.find("INVALID_GRAPH") != std::string::npos;
}

bool IsSamePathInsensitive(const std::wstring& lhs, const std::wstring& rhs)
{
	if (lhs.size() != rhs.size()) {
		return false;
	}

	for (size_t i = 0; i < lhs.size(); ++i) {
		if (towlower(lhs[i]) != towlower(rhs[i])) {
			return false;
		}
	}

	return true;
}

std::mutex& GetOcrSessionCacheMutex()
{
	static auto* cache_mutex = new std::mutex();
	return *cache_mutex;
}

std::unordered_map<std::string, OcrSessionCacheEntry>& GetOcrSessionCache()
{
	static auto* cache = new std::unordered_map<std::string, OcrSessionCacheEntry>();
	return *cache;
}

std::mutex& GetOcrSessionBuildMutex()
{
	static auto* build_mutex = new std::mutex();
	return *build_mutex;
}

std::shared_ptr<Ort::Env> GetSharedOcrOrtEnv()
{
	static std::shared_ptr<Ort::Env> shared_env = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "WoLNamesBlackedOut.OCR");
	return shared_env;
}

std::string WideToUtf8(const wchar_t* value)
{
	if (!value || value[0] == L'\0') {
		return {};
	}

	int len = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1) {
		return {};
	}

	std::string utf8(static_cast<size_t>(len) - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8.data(), len, nullptr, nullptr);
	return utf8;
}

std::string BuildOcrSessionCacheKey(const wchar_t* model_path, WoLNamesBlackedOut::Core::GpuVendor vendor, bool use_gpu)
{
	std::string key = WideToUtf8(model_path);
	key += "|gpu=";
	key += std::to_string(static_cast<int>(vendor));
	key += "|use_gpu=";
	key += use_gpu ? "1" : "0";
	char force_cpu_buf[32] = {};
	DWORD got_force_cpu = GetEnvironmentVariableA("WOL_FORCE_CPU_PIPELINE", force_cpu_buf, static_cast<DWORD>(sizeof(force_cpu_buf)));
	bool force_cpu = false;
	if (got_force_cpu > 0 && got_force_cpu < sizeof(force_cpu_buf)) {
		force_cpu = (force_cpu_buf[0] == '1') || (_stricmp(force_cpu_buf, "true") == 0);
	}
	key += "|forcecpu=";
	key += force_cpu ? "1" : "0";
	return key;
}

inline int ClampInt(int v, int lo, int hi) {
	return std::max(lo, std::min(v, hi));
}

inline std::wstring TrimUtf16(const std::wstring& s) {
	size_t begin = 0;
	while (begin < s.size() && iswspace(s[begin])) {
		++begin;
	}

	size_t end = s.size();
	while (end > begin && iswspace(s[end - 1])) {
		--end;
	}

	return s.substr(begin, end - begin);
}

std::string ApplySpaceCandidateFallback(const std::string& text)
{
	if (text.empty()) {
		return text;
	}

	auto is_lower = [](unsigned char c) {
		return std::islower(c) != 0;
	};

	std::string out;
	out.reserve(text.size());

	for (size_t i = 0; i < text.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if (c == '_' && i > 0 && (i + 1) < text.size()) {
			const unsigned char prev = static_cast<unsigned char>(text[i - 1]);
			const unsigned char next = static_cast<unsigned char>(text[i + 1]);
			if (is_lower(prev) && is_lower(next)) {
				if (out.empty() || out.back() != ' ') {
					out.push_back(' ');
				}
				continue;
			}
		}
		out.push_back(static_cast<char>(c));
	}

	std::string normalized;
	normalized.reserve(out.size());
	bool prev_space = false;
	for (unsigned char c : out) {
		if (std::isspace(c)) {
			if (!prev_space) {
				normalized.push_back(' ');
			}
			prev_space = true;
		} else {
			normalized.push_back(static_cast<char>(c));
			prev_space = false;
		}
	}

	while (!normalized.empty() && normalized.front() == ' ') {
		normalized.erase(normalized.begin());
	}
	while (!normalized.empty() && normalized.back() == ' ') {
		normalized.pop_back();
	}

	return normalized;
}

bool EnsureCatalogEpReady(Ort::Env* env, const char* provider_name)
{
	if (!env || !provider_name || provider_name[0] == '\0') {
		return false;
	}

	std::string display_name = NormalizeEpDisplayName(provider_name);
	char dbg[768] = {};
	snprintf(dbg, sizeof(dbg), "[OcrRecognizer] EnsureCatalogEpReady: provider=%s\n", provider_name);
	OutputDebugStringA(dbg);

	WinMLEpCatalogHandle catalog = nullptr;
	HRESULT hr = WinMLEpCatalogCreate(&catalog);
	if (FAILED(hr) || !catalog) {
		snprintf(dbg, sizeof(dbg), "[OcrRecognizer] WinMLEpCatalogCreate failed: 0x%08X\n", static_cast<unsigned>(hr));
		OutputDebugStringA(dbg);
		ReportStatus("OCR EP catalog initialization failed");
		return false;
	}

	WinMLEpHandle ep = nullptr;
	hr = WinMLEpCatalogFindProvider(catalog, provider_name, nullptr, &ep);
	if (FAILED(hr) || !ep) {
		snprintf(dbg, sizeof(dbg), "[OcrRecognizer] WinMLEpCatalogFindProvider failed: provider=%s hr=0x%08X\n", provider_name, static_cast<unsigned>(hr));
		OutputDebugStringA(dbg);
		ReportStatus("OCR %s provider unavailable", display_name.c_str());
		WinMLEpCatalogRelease(catalog);
		return false;
	}

	WinMLEpReadyState state = WinMLEpReadyState_NotPresent;
	hr = WinMLEpGetReadyState(ep, &state);
	if (FAILED(hr)) {
		snprintf(dbg, sizeof(dbg), "[OcrRecognizer] WinMLEpGetReadyState failed: provider=%s hr=0x%08X\n", provider_name, static_cast<unsigned>(hr));
		OutputDebugStringA(dbg);
		ReportStatus("OCR %s provider state check failed", display_name.c_str());
		WinMLEpCatalogRelease(catalog);
		return false;
	}
	snprintf(dbg, sizeof(dbg), "[OcrRecognizer] EP state: provider=%s state=%s\n", provider_name, OcrReadyStateToString(state));
	OutputDebugStringA(dbg);

	if (state == WinMLEpReadyState_NotPresent || state == WinMLEpReadyState_NotReady) {
		ReportStatus("Downloading OCR %s execution provider...", display_name.c_str());
		snprintf(dbg, sizeof(dbg), "[OcrRecognizer] WinMLEpEnsureReady start: provider=%s\n", provider_name);
		OutputDebugStringA(dbg);
		hr = WinMLEpEnsureReady(ep);
		if (FAILED(hr)) {
			snprintf(dbg, sizeof(dbg), "[OcrRecognizer] WinMLEpEnsureReady failed: provider=%s hr=0x%08X\n", provider_name, static_cast<unsigned>(hr));
			OutputDebugStringA(dbg);
			ReportStatus("OCR %s execution provider download failed", display_name.c_str());
			WinMLEpCatalogRelease(catalog);
			return false;
		}
		hr = WinMLEpGetReadyState(ep, &state);
		snprintf(dbg, sizeof(dbg), "[OcrRecognizer] EP state after ensure: provider=%s state=%s hr=0x%08X\n", provider_name, OcrReadyStateToString(state), static_cast<unsigned>(hr));
		OutputDebugStringA(dbg);
		if (FAILED(hr) || state != WinMLEpReadyState_Ready) {
			ReportStatus("OCR %s execution provider preparation failed", display_name.c_str());
			WinMLEpCatalogRelease(catalog);
			return false;
		}
		ReportStatus("OCR %s execution provider ready", display_name.c_str());
	}

	bool registered = false;
	size_t path_size = 0;
	hr = WinMLEpGetLibraryPathSize(ep, &path_size);
	snprintf(dbg, sizeof(dbg), "[OcrRecognizer] WinMLEpGetLibraryPathSize: provider=%s hr=0x%08X size=%zu\n", provider_name, static_cast<unsigned>(hr), path_size);
	OutputDebugStringA(dbg);
	if (SUCCEEDED(hr) && path_size > 0) {
		std::string library_path_utf8(path_size, '\0');
		hr = WinMLEpGetLibraryPath(ep, path_size, library_path_utf8.data(), nullptr);
		if (SUCCEEDED(hr)) {
			library_path_utf8.resize(strlen(library_path_utf8.c_str()));
			snprintf(dbg, sizeof(dbg), "[OcrRecognizer] EP library path: provider=%s path=%s\n", provider_name, library_path_utf8.c_str());
			OutputDebugStringA(dbg);
			int wlen = MultiByteToWideChar(CP_UTF8, 0, library_path_utf8.c_str(), -1, nullptr, 0);
			if (wlen > 0) {
				std::wstring library_path_w(static_cast<size_t>(wlen) - 1, L'\0');
				MultiByteToWideChar(CP_UTF8, 0, library_path_utf8.c_str(), -1, library_path_w.data(), wlen);
				try {
					env->RegisterExecutionProviderLibrary(provider_name, library_path_w);
					snprintf(dbg, sizeof(dbg), "[OcrRecognizer] RegisterExecutionProviderLibrary success: provider=%s\n", provider_name);
					OutputDebugStringA(dbg);
					registered = true;
				} catch (const Ort::Exception& ex) {
					snprintf(dbg, sizeof(dbg), "[OcrRecognizer] RegisterExecutionProviderLibrary failed: provider=%s, error=%s\n", provider_name, ex.what());
					OutputDebugStringA(dbg);
					ReportStatus("OCR %s provider registration failed", display_name.c_str());
					registered = false;
				} catch (const std::exception& ex) {
					snprintf(dbg, sizeof(dbg), "[OcrRecognizer] RegisterExecutionProviderLibrary failed: provider=%s, std::error=%s\n", provider_name, ex.what());
					OutputDebugStringA(dbg);
					ReportStatus("OCR %s provider registration failed", display_name.c_str());
					registered = false;
				} catch (...) {
					snprintf(dbg, sizeof(dbg), "[OcrRecognizer] RegisterExecutionProviderLibrary failed: provider=%s, unknown exception\n", provider_name);
					OutputDebugStringA(dbg);
					ReportStatus("OCR %s provider registration failed", display_name.c_str());
					registered = false;
				}
			}
		}
	}

	if (!registered) {
		try {
			auto ep_devices = env->GetEpDevices();
			bool found_target = false;
			for (const auto& device : ep_devices) {
				if (std::string(device.EpName()) == provider_name) {
					found_target = true;
					break;
				}
			}

			if (found_target) {
				snprintf(dbg, sizeof(dbg), "[OcrRecognizer] Provider visible in GetEpDevices despite registration failure: provider=%s. Continue.\n", provider_name);
				OutputDebugStringA(dbg);
				ReportStatus("OCR %s provider is visible and will be used", display_name.c_str());
				registered = true;
			} else {
				snprintf(dbg, sizeof(dbg), "[OcrRecognizer] Provider not visible in GetEpDevices: provider=%s\n", provider_name);
				OutputDebugStringA(dbg);
			}
		} catch (const Ort::Exception& ex) {
			snprintf(dbg, sizeof(dbg), "[OcrRecognizer] GetEpDevices check failed: provider=%s, error=%s\n", provider_name, ex.what());
			OutputDebugStringA(dbg);
		} catch (...) {
			snprintf(dbg, sizeof(dbg), "[OcrRecognizer] GetEpDevices check failed: provider=%s, unknown exception\n", provider_name);
			OutputDebugStringA(dbg);
		}
	}

	WinMLEpCatalogRelease(catalog);
	if (!registered) {
		ReportStatus("OCR %s provider registration failed", display_name.c_str());
	}
	return registered;
}

} // namespace

OcrRecognizer::OcrRecognizer()
{
	env_ = GetSharedOcrOrtEnv();
}

OcrRecognizer::~OcrRecognizer() = default;

void OcrRecognizer::SetD3D11Device(ID3D11Device* device, ID3D11DeviceContext* context)
{
	device_ = device;
	context_ = context;
}

void OcrRecognizer::SetGpuVendor(GpuVendor vendor)
{
	gpu_vendor_ = vendor;
}

bool OcrRecognizer::LoadDictionary(const wchar_t* dictionary_path)
{
	if (!dictionary_path || dictionary_path[0] == L'\0') {
		return false;
	}

	dictionary_.clear();
	dictionary_.push_back("blank");
	dictionary_has_space_token_ = false;

	std::wifstream ifs(dictionary_path);
	if (!ifs.is_open()) {
		return false;
	}

	std::wstring line;
	while (std::getline(ifs, line)) {
		if (!line.empty() && line[0] == 0xFEFF) {
			line.erase(0, 1);
		}

		if (!line.empty() && line.back() == L'\r') {
			line.pop_back();
		}

		if (line.empty()) {
			continue;
		}

		int utf8_len = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (utf8_len <= 0) {
			continue;
		}

		std::string token(static_cast<size_t>(utf8_len) - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, token.data(), utf8_len, nullptr, nullptr);
		if (token == " ") {
			dictionary_has_space_token_ = true;
		}
		dictionary_.push_back(token);
	}

	if (dictionary_.size() <= 1) {
		return false;
	}

	char dbg[192] = {};
	snprintf(dbg, sizeof(dbg), "[OcrRecognizer] Dictionary loaded: size=%zu, has_space_token=%d\n", dictionary_.size(), dictionary_has_space_token_ ? 1 : 0);
	OutputDebugStringA(dbg);

	return true;
}

bool OcrRecognizer::LoadModel(const wchar_t* model_path, bool prefer_gpu)
{
	if (!model_path || model_path[0] == L'\0') {
		return false;
	}

	if (!env_) {
		env_ = GetSharedOcrOrtEnv();
	}

	bool force_cpu_pipeline = false;
	{
		char force_cpu_buf[32] = {};
		DWORD got_force_cpu = GetEnvironmentVariableA("WOL_FORCE_CPU_PIPELINE", force_cpu_buf, static_cast<DWORD>(sizeof(force_cpu_buf)));
		if (got_force_cpu > 0 && got_force_cpu < sizeof(force_cpu_buf)) {
			force_cpu_pipeline = (force_cpu_buf[0] == '1') || (_stricmp(force_cpu_buf, "true") == 0);
		}
		char dbg_force[128] = {};
		snprintf(dbg_force, sizeof(dbg_force), "[OcrRecognizer] force_cpu_pipeline=%d prefer_gpu=%d\n", force_cpu_pipeline ? 1 : 0, prefer_gpu ? 1 : 0);
		OutputDebugStringA(dbg_force);
	}

	bool use_gpu = force_cpu_pipeline ? false : prefer_gpu;
	{
		char env_buf[8] = {};
		DWORD got = GetEnvironmentVariableA("WOL_DISABLE_OCR_DML", env_buf, static_cast<DWORD>(sizeof(env_buf)));
		if (got > 0 && (env_buf[0] == '1' || env_buf[0] == 'T' || env_buf[0] == 't' || env_buf[0] == 'Y' || env_buf[0] == 'y')) {
			use_gpu = false;
		}
	}

	const std::string cache_key = BuildOcrSessionCacheKey(model_path, gpu_vendor_, use_gpu);
	std::lock_guard<std::mutex> build_lock(GetOcrSessionBuildMutex());
	{
		auto& session_cache = GetOcrSessionCache();
		std::lock_guard<std::mutex> lock(GetOcrSessionCacheMutex());
		auto it = session_cache.find(cache_key);
		if (it != session_cache.end()) {
			session_ = it->second.session;
			input_names_ = it->second.input_names;
			output_names_ = it->second.output_names;
			std::string ep_display_name = NormalizeEpDisplayName(it->second.ep_display_name);
			OutputDebugStringA("[OcrRecognizer] Reusing cached OCR ONNX session\n");
			ReportStatus("Reusing OCR EP: %s", ep_display_name.c_str());
			return !input_names_.empty() && !output_names_.empty();
		}
	}

	try {
		Ort::SessionOptions session_options;
		session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
		session_options.SetIntraOpNumThreads(1);

		std::string selected_ep;
		bool ep_appended = false;

		std::vector<std::string> available_providers;
		try {
			available_providers = Ort::GetAvailableProviders();
		} catch (...) {
		}
		LogOcrAvailableProviders(available_providers);
		LogOcrEpCatalog();

		auto has_ep = [&available_providers](const std::string& name) {
			return std::find(available_providers.begin(), available_providers.end(), name) != available_providers.end();
		};

		auto try_append_catalog_ep = [this, &session_options](const char* ep_name,
			const std::unordered_map<std::string, std::string>& ep_options = {}) -> bool {
			if (!EnsureCatalogEpReady(env_.get(), ep_name)) {
				return false;
			}

			try {
				auto ep_devices = env_->GetEpDevices();
				std::vector<Ort::ConstEpDevice> target_devices;
				for (const auto& device : ep_devices) {
					if (std::string(device.EpName()) == ep_name) {
						target_devices.push_back(device);
					}
				}
				if (!target_devices.empty()) {
					session_options.AppendExecutionProvider_V2(*env_, target_devices, ep_options);
					return true;
				}
			} catch (...) {
			}

			return false;
		};

		auto try_append_dml = [&has_ep, &session_options, &selected_ep, &ep_appended](OrtDmlDeviceFilter filter, const char* ep_name) -> bool {
			if (!has_ep("DmlExecutionProvider")) {
				return false;
			}

			try {
				const OrtDmlApi* dml_api = nullptr;
				Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dml_api)));
				if (dml_api != nullptr) {
					OrtDmlDeviceOptions device_options;
					device_options.Preference = OrtDmlPerformancePreference::HighPerformance;
					device_options.Filter = filter;
					Ort::ThrowOnError(dml_api->SessionOptionsAppendExecutionProvider_DML2(session_options, &device_options));
					selected_ep = ep_name;
					ep_appended = true;
					return true;
				}
			} catch (...) {
			}

			return false;
		};

		auto try_append_npu_catalog_ep = [&]() -> bool {
			if (try_append_catalog_ep("QNNExecutionProvider")) {
				selected_ep = "QNNExecutionProvider";
				ep_appended = true;
				return true;
			}

			if (try_append_catalog_ep("VitisAIExecutionProvider")) {
				selected_ep = "VitisAIExecutionProvider";
				ep_appended = true;
				return true;
			}

			if (try_append_catalog_ep("OpenVINOExecutionProvider")) {
				selected_ep = "OpenVINOExecutionProvider";
				ep_appended = true;
				return true;
			}

			return false;
		};

		if (use_gpu) {
			const bool has_discrete_gpu = IsDiscreteD3D11Device(device_.Get());
			char dbg_gpu[256] = {};
			snprintf(dbg_gpu, sizeof(dbg_gpu), "[OcrRecognizer] use_gpu=1, has_discrete_gpu=%d, vendor=%d\n", has_discrete_gpu ? 1 : 0, static_cast<int>(gpu_vendor_));
			OutputDebugStringA(dbg_gpu);
			ReportStatus("Trying OCR vendor execution providers...");

			switch (gpu_vendor_) {
			case GpuVendor::NVIDIA: {
				std::unordered_map<std::string, std::string> trt_rtx_options = {
					{"nv_max_workspace_size", "4294967296"},
				};

				try {
					auto local_folder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
					std::wstring cache_path_w = local_folder.Path().c_str();
					cache_path_w += L"\\trt_rtx_cache";
					CreateDirectoryW(cache_path_w.c_str(), nullptr);
					std::string cache_path_utf8 = ModelCompilationCache::WideToUtf8(cache_path_w);
					if (!cache_path_utf8.empty()) {
						trt_rtx_options["nv_runtime_cache_path"] = cache_path_utf8;
					}
				} catch (...) {
				}

				if (try_append_catalog_ep("NvTensorRTRTXExecutionProvider", trt_rtx_options)) {
					selected_ep = "NvTensorRTRTXExecutionProvider";
					ep_appended = true;
					OutputDebugStringA("[OcrRecognizer] Vendor EP selected: NvTensorRTRTXExecutionProvider\n");
				}
				break;
			}
			case GpuVendor::AMD:
				if (try_append_catalog_ep("MIGraphXExecutionProvider")) {
					selected_ep = "MIGraphXExecutionProvider";
					ep_appended = true;
					OutputDebugStringA("[OcrRecognizer] Vendor EP selected: MIGraphXExecutionProvider\n");
				}
				break;
			case GpuVendor::Intel:
				if (try_append_catalog_ep("OpenVINOExecutionProvider")) {
					selected_ep = "OpenVINOExecutionProvider";
					ep_appended = true;
					OutputDebugStringA("[OcrRecognizer] Vendor EP selected: OpenVINOExecutionProvider\n");
				}
				break;
			default:
				OutputDebugStringA("[OcrRecognizer] Unknown GPU vendor. Vendor EP direct selection skipped.\n");
				break;
			}

			if (!ep_appended) {
				OutputDebugStringA("[OcrRecognizer] Vendor EP unavailable. Trying DirectML GPU fallback.\n");
				try_append_dml(OrtDmlDeviceFilter::Gpu, "DmlExecutionProvider");
			}

			if (!ep_appended) {
				ReportStatus("OCR dGPU EP unavailable. Falling back to NPU execution providers...");
				if (!try_append_npu_catalog_ep()) {
					#ifdef ENABLE_NPU_ADAPTER_ENUMERATION
					try_append_dml(OrtDmlDeviceFilter::Npu, "DmlExecutionProvider(NPU)");
					#endif
				}
			}

			if (!ep_appended) {
				ReportStatus("OCR NPU EP unavailable. Falling back to CPU execution provider...");
			}
		} else {
			if (force_cpu_pipeline) {
				ReportStatus("OCR force CPU pipeline enabled: trying NPU execution providers...");
				if (!try_append_npu_catalog_ep()) {
					#ifdef ENABLE_NPU_ADAPTER_ENUMERATION
					try_append_dml(OrtDmlDeviceFilter::Npu, "DmlExecutionProvider(NPU)");
					#endif
				}
				if (!ep_appended) {
					ReportStatus("OCR force CPU pipeline: NPU EP unavailable. Falling back to CPU execution provider...");
				}
			} else {
				ReportStatus("OCR GPU EP disabled. Falling back to CPU execution provider...");
			}
		}

		std::string ep_display_name = NormalizeEpDisplayName(selected_ep);
		if (ep_appended) {
			char dbg[256] = {};
			snprintf(dbg, sizeof(dbg), "[OcrRecognizer] GPU EP enabled: %s\n", selected_ep.c_str());
			OutputDebugStringA(dbg);
		} else {
			OutputDebugStringA("[OcrRecognizer] CPU EP (GPU EP not enabled)\n");
		}

		std::wstring session_model_path = model_path;
		ModelCompilationCache::CacheDecision compile_cache_decision;
		bool compile_cache_eligible = false;
		if (ep_appended && ModelCompilationCache::SupportsPersistentCompileCache(selected_ep)) {
			std::string cache_error;
			if (ModelCompilationCache::PrepareCacheDecision(model_path, selected_ep, compile_cache_decision, cache_error)) {
				compile_cache_eligible = true;
				if (compile_cache_decision.has_valid_compiled_model) {
					session_model_path = compile_cache_decision.compiled_model_path;
					OutputDebugStringA("[OcrRecognizer] Persistent compile cache hit. Using compiled model.\n");
					ReportStatus("OCR compiled cache hit: %s", ep_display_name.c_str());
				} else {
					char dbg_cache_miss[512] = {};
					snprintf(dbg_cache_miss, sizeof(dbg_cache_miss), "[OcrRecognizer] Persistent compile cache miss (%s).\n", compile_cache_decision.reason.c_str());
					OutputDebugStringA(dbg_cache_miss);
					ReportStatus("Compiling OCR model for EP: %s", ep_display_name.c_str());
					if (ModelCompilationCache::CompileModelToCache(*env_, session_options, compile_cache_decision, cache_error)) {
						session_model_path = compile_cache_decision.compiled_model_path;
						OutputDebugStringA("[OcrRecognizer] CompileModel succeeded. Using compiled model.\n");
					} else {
						char dbg_cache_compile_failed[512] = {};
						snprintf(dbg_cache_compile_failed, sizeof(dbg_cache_compile_failed), "[OcrRecognizer] CompileModel failed (%s). Falling back to source model.\n", cache_error.c_str());
						OutputDebugStringA(dbg_cache_compile_failed);
						ReportStatus("OCR compile fallback to source model: %s", ep_display_name.c_str());
						session_model_path = model_path;
					}
				}
			} else {
				char dbg_cache_prepare_failed[512] = {};
				snprintf(dbg_cache_prepare_failed, sizeof(dbg_cache_prepare_failed), "[OcrRecognizer] Prepare compile cache failed (%s).\n", cache_error.c_str());
				OutputDebugStringA(dbg_cache_prepare_failed);
			}
		}

		ReportStatus("Initializing OCR EP: %s", ep_display_name.c_str());
		auto create_session = [&](const std::wstring& path) {
			return std::make_shared<Ort::Session>(*env_, path.c_str(), session_options);
		};

		try {
			session_ = create_session(session_model_path);
		}
		catch (const Ort::Exception& ex) {
			const bool attempted_compiled = compile_cache_eligible &&
				!compile_cache_decision.compiled_model_path.empty() &&
				IsSamePathInsensitive(session_model_path, compile_cache_decision.compiled_model_path);

			if (!attempted_compiled) {
				throw;
			}

			if (IsInvalidGraphException(ex)) {
				OutputDebugStringA("[OcrRecognizer] Compiled model became invalid (INVALID_GRAPH). Recompiling cache.\n");
				ReportStatus("OCR compiled model invalidated. Recompiling: %s", ep_display_name.c_str());
				std::string remove_error;
				ModelCompilationCache::RemoveCacheArtifacts(compile_cache_decision, remove_error);
				std::string compile_error;
				if (ModelCompilationCache::CompileModelToCache(*env_, session_options, compile_cache_decision, compile_error)) {
					session_ = create_session(compile_cache_decision.compiled_model_path);
					ReportStatus("OCR compiled cache regenerated: %s", ep_display_name.c_str());
				} else {
					char dbg_retry_fail[512] = {};
					snprintf(dbg_retry_fail, sizeof(dbg_retry_fail), "[OcrRecognizer] Recompile after INVALID_GRAPH failed (%s). Fallback to source model.\n", compile_error.c_str());
					OutputDebugStringA(dbg_retry_fail);
					ReportStatus("OCR recompile failed. Fallback to source model: %s", ep_display_name.c_str());
					session_ = create_session(model_path);
				}
			} else {
				char dbg_compiled_load_fail[512] = {};
				snprintf(dbg_compiled_load_fail, sizeof(dbg_compiled_load_fail), "[OcrRecognizer] Failed to load compiled model (%s). Falling back to source model.\n", ex.what());
				OutputDebugStringA(dbg_compiled_load_fail);
				ReportStatus("OCR compiled model load failed. Fallback to source model: %s", ep_display_name.c_str());
				session_ = create_session(model_path);
			}
		}

		ReportStatus("OCR EP ready: %s", ep_display_name.c_str());

		input_names_.clear();
		output_names_.clear();
		Ort::AllocatorWithDefaultOptions allocator;

		const size_t input_count = session_->GetInputCount();
		for (size_t i = 0; i < input_count; ++i) {
			auto name_alloc = session_->GetInputNameAllocated(i, allocator);
			input_names_.push_back(name_alloc.get());
		}

		const size_t output_count = session_->GetOutputCount();
		for (size_t i = 0; i < output_count; ++i) {
			auto name_alloc = session_->GetOutputNameAllocated(i, allocator);
			output_names_.push_back(name_alloc.get());
		}

		if (!input_names_.empty() && !output_names_.empty()) {
			auto& session_cache = GetOcrSessionCache();
			std::lock_guard<std::mutex> lock(GetOcrSessionCacheMutex());
			session_cache[cache_key] = OcrSessionCacheEntry{ session_, input_names_, output_names_, ep_display_name };
		}

		return !input_names_.empty() && !output_names_.empty();
	} catch (...) {
		ReportStatus("OCR EP initialization failed");
		session_.reset();
		input_names_.clear();
		output_names_.clear();
		return false;
	}
}

bool OcrRecognizer::IsReady() const
{
	return session_ != nullptr && !dictionary_.empty() && !input_names_.empty() && !output_names_.empty();
}

std::vector<OcrTrackResult> OcrRecognizer::Recognize(
	ID3D11Texture2D* source_texture,
	uint32_t frame_width,
	uint32_t frame_height,
	const std::vector<OcrTrackRoi>& track_rois,
	int expand_pixels,
	int max_rois_per_frame) const
{
	std::vector<OcrTrackResult> results;
	auto total_start = std::chrono::high_resolution_clock::now();

	if (!IsReady() || !source_texture || frame_width == 0 || frame_height == 0 || track_rois.empty()) {
		return results;
	}

	std::vector<uint8_t> frame_bgra;
	auto readback_start = std::chrono::high_resolution_clock::now();
	if (!CopyTextureToBgra(source_texture, frame_width, frame_height, frame_bgra)) {
		return results;
	}
	auto readback_end = std::chrono::high_resolution_clock::now();
	auto readback_ms = std::chrono::duration<double, std::milli>(readback_end - readback_start).count();

	return RecognizeFromCpuBgra(frame_bgra.data(), frame_width, frame_height, track_rois, expand_pixels, max_rois_per_frame);
}

std::vector<OcrTrackResult> OcrRecognizer::RecognizeFromCpuBgra(
	const uint8_t* bgra_data,
	uint32_t frame_width,
	uint32_t frame_height,
	const std::vector<OcrTrackRoi>& track_rois,
	int expand_pixels,
	int max_rois_per_frame) const
{
	std::vector<OcrTrackResult> results;
	if (!bgra_data || frame_width == 0 || frame_height == 0 || track_rois.empty() || !session_) {
		return results;
	}

	auto total_start = std::chrono::high_resolution_clock::now();

	std::vector<uint8_t> frame_bgra;
	// bgra_data を直接使用するためコピーを回避
	const size_t bgra_size = static_cast<size_t>(frame_width) * static_cast<size_t>(frame_height) * 4ull;
	frame_bgra.assign(bgra_data, bgra_data + bgra_size);

	std::vector<uint64_t> track_ids;
	auto tensor_start = std::chrono::high_resolution_clock::now();
	std::vector<float> input = BuildInputTensor(
		frame_bgra,
		frame_width,
		frame_height,
		track_rois,
		expand_pixels,
		max_rois_per_frame,
		track_ids);
	auto tensor_end = std::chrono::high_resolution_clock::now();
	auto tensor_ms = std::chrono::duration<double, std::milli>(tensor_end - tensor_start).count();

	if (track_ids.empty() || input.empty()) {
		return results;
	}

	const int64_t batch = static_cast<int64_t>(ClampInt(max_rois_per_frame, 1, 32));
	std::vector<int64_t> input_shape = { batch, 3, kTargetHeight, kMaxWidth };
	auto pack_start = std::chrono::high_resolution_clock::now();

	Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
	Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
		memory_info,
		input.data(),
		input.size(),
		input_shape.data(),
		input_shape.size());

	std::vector<const char*> input_names;
	std::vector<const char*> output_names;
	input_names.reserve(this->input_names_.size());
	output_names.reserve(this->output_names_.size());

	for (const auto& n : this->input_names_) {
		input_names.push_back(n.c_str());
	}

	for (const auto& n : this->output_names_) {
		output_names.push_back(n.c_str());
	}
	auto pack_end = std::chrono::high_resolution_clock::now();
	auto pack_ms = std::chrono::duration<double, std::milli>(pack_end - pack_start).count();

	std::vector<Ort::Value> output_tensors;
	double run_ms = 0.0;
	const size_t active_rois = static_cast<size_t>(std::count_if(track_ids.begin(), track_ids.end(), [](uint64_t id) {
		return id != 0;
	}));
	try {
		auto run_start = std::chrono::high_resolution_clock::now();
		output_tensors = session_->Run(
			Ort::RunOptions{ nullptr },
			input_names.data(),
			&input_tensor,
			1,
			output_names.data(),
			output_names.size());
		auto run_end = std::chrono::high_resolution_clock::now();
		run_ms = std::chrono::duration<double, std::milli>(run_end - run_start).count();
		char dbg_run[256] = {};
		snprintf(dbg_run, sizeof(dbg_run),
			"[OcrRecognizer] ORT run: batch=%lld, active_rois=%zu, time=%.2f ms\n",
			static_cast<long long>(batch),
			active_rois,
			run_ms);
		OutputDebugStringA(dbg_run);
	} catch (...) {
		return results;
	}

	if (output_tensors.empty() || !output_tensors[0].IsTensor()) {
		return results;
	}

	auto shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();
	if (shape.size() < 3) {
		return results;
	}

	const float* logits = output_tensors[0].GetTensorData<float>();
	if (!logits) {
		return results;
	}

	int64_t batch_dim = shape[0] > 0 ? shape[0] : batch;
	int64_t t_dim = shape[1] > 0 ? shape[1] : 0;
	int64_t c_dim = shape[2] > 0 ? shape[2] : 0;

	if (batch_dim <= 0 || t_dim <= 0 || c_dim <= 0) {
		return results;
	}

	const int64_t per_batch = t_dim * c_dim;
	const int64_t decode_count = std::min<int64_t>(batch_dim, static_cast<int64_t>(track_ids.size()));
	results.reserve(static_cast<size_t>(decode_count));
	auto decode_start = std::chrono::high_resolution_clock::now();

	for (int64_t i = 0; i < decode_count; ++i) {
		if (track_ids[static_cast<size_t>(i)] == 0) {
			continue;
		}
		const float* ptr = logits + (i * per_batch);
		results.push_back(DecodeSingle(track_ids[static_cast<size_t>(i)], ptr, t_dim, c_dim));
	}
	auto decode_end = std::chrono::high_resolution_clock::now();
	auto decode_ms = std::chrono::duration<double, std::milli>(decode_end - decode_start).count();

	auto total_end = std::chrono::high_resolution_clock::now();
	auto total_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();
	char dbg[448] = {};
	snprintf(dbg, sizeof(dbg),
		"[OcrRecognizer] total=%.2f ms tensor=%.2f ms pack=%.2f ms run=%.2f ms decode=%.2f ms frame=%ux%u req=%zu out=%zu\n",
		total_ms,
		tensor_ms,
		pack_ms,
		run_ms,
		decode_ms,
		frame_width,
		frame_height,
		track_rois.size(),
		results.size());
	OutputDebugStringA(dbg);

	return results;
}

bool OcrRecognizer::CopyTextureToBgra(
	ID3D11Texture2D* source_texture,
	uint32_t frame_width,
	uint32_t frame_height,
	std::vector<uint8_t>& out_bgra) const
{
	if (!source_texture || !device_ || !context_) {
		return false;
	}
	auto copy_start = std::chrono::high_resolution_clock::now();

	D3D11_TEXTURE2D_DESC src_desc{};
	source_texture->GetDesc(&src_desc);

	D3D11_TEXTURE2D_DESC stage_desc = src_desc;
	stage_desc.Usage = D3D11_USAGE_STAGING;
	stage_desc.BindFlags = 0;
	stage_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stage_desc.MiscFlags = 0;

	Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
	if (FAILED(device_->CreateTexture2D(&stage_desc, nullptr, staging.ReleaseAndGetAddressOf())) || !staging) {
		return false;
	}

	context_->CopyResource(staging.Get(), source_texture);
	context_->Flush();

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
		return false;
	}

	out_bgra.resize(static_cast<size_t>(frame_width) * static_cast<size_t>(frame_height) * 4ull);
	const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
	const size_t dst_pitch = static_cast<size_t>(frame_width) * 4ull;
	for (uint32_t y = 0; y < frame_height; ++y) {
		memcpy(out_bgra.data() + (static_cast<size_t>(y) * dst_pitch), src + (static_cast<size_t>(y) * mapped.RowPitch), dst_pitch);
	}

	context_->Unmap(staging.Get(), 0);
	auto copy_end = std::chrono::high_resolution_clock::now();
	auto copy_ms = std::chrono::duration<double, std::milli>(copy_end - copy_start).count();
	char dbg[192] = {};
	snprintf(dbg, sizeof(dbg), "[OcrRecognizer] CopyTextureToBgra time=%.2f ms size=%ux%u\n", copy_ms, frame_width, frame_height);
	OutputDebugStringA(dbg);
	return true;
}

std::vector<float> OcrRecognizer::BuildInputTensor(
	const std::vector<uint8_t>& bgra,
	uint32_t frame_width,
	uint32_t frame_height,
	const std::vector<OcrTrackRoi>& track_rois,
	int expand_pixels,
	int max_rois_per_frame,
	std::vector<uint64_t>& out_track_ids) const
{
	const int max_rois = ClampInt(max_rois_per_frame, 1, 32);
	const int expand = ClampInt(expand_pixels, 0, 5);

	const size_t batch = static_cast<size_t>(max_rois);
	const size_t actual_count = std::min<size_t>(track_rois.size(), batch);
	const size_t sample_elements = 3ull * static_cast<size_t>(kTargetHeight) * static_cast<size_t>(kMaxWidth);
	std::vector<float> input(batch * sample_elements, 0.0f);

	out_track_ids.clear();
	out_track_ids.assign(batch, 0);

	if (bgra.empty() || frame_width == 0 || frame_height == 0) {
		return input;
	}

	cv::Mat frameMat(static_cast<int>(frame_height), static_cast<int>(frame_width), CV_8UC4, const_cast<uint8_t*>(bgra.data()));

	for (size_t i = 0; i < actual_count; ++i) {
		const auto& item = track_rois[i];
		const int x1 = ClampInt(static_cast<int>(std::floor(item.detection.x1)) - expand, 0, static_cast<int>(frame_width) - 1);
		const int y1 = ClampInt(static_cast<int>(std::floor(item.detection.y1)) - expand, 0, static_cast<int>(frame_height) - 1);
		const int x2 = ClampInt(static_cast<int>(std::ceil(item.detection.x2)) + expand, 1, static_cast<int>(frame_width));
		const int y2 = ClampInt(static_cast<int>(std::ceil(item.detection.y2)) + expand, 1, static_cast<int>(frame_height));

		const int roi_w = std::max(1, x2 - x1);
		const int roi_h = std::max(1, y2 - y1);

		cv::Rect roiRect(x1, y1, roi_w, roi_h);
		roiRect = roiRect & cv::Rect(0, 0, static_cast<int>(frame_width), static_cast<int>(frame_height));
		if (roiRect.width <= 0 || roiRect.height <= 0) {
			continue;
		}

		cv::Mat roiBgra = frameMat(roiRect);

		int resized_w = static_cast<int>(std::round((static_cast<float>(roiRect.width) * static_cast<float>(kTargetHeight)) / static_cast<float>(roiRect.height)));
		resized_w = ClampInt(resized_w, 1, kMaxWidth);

		cv::Mat resizedBgra;
		cv::resize(roiBgra, resizedBgra, cv::Size(resized_w, kTargetHeight), 0, 0, cv::INTER_LINEAR);

		// 左上寄せ、右側パディング用の kMaxWidth x kTargetHeight のキャンバスを作成
		// ((0 - 127.5) / 127.5) = -1.0 ではなく、0埋め(黒)に対応するパディング(BGRA: 0,0,0,0)
		cv::Mat canvas = cv::Mat::zeros(kTargetHeight, kMaxWidth, CV_8UC4);
		resizedBgra.copyTo(canvas(cv::Rect(0, 0, resized_w, kTargetHeight)));

		// PP-OCRの正規化: (px / 255.0 - 0.5) / 0.5 == (px - 127.5) / 127.5 == (px - 127.5) * (1 / 127.5)
		// BGR順序（チャンネル0: B, 1: G, 2: R）
		// cv::dnn::blobFromImage:
		//   scalefactor = 1.0 / 127.5
		//   mean = cv::Scalar(127.5, 127.5, 127.5)
		//   swapRB = false (BGRAの先頭3チャンネルBGRを保持)
		cv::Mat blob = cv::dnn::blobFromImage(
			canvas,
			1.0 / 127.5,
			cv::Size(kMaxWidth, kTargetHeight),
			cv::Scalar(127.5, 127.5, 127.5),
			false, // swapRB = false
			false, // crop
			CV_32F
		);

		float* dst = input.data() + (i * sample_elements);
		std::memcpy(dst, blob.ptr<float>(), sample_elements * sizeof(float));

		out_track_ids[i] = item.track_id;
	}

	return input;
}

OcrTrackResult OcrRecognizer::DecodeSingle(
	uint64_t track_id,
	const float* logits,
	int64_t time_steps,
	int64_t vocab_size) const
{
	OcrTrackResult result;
	result.track_id = track_id;

	if (!logits || time_steps <= 0 || vocab_size <= 0) {
		return result;
	}

	int prev_idx = -1;
	float conf_sum = 0.0f;
	int conf_count = 0;

	for (int64_t t = 0; t < time_steps; ++t) {
		const float* row = logits + (t * vocab_size);

		int best_idx = 0;
		float best_score = row[0];
		for (int64_t c = 1; c < vocab_size; ++c) {
			if (row[c] > best_score) {
				best_score = row[c];
				best_idx = static_cast<int>(c);
			}
		}

		if (best_idx == 0 || best_idx == prev_idx) {
			prev_idx = best_idx;
			continue;
		}

		if (best_idx >= 0 && static_cast<size_t>(best_idx) < dictionary_.size()) {
			result.text += dictionary_[static_cast<size_t>(best_idx)];
			conf_sum += best_score;
			++conf_count;
		}

		prev_idx = best_idx;
	}

	result.confidence = (conf_count > 0) ? (conf_sum / static_cast<float>(conf_count)) : 0.0f;
	result.text = ApplySpaceCandidateFallback(result.text);
	return result;
}

} // namespace WoLNamesBlackedOut::Core
