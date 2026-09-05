#pragma once

#include <winrt/Windows.Storage.h>
#include <winml/onnxruntime_cxx_api.h>

#include <algorithm>
#include <bcrypt.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace WoLNamesBlackedOut::Core::ModelCompilationCache {

struct CacheDecision {
	std::wstring source_model_path;
	std::string ep_name;
	std::wstring cache_directory;
	std::wstring compiled_model_path;
	std::wstring metadata_path;
	std::string source_model_sha256;
	bool has_valid_compiled_model = false;
	std::string reason;
};

inline std::string WideToUtf8(const std::wstring& value)
{
	if (value.empty()) {
		return {};
	}

	int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1) {
		return {};
	}

	std::string utf8(static_cast<size_t>(len) - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), len, nullptr, nullptr);
	return utf8;
}

inline std::wstring Utf8ToWide(const std::string& value)
{
	if (value.empty()) {
		return {};
	}

	int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
	if (len <= 1) {
		return {};
	}

	std::wstring wide(static_cast<size_t>(len) - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), len);
	return wide;
}

inline std::wstring GetFallbackBaseDirectory()
{
	wchar_t module_path[MAX_PATH] = {};
	if (GetModuleFileNameW(nullptr, module_path, MAX_PATH) == 0) {
		return L".";
	}

	std::wstring path(module_path);
	auto pos = path.find_last_of(L"\\/");
	if (pos == std::wstring::npos) {
		return L".";
	}

	return path.substr(0, pos);
}

inline std::wstring GetLocalStateBaseDirectory()
{
	try {
		auto local_folder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
		std::wstring local_path = local_folder.Path().c_str();
		if (!local_path.empty()) {
			return local_path;
		}
	} catch (...) {
	}

	return GetFallbackBaseDirectory();
}

inline std::wstring SanitizeFileName(const std::wstring& value)
{
	if (value.empty()) {
		return L"model";
	}

	std::wstring result;
	result.reserve(value.size());
	for (wchar_t ch : value) {
		if ((ch >= L'0' && ch <= L'9') ||
			(ch >= L'a' && ch <= L'z') ||
			(ch >= L'A' && ch <= L'Z') ||
			ch == L'_' || ch == L'-' || ch == L'.') {
			result.push_back(ch);
		} else {
			result.push_back(L'_');
		}
	}

	while (!result.empty() && (result.back() == L'.' || result.back() == L' ')) {
		result.pop_back();
	}

	if (result.empty()) {
		return L"model";
	}

	return result;
}

inline std::wstring JoinPath(const std::wstring& lhs, const std::wstring& rhs)
{
	if (lhs.empty()) {
		return rhs;
	}
	if (rhs.empty()) {
		return lhs;
	}

	if (lhs.back() == L'\\' || lhs.back() == L'/') {
		return lhs + rhs;
	}

	return lhs + L"\\" + rhs;
}

inline bool ComputeFileSha256(const std::wstring& path, std::string& out_hex)
{
	std::ifstream file(std::filesystem::path(path), std::ios::binary);
	if (!file) {
		return false;
	}

	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	PUCHAR hash_object = nullptr;
	PUCHAR hash_bytes = nullptr;
	DWORD hash_object_size = 0;
	DWORD hash_size = 0;
	DWORD result_size = 0;
	NTSTATUS status = 0;

	bool success = false;
	std::vector<unsigned char> buffer(1024 * 64);

	do {
		status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
		if (status < 0) break;

		status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hash_object_size), sizeof(hash_object_size), &result_size, 0);
		if (status < 0) break;

		status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &result_size, 0);
		if (status < 0) break;

		hash_object = static_cast<PUCHAR>(HeapAlloc(GetProcessHeap(), 0, hash_object_size));
		hash_bytes = static_cast<PUCHAR>(HeapAlloc(GetProcessHeap(), 0, hash_size));
		if (!hash_object || !hash_bytes) break;

		status = BCryptCreateHash(algorithm, &hash, hash_object, hash_object_size, nullptr, 0, 0);
		if (status < 0) break;

		while (file.good()) {
			file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize read_size = file.gcount();
			if (read_size <= 0) {
				break;
			}

			status = BCryptHashData(hash, buffer.data(), static_cast<ULONG>(read_size), 0);
			if (status < 0) break;
		}

		if (status < 0) break;
		status = BCryptFinishHash(hash, hash_bytes, hash_size, 0);
		if (status < 0) break;

		static constexpr char kHex[] = "0123456789abcdef";
		out_hex.clear();
		out_hex.reserve(hash_size * 2);
		for (DWORD i = 0; i < hash_size; ++i) {
			const unsigned char b = hash_bytes[i];
			out_hex.push_back(kHex[(b >> 4) & 0xF]);
			out_hex.push_back(kHex[b & 0xF]);
		}

		success = true;
	} while (false);

	if (hash) {
		BCryptDestroyHash(hash);
	}
	if (hash_object) {
		HeapFree(GetProcessHeap(), 0, hash_object);
	}
	if (hash_bytes) {
		HeapFree(GetProcessHeap(), 0, hash_bytes);
	}
	if (algorithm) {
		BCryptCloseAlgorithmProvider(algorithm, 0);
	}

	return success;
}

inline bool ReadMetadataSourceHash(const std::wstring& metadata_path, std::string& out_hash)
{
	std::ifstream input(std::filesystem::path(metadata_path), std::ios::binary);
	if (!input) {
		return false;
	}

	std::string line;
	while (std::getline(input, line)) {
		constexpr char key[] = "source_sha256=";
		if (line.rfind(key, 0) == 0) {
			out_hash = line.substr(sizeof(key) - 1);
			return !out_hash.empty();
		}
	}

	return false;
}

inline bool SupportsPersistentCompileCache(const std::string& ep_name)
{
	return ep_name == "NvTensorRTRTXExecutionProvider" ||
		ep_name == "OpenVINOExecutionProvider" ||
		ep_name == "MIGraphXExecutionProvider" ||
		ep_name == "VitisAIExecutionProvider";
}

inline std::string NormalizeEpCacheFolderName(const std::string& ep_name)
{
	if (ep_name == "NvTensorRTRTXExecutionProvider") {
		return "trt_rtx";
	}
	if (ep_name == "OpenVINOExecutionProvider") {
		return "openvino";
	}
	if (ep_name == "MIGraphXExecutionProvider") {
		return "migraphx";
	}
	if (ep_name == "VitisAIExecutionProvider") {
		return "vitisai";
	}

	std::string normalized = ep_name;
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
			return static_cast<char>(c);
		}
		if (c >= 'A' && c <= 'Z') {
			return static_cast<char>(c - 'A' + 'a');
		}
		return '_';
	});

	if (normalized.empty()) {
		return "unknown_ep";
	}

	return normalized;
}

inline bool PrepareCacheDecision(
	const wchar_t* source_model_path,
	const std::string& ep_name,
	CacheDecision& out_decision,
	std::string& out_error)
{
	out_decision = {};
	out_error.clear();

	if (!source_model_path || source_model_path[0] == L'\0') {
		out_error = "source model path is empty";
		return false;
	}

	out_decision.source_model_path = source_model_path;
	out_decision.ep_name = ep_name;

	if (!SupportsPersistentCompileCache(ep_name)) {
		out_decision.reason = "ep-not-supported";
		return true;
	}

	const std::wstring local_base = GetLocalStateBaseDirectory();
	const std::string ep_folder = NormalizeEpCacheFolderName(ep_name) + "_cache";
	out_decision.cache_directory = JoinPath(local_base, Utf8ToWide(ep_folder));

	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(out_decision.cache_directory), ec);
	if (ec) {
		out_error = "failed to create cache directory";
		return false;
	}

	const std::filesystem::path source_path(source_model_path);
	const std::wstring model_stem = SanitizeFileName(source_path.stem().wstring());
	out_decision.compiled_model_path = JoinPath(out_decision.cache_directory, model_stem + L"_ctx.onnx");
	out_decision.metadata_path = out_decision.compiled_model_path + L".meta";

	if (!ComputeFileSha256(source_model_path, out_decision.source_model_sha256)) {
		out_error = "failed to compute source model sha256";
		return false;
	}

	const bool compiled_exists = std::filesystem::exists(std::filesystem::path(out_decision.compiled_model_path));
	const bool metadata_exists = std::filesystem::exists(std::filesystem::path(out_decision.metadata_path));
	if (!compiled_exists) {
		out_decision.reason = "cache-miss:no-compiled-model";
		return true;
	}

	if (!metadata_exists) {
		out_decision.reason = "cache-miss:no-metadata";
		return true;
	}

	std::string cached_hash;
	if (!ReadMetadataSourceHash(out_decision.metadata_path, cached_hash)) {
		out_decision.reason = "cache-miss:invalid-metadata";
		return true;
	}

	if (cached_hash != out_decision.source_model_sha256) {
		out_decision.reason = "cache-miss:hash-mismatch";
		return true;
	}

	out_decision.has_valid_compiled_model = true;
	out_decision.reason = "cache-hit";
	return true;
}

inline bool WriteCacheMetadata(const CacheDecision& decision, std::string& out_error)
{
	out_error.clear();
	if (decision.metadata_path.empty()) {
		out_error = "metadata path is empty";
		return false;
	}

	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(decision.cache_directory), ec);
	if (ec) {
		out_error = "failed to create metadata directory";
		return false;
	}

	std::ofstream output(std::filesystem::path(decision.metadata_path), std::ios::binary | std::ios::trunc);
	if (!output) {
		out_error = "failed to open metadata file";
		return false;
	}

	auto now = std::chrono::system_clock::now();
	auto now_tt = std::chrono::system_clock::to_time_t(now);
	std::tm now_tm{};
	gmtime_s(&now_tm, &now_tt);
	char timestamp[64] = {};
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &now_tm);

	output << "source_sha256=" << decision.source_model_sha256 << "\n";
	output << "source_model=" << WideToUtf8(decision.source_model_path) << "\n";
	output << "ep_name=" << decision.ep_name << "\n";
	output << "compiled_model=" << WideToUtf8(decision.compiled_model_path) << "\n";
	output << "compiled_utc=" << timestamp << "\n";
	output.flush();

	if (!output.good()) {
		out_error = "failed to write metadata";
		return false;
	}

	return true;
}

inline bool RemoveCacheArtifacts(const CacheDecision& decision, std::string& out_error)
{
	out_error.clear();
	std::error_code ec;

	if (!decision.compiled_model_path.empty()) {
		std::filesystem::remove(std::filesystem::path(decision.compiled_model_path), ec);
		if (ec) {
			out_error = "failed to remove compiled model";
			return false;
		}
	}

	if (!decision.metadata_path.empty()) {
		std::filesystem::remove(std::filesystem::path(decision.metadata_path), ec);
		if (ec) {
			out_error = "failed to remove metadata";
			return false;
		}
	}

	return true;
}

inline bool CompileModelToCache(
	const Ort::Env& env,
	const Ort::SessionOptions& session_options,
	CacheDecision& decision,
	std::string& out_error)
{
	out_error.clear();
	if (decision.source_model_path.empty() || decision.compiled_model_path.empty()) {
		out_error = "invalid compile cache decision";
		return false;
	}

	const OrtApi& api = Ort::GetApi();
	const OrtCompileApi* compile_api = api.GetCompileApi();
	if (!compile_api) {
		out_error = "compile api unavailable";
		return false;
	}

	std::string cleanup_error;
	if (!RemoveCacheArtifacts(decision, cleanup_error)) {
		out_error = cleanup_error;
		return false;
	}

	OrtModelCompilationOptions* compile_options = nullptr;
	OrtStatus* status = compile_api->CreateModelCompilationOptionsFromSessionOptions(env, session_options, &compile_options);
	if (status != nullptr) {
		out_error = api.GetErrorMessage(status);
		api.ReleaseStatus(status);
		return false;
	}

	auto release_compile_options = [&]() {
		if (compile_options != nullptr) {
			compile_api->ReleaseModelCompilationOptions(compile_options);
			compile_options = nullptr;
		}
	};

	status = compile_api->ModelCompilationOptions_SetInputModelPath(compile_options, decision.source_model_path.c_str());
	if (status != nullptr) {
		out_error = api.GetErrorMessage(status);
		api.ReleaseStatus(status);
		release_compile_options();
		return false;
	}

	status = compile_api->ModelCompilationOptions_SetOutputModelPath(compile_options, decision.compiled_model_path.c_str());
	if (status != nullptr) {
		out_error = api.GetErrorMessage(status);
		api.ReleaseStatus(status);
		release_compile_options();
		return false;
	}

	status = compile_api->CompileModel(env, compile_options);
	if (status != nullptr) {
		out_error = api.GetErrorMessage(status);
		api.ReleaseStatus(status);
		release_compile_options();
		return false;
	}

	release_compile_options();

	if (!std::filesystem::exists(std::filesystem::path(decision.compiled_model_path))) {
		out_error = "compile output was not generated";
		return false;
	}

	if (!WriteCacheMetadata(decision, out_error)) {
		return false;
	}

	decision.has_valid_compiled_model = true;
	decision.reason = "compiled";
	return true;
}

} // namespace WoLNamesBlackedOut::Core::ModelCompilationCache
