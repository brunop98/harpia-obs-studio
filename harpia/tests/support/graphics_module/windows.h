#pragma once

// Only for the diagnostic probe's cross-platform control-flow test. This does
// not emulate DLL loading or replace a native Windows integration test.
#include <algorithm>
#include <codecvt>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <locale>
#include <string>

using DWORD = unsigned long;
using HMODULE = void *;
using LPWSTR = wchar_t *;
constexpr DWORD CP_UTF8 = 65001;
constexpr DWORD MB_ERR_INVALID_CHARS = 8;
constexpr DWORD FORMAT_MESSAGE_ALLOCATE_BUFFER = 0x100;
constexpr DWORD FORMAT_MESSAGE_FROM_SYSTEM = 0x1000;
constexpr DWORD FORMAT_MESSAGE_IGNORE_INSERTS = 0x200;
constexpr DWORD LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x100;
constexpr DWORD LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x1000;
constexpr DWORD ERROR_INVALID_PARAMETER = 87;
constexpr DWORD ERROR_FILENAME_EXCED_RANGE = 206;
constexpr DWORD ERROR_BAD_PATHNAME = 161;

namespace fake_graphics_windows {
struct State {
	std::wstring executable = L"C:\\Harpia\\bin\\64bit\\harpia.exe";
	std::wstring loadedPath;
	DWORD flags = 0, lastError = 0, filenameError = 0, loadError = 126;
	int filenameCalls = 0, loadCalls = 0, frees = 0;
	bool loadSuccess = false, formatSuccess = true;
};
inline State state;
inline void reset() { state = State(); }
}

inline DWORD GetLastError() { return fake_graphics_windows::state.lastError; }
inline int MultiByteToWideChar(DWORD, DWORD, const char *value, int length, wchar_t *out, int capacity)
{
	std::wstring text;
	try {
		text = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(
			value, value + (length < 0 ? std::strlen(value) : size_t(length)));
	} catch (...) {
		fake_graphics_windows::state.lastError = 1113;
		return 0;
	}
	const int count = int(text.size()) + (length < 0 ? 1 : 0);
	if (!out)
		return count;
	if (capacity < count)
		return 0;
	std::copy(text.c_str(), text.c_str() + count, out);
	return count;
}
inline int WideCharToMultiByte(DWORD, DWORD, const wchar_t *value, int length, char *out, int capacity,
			       const char *, bool *)
{
	const auto text = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(value, value + length);
	if (!out)
		return int(text.size());
	if (capacity < int(text.size()))
		return 0;
	std::copy(text.begin(), text.end(), out);
	return int(text.size());
}
inline DWORD GetModuleFileNameW(HMODULE, wchar_t *out, DWORD capacity)
{
	auto &state = fake_graphics_windows::state;
	++state.filenameCalls;
	if (state.filenameError) {
		state.lastError = state.filenameError;
		return 0;
	}
	const auto count = std::min<size_t>(capacity, state.executable.size());
	std::copy_n(state.executable.data(), count, out);
	if (count < capacity)
		out[count] = L'\0';
	return DWORD(count);
}
inline HMODULE LoadLibraryExW(const wchar_t *path, void *, DWORD flags)
{
	auto &state = fake_graphics_windows::state;
	state.loadedPath = path;
	state.flags = flags;
	++state.loadCalls;
	state.lastError = state.loadError;
	return state.loadSuccess ? reinterpret_cast<HMODULE>(1) : nullptr;
}
inline bool FreeLibrary(HMODULE)
{
	++fake_graphics_windows::state.frees;
	return true;
}
inline DWORD FormatMessageW(DWORD, const void *, DWORD, DWORD, LPWSTR out, DWORD, void *)
{
	// Mutating lastError proves that the probe captures the loader error first.
	fake_graphics_windows::state.lastError = 5;
	if (!fake_graphics_windows::state.formatSuccess)
		return 0;
	const wchar_t *text = L"Mock Windows detail.\r\n";
	const auto count = std::wcslen(text);
	auto *copy = static_cast<wchar_t *>(std::malloc((count + 1) * sizeof(wchar_t)));
	std::copy_n(text, count + 1, copy);
	*reinterpret_cast<wchar_t **>(out) = copy;
	return DWORD(count);
}
inline void *LocalFree(void *memory) { std::free(memory); return nullptr; }
