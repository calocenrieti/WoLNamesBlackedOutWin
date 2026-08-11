#include "pch.h"
#include "WinMLUtils.h"
#include "CoreTypes.h"
#include <iostream>
#include <mutex>
#include <winml/dml_provider_factory.h>
extern "C" {
#include <libswscale/swscale.h>
}

namespace {

struct WinMLSessionCacheEntry {
	std::shared_ptr<Ort::Session> session;
	std::vector<std::string> input_names;
	std::vector<std::string> output_names;
	std::string ep_display_name;
};

std::string NormalizeEpDisplayName(const std::string& ep_name) {
	if (ep_name.empty()) {
		return "CPU";
	}

	if (ep_name == "NvTensorRTRTXExecutionProvider") {
		return "TensorRT-RTX";
	}

	if (ep_name == "DmlExecutionProvider") {
		return "DirectML";
	}

	if (ep_name == "MIGraphXExecutionProvider") {
		return "MIGraphX";
	}

	if (ep_name == "OpenVINOExecutionProvider") {
		return "OpenVINO";
	}

	return ep_name;
}

std::mutex& GetWinMLSessionCacheMutex() {
	static auto* cache_mutex = new std::mutex();
	return *cache_mutex;
}

std::unordered_map<std::string, WinMLSessionCacheEntry>& GetWinMLSessionCache() {
	static auto* cache = new std::unordered_map<std::string, WinMLSessionCacheEntry>();
	return *cache;
}

std::mutex& GetWinMLSessionBuildMutex() {
	static auto* build_mutex = new std::mutex();
	return *build_mutex;
}

std::shared_ptr<Ort::Env> GetSharedOrtEnv() {
	static std::shared_ptr<Ort::Env> shared_env = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "WoLNamesBlackedOut");
	return shared_env;
}

std::string WideToUtf8(const wchar_t* value) {
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

std::string BuildWinMLSessionCacheKey(const wchar_t* model_path, WoLNamesBlackedOut::Core::GpuVendor vendor) {
	std::string key = WideToUtf8(model_path);
	key += "|gpu=";
	key += std::to_string(static_cast<int>(vendor));
	return key;
}

// 外部から設定された状態通知コールバック
StatusCallback g_status_callback = nullptr;
std::mutex g_status_callback_mutex;

void ReportStatus(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_status_callback_mutex);
    if (!g_status_callback) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_status_callback(buf);
}

extern "C" __declspec(dllexport) void __stdcall SetWinMLStatusCallback(StatusCallback callback) {
    std::lock_guard<std::mutex> lock(g_status_callback_mutex);
    g_status_callback = callback;
}

// WinML EPカタログ列挙コールバック
BOOL CALLBACK EpCatalogEnumCallback(
    _In_ WinMLEpHandle ep,
    _In_ const WinMLEpInfo* info,
    _In_opt_ void* context) {
    (void)context;

    auto get_string = [](WinMLEpHandle handle,
                         HRESULT(STDMETHODCALLTYPE* get_size)(WinMLEpHandle, size_t*),
                         HRESULT(STDMETHODCALLTYPE* get_value)(WinMLEpHandle, size_t, char*, size_t*)) -> std::string {
        size_t size = 0;
        if (FAILED(get_size(handle, &size)) || size == 0) return {};
        std::string buffer(size, '\0');
        size_t used = 0;
        if (SUCCEEDED(get_value(handle, buffer.size(), buffer.data(), &used))) {
            if (used > 0 && used <= buffer.size()) buffer.resize(used);
        }
        return buffer;
    };

    std::string name = info && info->name ? std::string(info->name) : get_string(ep, WinMLEpGetNameSize, WinMLEpGetName);
    std::string version = info && info->version ? std::string(info->version) : get_string(ep, WinMLEpGetVersionSize, WinMLEpGetVersion);
    std::string pfn = info && info->packageFamilyName ? std::string(info->packageFamilyName) : get_string(ep, WinMLEpGetPackageFamilyNameSize, WinMLEpGetPackageFamilyName);
    std::string lib_path = info && info->libraryPath ? std::string(info->libraryPath) : get_string(ep, WinMLEpGetLibraryPathSize, WinMLEpGetLibraryPath);

    const char* ready_state = "Unknown";
    if (info) {
        switch (info->readyState) {
            case WinMLEpReadyState_Ready: ready_state = "Ready"; break;
            case WinMLEpReadyState_NotReady: ready_state = "NotReady"; break;
            case WinMLEpReadyState_NotPresent: ready_state = "NotPresent"; break;
        }
    }

    const char* certification = "Unknown";
    if (info) {
        switch (info->certification) {
            case WinMLEpCertification_Certified: certification = "Certified"; break;
            case WinMLEpCertification_Uncertified: certification = "Uncertified"; break;
            default: certification = "Unknown"; break;
        }
    }

    char dbg[1024];
    snprintf(dbg, sizeof(dbg),
        "[WinML EP Catalog] name=%s, version=%s, ready=%s, cert=%s, pfn=%s, lib=%s\n",
        name.c_str(), version.c_str(), ready_state, certification, pfn.c_str(), lib_path.c_str());
    OutputDebugStringA(dbg);

    return TRUE; // 次のEPへ続行
}

} // namespace

namespace WoLNamesBlackedOut::Core {

WinMLUtils::WinMLUtils() : initialized_(false), session_(nullptr) {
}

WinMLUtils::~WinMLUtils() {
	Uninitialize();
}

void WinMLUtils::SetD3D11Device(ID3D11Device* device, ID3D11DeviceContext* context) {
	device_ = device;
	context_ = context;
}

bool WinMLUtils::Initialize() {
	if (initialized_) return true;

	try {
		OutputDebugStringA("[WinMLUtils] Acquiring shared Ort::Env...\n");
		env_ = GetSharedOrtEnv();

		// COM初期化（S_FALSE=既初期化済み、RPC_E_CHANGED_MODE=別モードで初期化済みはいずれも続行可）
		HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
		if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
			char dbg[128];
			snprintf(dbg, sizeof(dbg), "[WinMLUtils] RoInitialize failed: 0x%08X\n", (unsigned)hr);
			OutputDebugStringA(dbg);
			return false;
		}
		OutputDebugStringA("[WinMLUtils] Initialize OK\n");

		// GPU前処理シェーダー初期化
		if (device_ && context_) {
			preprocess_shader_ = std::make_unique<PreprocessShader>();
			if (preprocess_shader_->Initialize(device_.Get(), context_.Get())) {
				OutputDebugStringA("[WinMLUtils] PreprocessShader initialized\n");
			} else {
				OutputDebugStringA("[WinMLUtils] PreprocessShader init failed, falling back to CPU preprocess\n");
				preprocess_shader_.reset();
			}
		}

		initialized_ = true;
		LogEpCatalog();
		return true;
	}
	catch (const std::exception& ex) {
		char dbg[256];
		snprintf(dbg, sizeof(dbg), "[WinMLUtils] Initialize exception: %s\n", ex.what());
		OutputDebugStringA(dbg);
		return false;
	}
}

void WinMLUtils::Uninitialize() {
	ReleaseIoBinding();
	session_.reset();
	if (initialized_) {
		RoUninitialize();
		initialized_ = false;
	}
	env_.reset();
	input_names_.clear();
	output_names_.clear();
}

void WinMLUtils::LogEpCatalog() const {
    OutputDebugStringA("[WinMLUtils] Enumerating WinML EP catalog...\n");
    WinMLEpCatalogHandle catalog = nullptr;
    HRESULT hr = WinMLEpCatalogCreate(&catalog);
    if (FAILED(hr) || !catalog) {
        char dbg[256];
        snprintf(dbg, sizeof(dbg), "[WinMLUtils] WinMLEpCatalogCreate failed: 0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return;
    }

    hr = WinMLEpCatalogEnumProviders(catalog, EpCatalogEnumCallback, nullptr);
    if (FAILED(hr)) {
        char dbg[256];
        snprintf(dbg, sizeof(dbg), "[WinMLUtils] WinMLEpCatalogEnumProviders failed: 0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
    }

    WinMLEpCatalogRelease(catalog);
}

bool WinMLUtils::EnsureEpReady(const char* provider_name) {
    if (!provider_name) return false;

    char dbg[512];
    snprintf(dbg, sizeof(dbg), "[WinMLUtils] EnsureEpReady: %s\n", provider_name);
    OutputDebugStringA(dbg);

    WinMLEpCatalogHandle catalog = nullptr;
    HRESULT hr = WinMLEpCatalogCreate(&catalog);
    if (FAILED(hr) || !catalog) {
        snprintf(dbg, sizeof(dbg), "[WinMLUtils] WinMLEpCatalogCreate failed: 0x%08X\n", (unsigned)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    WinMLEpHandle ep = nullptr;
    hr = WinMLEpCatalogFindProvider(catalog, provider_name, nullptr, &ep);
    if (FAILED(hr) || !ep) {
        snprintf(dbg, sizeof(dbg), "[WinMLUtils] WinMLEpCatalogFindProvider(%s) failed: 0x%08X\n", provider_name, (unsigned)hr);
        OutputDebugStringA(dbg);
        WinMLEpCatalogRelease(catalog);
        return false;
    }

    WinMLEpReadyState state = WinMLEpReadyState_NotPresent;
    hr = WinMLEpGetReadyState(ep, &state);
    if (SUCCEEDED(hr)) {
        const char* state_str = "Unknown";
        switch (state) {
            case WinMLEpReadyState_Ready: state_str = "Ready"; break;
            case WinMLEpReadyState_NotReady: state_str = "NotReady"; break;
            case WinMLEpReadyState_NotPresent: state_str = "NotPresent"; break;
        }
        snprintf(dbg, sizeof(dbg), "[WinMLUtils] EP %s state: %s\n", provider_name, state_str);
        OutputDebugStringA(dbg);
    }

    std::string provider_display_name(provider_name ? provider_name : "");
    if (provider_display_name == "NvTensorRTRTXExecutionProvider") {
        provider_display_name = "TensorRT-RTX";
    }

    bool result = false;
    if (state == WinMLEpReadyState_Ready) {
        result = true;
    } else if (state == WinMLEpReadyState_NotPresent || state == WinMLEpReadyState_NotReady) {
        ReportStatus("Downloading %s execution provider...", provider_display_name.c_str());
        OutputDebugStringA("[WinMLUtils] Calling WinMLEpEnsureReady...\n");
        hr = WinMLEpEnsureReady(ep);
        if (SUCCEEDED(hr)) {
            // 再度状態を確認
            hr = WinMLEpGetReadyState(ep, &state);
            if (SUCCEEDED(hr) && state == WinMLEpReadyState_Ready) {
                ReportStatus("%s execution provider ready", provider_display_name.c_str());
                snprintf(dbg, sizeof(dbg), "[WinMLUtils] EP %s is now Ready\n", provider_name);
                OutputDebugStringA(dbg);
                result = true;
            } else {
                ReportStatus("%s execution provider preparation failed", provider_display_name.c_str());
                snprintf(dbg, sizeof(dbg), "[WinMLUtils] EP %s still not Ready after EnsureReady (state=%d)\n", provider_name, (int)state);
                OutputDebugStringA(dbg);
            }
        } else {
            ReportStatus("%s execution provider download failed", provider_display_name.c_str());
            snprintf(dbg, sizeof(dbg), "[WinMLUtils] WinMLEpEnsureReady failed: 0x%08X\n", (unsigned)hr);
            OutputDebugStringA(dbg);
        }
    }

    // ONNX Runtime に EP ライブラリを登録（Microsoft Learn 推奨フロー）
    if (result && env_) {
        size_t pathSize = 0;
        hr = WinMLEpGetLibraryPathSize(ep, &pathSize);
        if (SUCCEEDED(hr) && pathSize > 0) {
            std::string libraryPathUtf8(pathSize, '\0');
            hr = WinMLEpGetLibraryPath(ep, pathSize, libraryPathUtf8.data(), nullptr);
            if (SUCCEEDED(hr)) {
                // null terminator を含む可能性があるので trim
                size_t len = strlen(libraryPathUtf8.c_str());
                libraryPathUtf8.resize(len);
                // UTF-8 -> UTF-16(wstring) 変換
                std::wstring libraryPathW;
                int wlen = MultiByteToWideChar(CP_UTF8, 0, libraryPathUtf8.c_str(), -1, nullptr, 0);
                if (wlen > 0) {
                    libraryPathW.resize(static_cast<size_t>(wlen) - 1);
                    MultiByteToWideChar(CP_UTF8, 0, libraryPathUtf8.c_str(), -1, libraryPathW.data(), wlen);
                }
                try {
                    env_->RegisterExecutionProviderLibrary(provider_name, libraryPathW);
                    snprintf(dbg, sizeof(dbg), "[WinMLUtils] Registered EP library: %s\n", libraryPathUtf8.c_str());
                    OutputDebugStringA(dbg);
                } catch (const Ort::Exception& ex) {
                    snprintf(dbg, sizeof(dbg), "[WinMLUtils] RegisterExecutionProviderLibrary failed: %s\n", ex.what());
                    OutputDebugStringA(dbg);
                    result = false;
                }
            }
        } else {
            OutputDebugStringA("[WinMLUtils] WinMLEpGetLibraryPathSize failed or empty\n");
        }
    }

    WinMLEpCatalogRelease(catalog);
    return result;
}

bool WinMLUtils::LoadModel(const wchar_t* model_path) {
	if (!model_path) {
		std::cerr << "Invalid model path." << std::endl;
		OutputDebugStringA("[WinMLUtils] LoadModel: model_path is null\n");
		return false;
	}

	ReleaseIoBinding();
	session_.reset();
	input_names_.clear();
	output_names_.clear();

	if (!env_) {
		env_ = GetSharedOrtEnv();
	}

	const std::string cache_key = BuildWinMLSessionCacheKey(model_path, gpu_vendor_);
	std::lock_guard<std::mutex> build_lock(GetWinMLSessionBuildMutex());
	{
		auto& session_cache = GetWinMLSessionCache();
		std::lock_guard<std::mutex> lock(GetWinMLSessionCacheMutex());
		auto it = session_cache.find(cache_key);
		if (it != session_cache.end()) {
			session_ = it->second.session;
			input_names_ = it->second.input_names;
			output_names_ = it->second.output_names;
			std::string ep_display = NormalizeEpDisplayName(it->second.ep_display_name);
			OutputDebugStringA("[WinMLUtils] Reusing cached ONNX session\n");
			ReportStatus("Reusing EP: %s", ep_display.c_str());
			if (InitializeIoBinding()) {
				OutputDebugStringA("[WinMLUtils] IoBinding ready for reused session\n");
			}
			return true;
		}
	}

	try {
		char model_narrow[400] = {};
		WideCharToMultiByte(CP_ACP, 0, model_path, -1, model_narrow, sizeof(model_narrow)-1, nullptr, nullptr);
		char dbg[512]; snprintf(dbg, sizeof(dbg), "[WinMLUtils] LoadModel: '%s'\n", model_narrow);
		OutputDebugStringA(dbg);

		// ONNX Runtime セッションを作成（正しいコンストラクタ使用）
		Ort::SessionOptions session_options;
		session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

		// 利用可能なEP一覧を取得
		std::vector<std::string> available_providers;
		try {
			available_providers = Ort::GetAvailableProviders();
			std::string provider_list;
			for (const auto& provider : available_providers) {
				if (!provider_list.empty()) provider_list += ", ";
				provider_list += provider;
			}
			char dbg_providers[512];
			snprintf(dbg_providers, sizeof(dbg_providers), "[WinMLUtils] Available providers: %s\n", provider_list.c_str());
			OutputDebugStringA(dbg_providers);
		}
		catch (const Ort::Exception& ex) {
			char dbg[256];
			snprintf(dbg, sizeof(dbg), "[WinMLUtils] GetAvailableProviders failed: %s\n", ex.what());
			OutputDebugStringA(dbg);
		}

		auto has_ep = [&available_providers](const std::string& name) {
			return std::find(available_providers.begin(), available_providers.end(), name) != available_providers.end();
		};

		// ヘルパー: WinMLカタログEPをダウンロード・登録し、AppendExecutionProvider_V2 で追加
		auto try_append_catalog_ep = [&](const char* ep_name,
			const std::unordered_map<std::string, std::string>& ep_options = {}) -> bool {
			// EnsureEpReady でダウンロード・ONNX Runtime登録まで完了
			// (既にReadyの場合もRegisterExecutionProviderLibraryを呼ぶ)
			if (!EnsureEpReady(ep_name)) {
				return false;
			}
			// GetEpDevices() から該当EPを検索して AppendExecutionProvider_V2
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
			} catch (const Ort::Exception& ex) {
				char dbg[512];
				snprintf(dbg, sizeof(dbg), "[WinMLUtils] %s AppendExecutionProvider_V2 failed: %s\n", ep_name, ex.what());
				OutputDebugStringA(dbg);
			}
			return false;
		};

		// GPUベンダーに応じたEP優先順位で選択（WinML 2.x 推奨EP名）
		// カタログEP（NvTensorRTRTX/MIGraphX/OpenVINO）は EnsureEpReady + AppendExecutionProvider_V2 が必要
		// 組み込みEP（DML/CUDA/TensorRT）は従来のAppendExecutionProvider APIを使用
		std::string selected_ep;
		std::string ep_reason;
		bool ep_appended = false;
		switch (gpu_vendor_) {
		case GpuVendor::NVIDIA: {
			// NvTensorRTRTX EPの最適化オプション
			// YOLOv8固定サイズ(1x3x640x640)の最適化プロファイル + ワークスペース + ランタイムキャッシュ
			std::unordered_map<std::string, std::string> trt_rtx_options = {
				{"nv_profile_min_shapes", "images:1x3x736x1280"},
				{"nv_profile_max_shapes", "images:1x3x736x1280"},
				{"nv_profile_opt_shapes", "images:1x3x736x1280"},
				{"nv_max_workspace_size", "4294967296"},  // 4GB
			};
			// ランタイムキャッシュディレクトリを設定（JITカーネルキャッシュ）
			// パッケージアプリ対応: LocalFolderを使用（書き込み可能なアプリデータ領域）
			std::string cache_path_str;
			try {
				auto local_folder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
				std::wstring cache_path_w = local_folder.Path().c_str();
				cache_path_w += L"\\trt_rtx_cache";
				CreateDirectoryW(cache_path_w.c_str(), nullptr);
				// UTF-16 -> UTF-8 変換
				int utf8_len = WideCharToMultiByte(CP_UTF8, 0, cache_path_w.c_str(), -1, nullptr, 0, nullptr, nullptr);
				if (utf8_len > 0) {
					cache_path_str.resize(utf8_len - 1);
					WideCharToMultiByte(CP_UTF8, 0, cache_path_w.c_str(), -1, cache_path_str.data(), utf8_len, nullptr, nullptr);
				}
				trt_rtx_options["nv_runtime_cache_path"] = cache_path_str;
				char dbg[512];
				snprintf(dbg, sizeof(dbg), "[WinMLUtils] TensorRT-RTX runtime cache (LocalFolder): %s\n", cache_path_str.c_str());
				OutputDebugStringA(dbg);
			} catch (...) {
				// WinRT APIが使えない場合は従来のパスにフォールバック
				char cache_path[MAX_PATH];
				if (GetModuleFileNameA(nullptr, cache_path, MAX_PATH)) {
					char* last_slash = strrchr(cache_path, '\\');
					if (last_slash) {
						*(last_slash + 1) = '\0';
						strcat_s(cache_path, sizeof(cache_path), "trt_rtx_cache");
						CreateDirectoryA(cache_path, nullptr);
						trt_rtx_options["nv_runtime_cache_path"] = std::string(cache_path);
						char dbg[512];
						snprintf(dbg, sizeof(dbg), "[WinMLUtils] TensorRT-RTX runtime cache (fallback): %s\n", cache_path);
						OutputDebugStringA(dbg);
					}
				}
			}
			if (try_append_catalog_ep("NvTensorRTRTXExecutionProvider", trt_rtx_options)) {
				selected_ep = "TensorRT-RTX";
				ep_reason = "NVIDIA GPU: TensorRT-RTX selected (profile shapes + 4GB workspace + runtime cache)";
				ep_appended = true;
			} else if (has_ep("DmlExecutionProvider")) {
				selected_ep = "DmlExecutionProvider";
				ep_reason = "NVIDIA GPU: DirectML selected (TensorRT-RTX not available)";
			}
			break;
		}
		case GpuVendor::AMD:
			if (try_append_catalog_ep("MIGraphXExecutionProvider")) {
				selected_ep = "MIGraphXExecutionProvider";
				ep_reason = "AMD GPU: MIGraphX selected";
				ep_appended = true;
			} else if (has_ep("DmlExecutionProvider")) {
				selected_ep = "DmlExecutionProvider";
				ep_reason = "AMD GPU: DirectML selected (MIGraphX not available)";
			}
			break;
		case GpuVendor::Intel:
			if (try_append_catalog_ep("OpenVINOExecutionProvider")) {
				selected_ep = "OpenVINOExecutionProvider";
				ep_reason = "Intel GPU: OpenVINO selected";
				ep_appended = true;
			} else if (has_ep("DmlExecutionProvider")) {
				selected_ep = "DmlExecutionProvider";
				ep_reason = "Intel GPU: DirectML selected (OpenVINO not available)";
			}
			break;
		default:
			if (has_ep("DmlExecutionProvider")) {
				selected_ep = "DmlExecutionProvider";
				ep_reason = "Unknown GPU: DirectML selected";
			}
			break;
		}

		// 組み込みEPの追加（カタログEPは try_append_catalog_ep で既に追加済み）
		if (!selected_ep.empty() && !ep_appended) {
			try {
				if (selected_ep == "DmlExecutionProvider") {
					const OrtDmlApi* dml_api = nullptr;
					Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dml_api)));
					OrtDmlDeviceOptions device_options;
					device_options.Preference = OrtDmlPerformancePreference::HighPerformance;
					device_options.Filter = OrtDmlDeviceFilter::Gpu;
					Ort::ThrowOnError(dml_api->SessionOptionsAppendExecutionProvider_DML2(session_options, &device_options));
					ep_appended = true;
				}
			}
			catch (const Ort::Exception& ex) {
				char dbg[512];
				snprintf(dbg, sizeof(dbg), "[WinMLUtils] Failed to append %s: %s\n", selected_ep.c_str(), ex.what());
				OutputDebugStringA(dbg);
			}
		}

		if (ep_appended) {
			char dbg[512];
			snprintf(dbg, sizeof(dbg), "[WinMLUtils] %s\n", ep_reason.c_str());
			OutputDebugStringA(dbg);
		} else {
			OutputDebugStringA("[WinMLUtils] No GPU EP available, using CPU\n");
		}

		std::string ep_display_name = NormalizeEpDisplayName(selected_ep);
		ReportStatus("Initializing EP: %s", ep_display_name.c_str());
		auto created_session = std::make_shared<Ort::Session>(*env_, model_path, session_options);
		session_ = created_session;
		ReportStatus("EP ready: %s", ep_display_name.c_str());

		// 利用可能なEP一覧をログ出力（SetEpSelectionPolicyで自動選択されたEPの確認用）
		try {
			std::vector<std::string> available_providers = Ort::GetAvailableProviders();
			std::string provider_list;
			for (const auto& provider : available_providers) {
				if (!provider_list.empty()) provider_list += ", ";
				provider_list += provider;
			}
			char dbg_providers[512];
			snprintf(dbg_providers, sizeof(dbg_providers), "[WinMLUtils] Available providers: %s\n", provider_list.c_str());
			OutputDebugStringA(dbg_providers);
		}
		catch (const Ort::Exception& ex) {
			char dbg[256];
			snprintf(dbg, sizeof(dbg), "[WinMLUtils] GetAvailableProviders failed: %s\n", ex.what());
			OutputDebugStringA(dbg);
		}

		// モデルの入力名を取得
		size_t num_inputs = session_->GetInputCount();
		input_names_.clear();
		for (size_t i = 0; i < num_inputs; ++i) {
			auto name_alloc = session_->GetInputNameAllocated(i, Ort::AllocatorWithDefaultOptions{});
			input_names_.push_back(std::string(name_alloc.get()));
		}

		// モデルの出力名を取得
		size_t num_outputs = session_->GetOutputCount();
		output_names_.clear();
		for (size_t i = 0; i < num_outputs; ++i) {
			auto name_alloc = session_->GetOutputNameAllocated(i, Ort::AllocatorWithDefaultOptions{});
			output_names_.push_back(std::string(name_alloc.get()));
		}

		// IoBindingを初期化（推論メモリの事前確保・再利用）
		if (InitializeIoBinding()) {
			OutputDebugStringA("[WinMLUtils] IoBinding ready for inference optimization\n");
		} else {
			OutputDebugStringA("[WinMLUtils] IoBinding init failed, using standard inference path\n");
		}

		char dbg2[256];
		snprintf(dbg2, sizeof(dbg2), "[WinMLUtils] LoadModel OK: inputs=%zu outputs=%zu\n",
			input_names_.size(), output_names_.size());
		OutputDebugStringA(dbg2);
		{
			auto& session_cache = GetWinMLSessionCache();
			std::lock_guard<std::mutex> lock(GetWinMLSessionCacheMutex());
			session_cache[cache_key] = WinMLSessionCacheEntry{ session_, input_names_, output_names_, ep_display_name };
		}

		return true;
	}
	catch (const Ort::Exception& ex) {
		char dbg[512]; snprintf(dbg, sizeof(dbg), "[WinMLUtils] LoadModel Ort::Exception: %s\n", ex.what());
		OutputDebugStringA(dbg);
		return false;
	}
	catch (const std::exception& ex) {
		char dbg[512]; snprintf(dbg, sizeof(dbg), "[WinMLUtils] LoadModel std::exception: %s\n", ex.what());
		OutputDebugStringA(dbg);
		return false;
	}
}

Ort::MemoryInfo WinMLUtils::CreateInputMemoryInfo(ID3D11Texture2D* texture, uint32_t width, uint32_t height) {
	// D3D11テクスチャからメモリ情報を取得（GPUメモリアロケータ使用）
	if (!texture) return Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

	Microsoft::WRL::ComPtr<IDXGIResource> dxgi_resource;
	HRESULT hr = texture->QueryInterface(IID_PPV_ARGS(&dxgi_resource));
	if (SUCCEEDED(hr) && dxgi_resource) {
		// DXGIリソースが有効な場合、CPUメモリアロケータを使用（D3D11メモリオプションは省略）
		return Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
	}

	return Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
}

std::vector<float> WinMLUtils::Evaluate(
	const std::string& input_name,
	const float* input_tensor_data,
	size_t data_size,
	const std::vector<int64_t>& input_shape) {
	if (!session_) {
		throw std::runtime_error("Model not loaded");
	}

	try {
		// 入力テンソル作成
		auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
		Ort::Value input_tensor = Ort::Value::CreateTensor<float>(memory_info, 
			const_cast<float*>(input_tensor_data), data_size, 
			input_shape.data(), input_shape.size());

		// 出力名（事前に取得済み）
		std::vector<const char*> input_names_arr{ const_cast<char*>(input_name.c_str()) };
		std::vector<const char*> output_names_arr{ output_names_[0].c_str() };

		std::vector<Ort::Value> output_tensors = session_->Run(
			Ort::RunOptions{nullptr},
			input_names_arr.data(), &input_tensor, 1,
			output_names_arr.data(), 1);

		// 出力データを取得
		auto tensor_info = output_tensors[0].GetTensorTypeAndShapeInfo();
		auto element_count = tensor_info.GetElementCount();
		
		const float* output_data = output_tensors[0].GetTensorData<float>();
		std::vector<float> result(output_data, output_data + element_count);

		return result;
	}
	catch (const Ort::Exception& ex) {
		std::cerr << "Inference failed: " << ex.what() << std::endl;
		throw;
	}
}

std::vector<float> WinMLUtils::EvaluateFromTexture(
	const std::string& input_name,
	ID3D11Texture2D* texture,
	uint32_t width,
	uint32_t height) {
	if (!session_ || !texture) {
		throw std::runtime_error("Model not loaded or invalid texture");
	}

	try {
		// モデルの入力形状を取得
		std::vector<int64_t> model_shape = GetInputShape(input_name);
		if (model_shape.empty() || model_shape.size() != 4) {
			model_shape = { 1, 3, static_cast<int64_t>(height), static_cast<int64_t>(width) };
		}

		int64_t model_h = model_shape[2] > 0 ? model_shape[2] : static_cast<int64_t>(height);
		int64_t model_w = model_shape[3] > 0 ? model_shape[3] : static_cast<int64_t>(width);
		std::vector<int64_t> input_shape = { 1, 3, model_h, model_w };

		std::vector<float> final_input_data;
		bool used_gpu_preprocess = false;

		// 既定ではCPU前処理を使用し、PreviewPipelineと同じ入力生成経路に揃える。
		// 必要な場合のみ環境変数 WOL_USE_GPU_PREPROCESS=1 でGPU前処理を有効化できる。
		bool allow_gpu_preprocess = false;
		{
			char env_buf[8] = {};
			DWORD got = GetEnvironmentVariableA("WOL_USE_GPU_PREPROCESS", env_buf, static_cast<DWORD>(sizeof(env_buf)));
			if (got > 0 && (env_buf[0] == '1' || env_buf[0] == 'T' || env_buf[0] == 't' || env_buf[0] == 'Y' || env_buf[0] == 'y')) {
				allow_gpu_preprocess = true;
			}
		}

		// GPU Compute Shader前処理パス（PreprocessShaderが初期化済みの場合）
		if (allow_gpu_preprocess && preprocess_shader_ && device_ && context_) {
			auto gpu_start = std::chrono::high_resolution_clock::now();
			if (preprocess_shader_->Process(texture, width, height,
				static_cast<uint32_t>(model_w), static_cast<uint32_t>(model_h),
				final_input_data)) {
				used_gpu_preprocess = true;
				auto gpu_end = std::chrono::high_resolution_clock::now();
				auto gpu_ms = std::chrono::duration_cast<std::chrono::milliseconds>(gpu_end - gpu_start).count();
				char dbg[128];
				snprintf(dbg, sizeof(dbg), "[WinMLUtils] GPU preprocess completed in %lld ms\n", gpu_ms);
				OutputDebugStringA(dbg);
			} else {
				OutputDebugStringA("[WinMLUtils] GPU preprocess failed, falling back to CPU\n");
			}
		}

		// CPUフォールバックパス（GPU前処理が失敗した場合）
		if (!used_gpu_preprocess) {
			auto cpu_start = std::chrono::high_resolution_clock::now();

			// テクスチャからCPUメモリにデータをコピー
			D3D11_TEXTURE2D_DESC src_desc = {};
			texture->GetDesc(&src_desc);

			D3D11_TEXTURE2D_DESC staging_desc = {};
			staging_desc.Width = src_desc.Width;
			staging_desc.Height = src_desc.Height;
			staging_desc.MipLevels = 1;
			staging_desc.ArraySize = 1;
			staging_desc.Format = src_desc.Format;
			staging_desc.SampleDesc.Count = 1;
			staging_desc.Usage = D3D11_USAGE_STAGING;
			staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

			Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture;
			HRESULT hr = device_->CreateTexture2D(&staging_desc, nullptr, staging_texture.ReleaseAndGetAddressOf());
			if (FAILED(hr)) {
				throw std::runtime_error("Failed to create staging texture");
			}

			context_->CopyResource(staging_texture.Get(), texture);
			context_->Flush();

			D3D11_MAPPED_SUBRESOURCE mapped_resource;
			hr = context_->Map(staging_texture.Get(), 0, D3D11_MAP_READ, 0, &mapped_resource);
			if (FAILED(hr)) {
				throw std::runtime_error("Failed to map staging texture for reading");
			}

			size_t row_pitch = mapped_resource.RowPitch;
			size_t pixel_size = width * height * 4;
			std::vector<uint8_t> cpu_buffer(pixel_size);

			const uint8_t* src = static_cast<const uint8_t*>(mapped_resource.pData);
			for (uint32_t y = 0; y < height; ++y) {
				memcpy(cpu_buffer.data() + y * width * 4, src + y * row_pitch, width * 4);
			}
			context_->Unmap(staging_texture.Get(), 0);

			// PreviewPipelineと同じ前処理: 左上揃えリサイズ（余白は黒）
			float scale_x = static_cast<float>(width) / static_cast<float>(model_w);
			float scale_y = static_cast<float>(height) / static_cast<float>(model_h);
			float resizeScales = std::max(scale_x, scale_y);
			int scaled_w = (scale_x >= scale_y) ? static_cast<int>(model_w) : static_cast<int>(width / resizeScales);
			int scaled_h = (scale_y >= scale_x) ? static_cast<int>(model_h) : static_cast<int>(height / resizeScales);
			scaled_w = std::max(1, std::min(static_cast<int>(model_w), scaled_w));
			scaled_h = std::max(1, std::min(static_cast<int>(model_h), scaled_h));

			final_input_data.resize(static_cast<size_t>(model_w * model_h * 3), 0.0f);
			std::vector<uint8_t> rgb_scaled(static_cast<size_t>(scaled_w * scaled_h * 3));

			struct SwsContext* sws = sws_getContext(
				static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_BGRA,
				scaled_w, scaled_h, AV_PIX_FMT_RGB24,
				SWS_BILINEAR, nullptr, nullptr, nullptr);
			if (sws) {
				const uint8_t* src_planes[1] = { cpu_buffer.data() };
				int src_strides[1] = { static_cast<int>(width * 4) };
				uint8_t* dst_planes[1] = { rgb_scaled.data() };
				int dst_strides[1] = { scaled_w * 3 };
				sws_scale(sws, src_planes, src_strides, 0, static_cast<int>(height), dst_planes, dst_strides);
				sws_freeContext(sws);

				for (int y = 0; y < scaled_h; ++y) {
					for (int x = 0; x < scaled_w; ++x) {
						const uint8_t* p = &rgb_scaled[(static_cast<size_t>(y) * scaled_w + x) * 3];
						size_t dst_idx = static_cast<size_t>(y) * static_cast<size_t>(model_w) + static_cast<size_t>(x);
						final_input_data[0 * static_cast<size_t>(model_w * model_h) + dst_idx] = static_cast<float>(p[0]) / 255.0f;
						final_input_data[1 * static_cast<size_t>(model_w * model_h) + dst_idx] = static_cast<float>(p[1]) / 255.0f;
						final_input_data[2 * static_cast<size_t>(model_w * model_h) + dst_idx] = static_cast<float>(p[2]) / 255.0f;
					}
				}
			}

			auto cpu_end = std::chrono::high_resolution_clock::now();
			auto cpu_ms = std::chrono::duration_cast<std::chrono::milliseconds>(cpu_end - cpu_start).count();
			char dbg[128];
			snprintf(dbg, sizeof(dbg), "[WinMLUtils] CPU preprocess completed in %lld ms\n", cpu_ms);
			OutputDebugStringA(dbg);
		}

		char dbg[256];
		snprintf(dbg, sizeof(dbg), "[WinMLUtils] EvaluateFromTexture: shape=[%lld,%lld,%lld,%lld], data_size=%zu, gpu_preprocess=%s\n",
			input_shape[0], input_shape[1], input_shape[2], input_shape[3], final_input_data.size(),
			used_gpu_preprocess ? "yes" : "no");
		OutputDebugStringA(dbg);

		// ONNX Runtimeで推論実行（実行時間計測）
		auto inference_start = std::chrono::high_resolution_clock::now();

		std::vector<float> result;
		bool allow_iobinding = false;
		{
			char env_buf[8] = {};
			DWORD got = GetEnvironmentVariableA("WOL_USE_IOBINDING", env_buf, static_cast<DWORD>(sizeof(env_buf)));
			if (got > 0 && (env_buf[0] == '1' || env_buf[0] == 'T' || env_buf[0] == 't' || env_buf[0] == 'Y' || env_buf[0] == 'y')) {
				allow_iobinding = true;
			}
		}

		if (allow_iobinding && io_binding_initialized_ && io_binding_) {
			// IoBindingパス: 入力バッファは再利用し、出力は毎回実際のテンソル形状で受け取る。
			if (input_buffer_.size() != final_input_data.size()) {
				input_buffer_.resize(final_input_data.size());
			}
			memcpy(input_buffer_.data(), final_input_data.data(), final_input_data.size() * sizeof(float));

			io_binding_->ClearBoundInputs();
			io_binding_->ClearBoundOutputs();

			auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
			Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
				memory_info,
				input_buffer_.data(),
				input_buffer_.size(),
				input_shape.data(),
				input_shape.size());

			io_binding_->BindInput(input_name.c_str(), input_tensor);
			io_binding_->BindOutput(output_names_[0].c_str(), memory_info);

			session_->Run(Ort::RunOptions{nullptr}, *io_binding_);

			auto output_values = io_binding_->GetOutputValues();
			if (output_values.empty()) {
				throw std::runtime_error("IoBinding returned no output values");
			}

			auto& output_tensor = output_values[0];
			auto tensor_info = output_tensor.GetTensorTypeAndShapeInfo();
			size_t element_count = tensor_info.GetElementCount();
			const float* output_data = output_tensor.GetTensorData<float>();
			result.assign(output_data, output_data + element_count);

			char dbg_ib[192];
			snprintf(dbg_ib, sizeof(dbg_ib), "[WinMLUtils] Inference via IoBinding (elements=%zu)\n", element_count);
			OutputDebugStringA(dbg_ib);
		} else {
			// 従来パス: 毎回Ort::Valueを作成
			result = Evaluate(input_name, final_input_data.data(), final_input_data.size(), input_shape);
		}

		auto inference_end = std::chrono::high_resolution_clock::now();
		auto inference_ms = std::chrono::duration_cast<std::chrono::milliseconds>(inference_end - inference_start).count();
		snprintf(dbg, sizeof(dbg), "[WinMLUtils] Inference completed in %lld ms (GPU typically <50ms, CPU typically >100ms)\n", inference_ms);
		OutputDebugStringA(dbg);
		return result;
	}
	catch (const Ort::Exception& ex) {
		char dbg[512];
		snprintf(dbg, sizeof(dbg), "[WinMLUtils] Ort::Exception: %s\n", ex.what());
		OutputDebugStringA(dbg);
		std::cerr << "EvaluateFromTexture failed: " << ex.what() << std::endl;
		throw;
	}
}

std::vector<int64_t> WinMLUtils::GetInputShape(const std::string& input_name) const {
	if (!session_) return {};

	try {
		Ort::AllocatorWithDefaultOptions allocator;
		for (size_t i = 0; i < session_->GetInputCount(); ++i) {
			auto name_alloc = session_->GetInputNameAllocated(i, static_cast<OrtAllocator*>(allocator));
			if (std::string(name_alloc.get()) == input_name) {
				auto type_info = session_->GetInputTypeInfo(i);
				auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
				return tensor_info.GetShape();
			}
		}
	}
	catch (...) {}
	return {};
}

std::vector<int64_t> WinMLUtils::GetOutputShape(const std::string& output_name) const {
	if (!session_) return {};
	try {
		Ort::AllocatorWithDefaultOptions allocator;
		for (size_t i = 0; i < session_->GetOutputCount(); ++i) {
			auto name_alloc = session_->GetOutputNameAllocated(i, static_cast<OrtAllocator*>(allocator));
			if (std::string(name_alloc.get()) == output_name) {
				auto type_info = session_->GetOutputTypeInfo(i);
				auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
				return tensor_info.GetShape();
			}
		}
	}
	catch (...) {}
	return {};
}

Microsoft::WRL::ComPtr<IDXGISurface> WinMLUtils::TextureToDXGISurface(ID3D11Texture2D* texture) {
	if (!texture) return nullptr;

	Microsoft::WRL::ComPtr<IDXGIResource> dxgi_resource;
	HRESULT hr = texture->QueryInterface(IID_PPV_ARGS(&dxgi_resource));
	if (FAILED(hr) || !dxgi_resource) {
		std::cerr << "Failed to QueryInterface IDXGIResource." << std::endl;
		return nullptr;
	}

	Microsoft::WRL::ComPtr<IDXGISurface> dxgi_surface;
	hr = dxgi_resource->QueryInterface(IID_PPV_ARGS(&dxgi_surface));
	if (FAILED(hr) || !dxgi_surface) {
		std::cerr << "Failed to QueryInterface IDXGISurface." << std::endl;
		return nullptr;
	}

	return dxgi_surface;
}

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface WinMLUtils::DXGISurfaceToDirect3DSurface(
	IDXGISurface* dxgi_surface) {
	if (!dxgi_surface) return nullptr;

	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface direct3dsurface{ nullptr };
	winrt::check_hresult(dxgi_surface->QueryInterface(
		winrt::guid_of<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface>(),
		reinterpret_cast<void**>(winrt::put_abi(direct3dsurface))));
	return direct3dsurface;
}

winrt::Windows::Media::VideoFrame WinMLUtils::DXGISurfaceToVideoFrame(IDXGISurface* dxgi_surface) {
	if (!dxgi_surface) return nullptr;

	auto direct3dsurface = DXGISurfaceToDirect3DSurface(dxgi_surface);
	return winrt::Windows::Media::VideoFrame::CreateWithDirect3D11Surface(direct3dsurface);
}

bool WinMLUtils::InitializeIoBinding() {
	if (!session_ || io_binding_initialized_) {
		return io_binding_initialized_;
	}

	try {
		// モデルの入出力形状を取得
		if (input_names_.empty() || output_names_.empty()) {
			OutputDebugStringA("[WinMLUtils] IoBinding init failed: no input/output names\n");
			return false;
		}

		input_shape_ = GetInputShape(input_names_[0]);
		if (input_shape_.empty() || input_shape_.size() != 4) {
			// モデルから形状が取得できない場合はデフォルトを使用
			input_shape_ = { 1, 3, 640, 640 };
		}

		// 入力バッファを事前確保
		size_t input_elements = 1;
		for (auto dim : input_shape_) {
			if (dim > 0) input_elements *= static_cast<size_t>(dim);
		}
		input_buffer_.resize(input_elements, 0.0f);

		// IoBindingを作成
		io_binding_ = std::make_unique<Ort::IoBinding>(*session_);

		io_binding_initialized_ = true;
		char dbg[256];
		snprintf(dbg, sizeof(dbg), "[WinMLUtils] IoBinding initialized: input=[%lld,%lld,%lld,%lld]\n",
			input_shape_[0], input_shape_[1], input_shape_[2], input_shape_[3]);
		OutputDebugStringA(dbg);
		return true;
	}
	catch (const Ort::Exception& ex) {
		char dbg[512];
		snprintf(dbg, sizeof(dbg), "[WinMLUtils] IoBinding init failed: %s\n", ex.what());
		OutputDebugStringA(dbg);
		io_binding_.reset();
		input_buffer_.clear();
		return false;
	}
}

void WinMLUtils::ReleaseIoBinding() {
	if (io_binding_) {
		io_binding_->ClearBoundInputs();
		io_binding_->ClearBoundOutputs();
		io_binding_.reset();
	}
	input_buffer_.clear();
	input_shape_.clear();
	io_binding_initialized_ = false;
}

} // namespace WoLNamesBlackedOut::Core
