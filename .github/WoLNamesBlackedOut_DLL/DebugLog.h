#pragma once

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <string>

inline bool WOL_IsDebugLogExportEnabled()
{
	char value[16] = {};
	DWORD len = GetEnvironmentVariableA("WOL_DEBUG_LOG_EXPORT", value, static_cast<DWORD>(sizeof(value)));
	if (len == 0 || len >= sizeof(value))
	{
		return false;
	}

	std::string text(value, len);
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text == "1" || text == "true";
}

inline std::string WOL_GetOrCreateDebugLogPath()
{
	static std::string debugLogFilePath;
	if (!debugLogFilePath.empty())
	{
		return debugLogFilePath;
	}

	char sharedPath[MAX_PATH * 4] = {};
	DWORD sharedLen = GetEnvironmentVariableA("WOL_DEBUG_LOG_FILE_PATH", sharedPath, static_cast<DWORD>(sizeof(sharedPath)));
	if (sharedLen > 0 && sharedLen < sizeof(sharedPath))
	{
		debugLogFilePath = std::string(sharedPath, sharedLen);
		return debugLogFilePath;
	}

	char tempPath[MAX_PATH] = {};
	DWORD pathLen = GetTempPathA(static_cast<DWORD>(sizeof(tempPath)), tempPath);
	if (pathLen == 0 || pathLen >= sizeof(tempPath))
	{
		return {};
	}

	SYSTEMTIME st{};
	GetLocalTime(&st);

	char fileName[128] = {};
	sprintf_s(fileName,
		"wol_DebugLog_%04u%02u%02u_%02u%02u%02u.txt",
		static_cast<unsigned>(st.wYear),
		static_cast<unsigned>(st.wMonth),
		static_cast<unsigned>(st.wDay),
		static_cast<unsigned>(st.wHour),
		static_cast<unsigned>(st.wMinute),
		static_cast<unsigned>(st.wSecond));

	debugLogFilePath = std::string(tempPath) + fileName;
	return debugLogFilePath;
}

inline void WOL_OutputDebugStringA(const char* msg)
{
	if (!msg || !WOL_IsDebugLogExportEnabled())
	{
		return;
	}

	static std::mutex debugLogMutex;
	std::lock_guard<std::mutex> lock(debugLogMutex);

	std::string path = WOL_GetOrCreateDebugLogPath();
	if (path.empty())
	{
		return;
	}

	std::ofstream f(path, std::ios::app | std::ios::binary);
	if (f.is_open())
	{
		f << msg;
	}
}

inline void WOL_OutputDebugStringW(const wchar_t* msg)
{
	if (!msg || !WOL_IsDebugLogExportEnabled())
	{
		return;
	}

	int required = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
	if (required <= 1)
	{
		return;
	}

	std::string utf8(static_cast<size_t>(required - 1), '\0');
	int converted = WideCharToMultiByte(CP_UTF8, 0, msg, -1, utf8.data(), required, nullptr, nullptr);
	if (converted <= 1)
	{
		return;
	}

	WOL_OutputDebugStringA(utf8.c_str());
}

#ifdef OutputDebugStringA
#undef OutputDebugStringA
#endif
#define OutputDebugStringA WOL_OutputDebugStringA

#ifdef OutputDebugStringW
#undef OutputDebugStringW
#endif
#define OutputDebugStringW WOL_OutputDebugStringW
