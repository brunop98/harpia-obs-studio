#pragma once

#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vector>
#endif

namespace harpia {

struct GraphicsModuleDiagnostic {
	std::string modulePath;
	unsigned long windowsError = 0;
	std::string systemMessage;
	bool loaded = false;
	bool loadAttempted = true;
};

// Keep the Windows error visible even if FormatMessage cannot describe it.
inline std::string describeGraphicsModuleDiagnostic(const GraphicsModuleDiagnostic &result)
{
	const std::string module = result.modulePath.empty() ? "<unspecified module>" : result.modulePath;
	if (result.loaded)
		return "Graphics module diagnostic: '" + module +
		       "' loaded during a diagnostic retry. The original load failure may have been transient. "
		       "Restart Harpia and retain the full startup log if it recurs.";
	std::string text = "Graphics module diagnostic: '" + module +
		(result.loadAttempted ? "' could not be loaded" : "' could not be checked") +
		" (Windows error " + std::to_string(result.windowsError) + ").";
	if (!result.systemMessage.empty())
		text += " " + result.systemMessage;
	if (!result.loadAttempted)
		return text + " The diagnostic could not resolve a valid module path beside harpia.exe.";
	switch (result.windowsError) {
	case 126:
		return text + " The DLL or one of its dependencies is missing. Replace the complete Harpia "
			      "package and install or repair the Microsoft Visual C++ x64 Redistributable.";
	case 127:
		return text + " A required entry point is missing from this DLL or a dependency. Replace the "
			      "complete package with matching DLL versions; do not mix builds.";
	case 193:
		return text + " A DLL has the wrong architecture or is damaged. Use the complete Windows x64 package.";
	case 1114:
		return text + " A DLL initialization routine failed. Include this error and the preceding startup "
			      "log when reporting the problem; check the graphics driver too.";
	case 5:
		return text + " Access was denied. Check the file permissions and Windows Security protection history.";
	default:
		return text + " Include this path, error number and the preceding startup log when reporting the problem.";
	}
}

#if defined(_WIN32)
namespace graphics_module_diagnostics {
inline std::string utf8(const std::wstring &value)
{
	if (value.empty())
		return {};
	const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
	if (size <= 0)
		return {};
	std::string result(size, '\0');
	if (!WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), result.data(), size, nullptr, nullptr))
		return {};
	return result;
}

inline std::string errorMessage(DWORD error)
{
	if (error == 0)
		return {};
	wchar_t *message = nullptr;
	const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
					   FORMAT_MESSAGE_IGNORE_INSERTS,
					   nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
	std::string result = length && message ? utf8(std::wstring(message, length)) : std::string();
	if (message)
		LocalFree(message);
	while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' '))
		result.pop_back();
	return result;
}
} // namespace graphics_module_diagnostics

// Run only after OBS returns OBS_VIDEO_MODULE_NOT_FOUND. This diagnoses loading
// the bundled module, not creating a D3D device, and balances a successful load.
inline std::string inspectWindowsGraphicsModule(const char *module)
{
	GraphicsModuleDiagnostic result;
	result.modulePath = module && *module ? module : "<unspecified module>";
	result.loadAttempted = false;
	const auto failed = [&result](DWORD error) {
		result.windowsError = error;
		result.systemMessage = graphics_module_diagnostics::errorMessage(error);
		return describeGraphicsModuleDiagnostic(result);
	};
	if (!module || !*module)
		return failed(ERROR_INVALID_PARAMETER);
	const int nameSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, module, -1, nullptr, 0);
	if (nameSize <= 0)
		return failed(GetLastError());
	if (nameSize > 32768)
		return failed(ERROR_FILENAME_EXCED_RANGE);
	std::vector<wchar_t> nameBuffer(nameSize);
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, module, -1, nameBuffer.data(), nameSize))
		return failed(GetLastError());
	const std::wstring name(nameBuffer.data(), nameSize - 1);
	if (name.find_first_of(L"\\/:") != std::wstring::npos || name == L"." || name == L"..")
		return failed(ERROR_INVALID_PARAMETER); // only inspect a module beside this executable
	std::vector<wchar_t> executable(1024);
	DWORD length = 0;
	for (;;) {
		length = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
		if (!length)
			return failed(GetLastError());
		if (length < executable.size())
			break;
		if (executable.size() >= 32768)
			return failed(ERROR_FILENAME_EXCED_RANGE);
		executable.resize(executable.size() * 2);
	}
	std::wstring path(executable.data(), length);
	const auto slash = path.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
		return failed(ERROR_BAD_PATHNAME);
	path.resize(slash + 1);
	path += name;
	result.modulePath = graphics_module_diagnostics::utf8(path);
	if (path.size() >= 32768)
		return failed(ERROR_FILENAME_EXCED_RANGE);
	result.loadAttempted = true;
	HMODULE loaded = LoadLibraryExW(path.c_str(), nullptr,
				      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!loaded) {
		const DWORD error = GetLastError(); // capture before formatting or calling another Windows API
		return failed(error);
	}
	result.loaded = true;
	FreeLibrary(loaded);
	return describeGraphicsModuleDiagnostic(result);
}
#endif

} // namespace harpia
