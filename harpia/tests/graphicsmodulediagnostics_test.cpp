#include "core/GraphicsModuleDiagnostics.hpp"

#include <cstdio>
#include <string>
#include <utility>

using namespace harpia;
namespace {
int failures = 0;
bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}
}

int main()
{
	GraphicsModuleDiagnostic result;
	result.modulePath = "C:\\Harpia\\bin\\64bit\\libobs-d3d11.dll";
	result.windowsError = 126;
	result.systemMessage = "The specified module could not be found.";
	std::string text = describeGraphicsModuleDiagnostic(result);
	check(has(text, result.modulePath) && has(text, "Windows error 126") && has(text, result.systemMessage),
	      "diagnostic includes the exact path, numeric loader error and Windows detail");
	check(has(text, "one of its dependencies") && has(text, "Visual C++ x64"),
	      "a missing dependency does not falsely claim that the renderer file itself is absent");
	for (const auto &error : {std::pair<unsigned long, const char *>(127, "entry point"),
				 {193, "architecture"}, {1114, "initialization"}, {5, "Access"}}) {
		result.windowsError = error.first;
		text = describeGraphicsModuleDiagnostic(result);
		check(has(text, "Windows error " + std::to_string(error.first)) && has(text, error.second),
		      "known Windows loader errors retain their number and specific recovery advice");
	}
	result.windowsError = 9999;
	result.systemMessage.clear();
	text = describeGraphicsModuleDiagnostic(result);
	check(has(text, "Windows error 9999") && has(text, "startup log"),
	      "unknown errors stay useful without a system-message translation");
	result.loaded = true;
	text = describeGraphicsModuleDiagnostic(result);
	check(has(text, "loaded during a diagnostic retry") && has(text, "may have been transient") &&
	      !has(text, "could not be loaded"), "a successful retry does not misrepresent the original engine failure");
	result.loaded = false;
	result.loadAttempted = false;
	text = describeGraphicsModuleDiagnostic(result);
	check(has(text, "could not be checked") && !has(text, "could not be loaded"),
	      "path-resolution failure is distinguished from an actual DLL-load failure");
	result.modulePath.clear();
	check(has(describeGraphicsModuleDiagnostic(result), "<unspecified module>"),
	      "missing module names remain readable");

#if defined(HARPIA_TEST_FAKE_WINDOWS)
	using namespace fake_graphics_windows;
	reset();
	text = inspectWindowsGraphicsModule("libobs-d3d11.dll");
	check(state.loadedPath == L"C:\\Harpia\\bin\\64bit\\libobs-d3d11.dll" &&
	      has(text, "Windows error 126"), "native probe loads the renderer beside the executable, independently of CWD");
	check(state.flags == (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS),
	      "native probe searches the module's own folder and standard safe dependency directories");
	check(state.lastError == 5 && has(text, "Windows error 126") && has(text, "Mock Windows detail."),
	      "loader error is captured before FormatMessage changes thread last-error state");
	check(state.frees == 0 && text.find('\n') == std::string::npos,
	      "failed loads are never freed and system-message newlines are trimmed");
	reset();
	state.loadError = 193;
	state.formatSuccess = false;
	text = inspectWindowsGraphicsModule("libobs-d3d11.dll");
	check(has(text, "Windows error 193") && has(text, "architecture"),
	      "failure to format a Windows message never discards the original loader code");
	reset();
	state.executable = L"C:\\Usuários\\影片\\harpia.exe";
	text = inspectWindowsGraphicsModule(u8"渲染.dll");
	check(state.loadedPath == L"C:\\Usuários\\影片\\渲染.dll" &&
	      has(text, u8"C:\\Usuários\\影片\\渲染.dll"), "Unicode executable directories and module names remain readable as UTF-8");
	reset();
	state.executable = L"C:\\";
	for (int i = 0; i < 100; ++i)
		state.executable += L"long-directory-name\\";
	state.executable += L"harpia.exe";
	text = inspectWindowsGraphicsModule("libobs-d3d11.dll");
	check(state.filenameCalls > 1 && state.loadedPath.size() > 1024 && has(text, "libobs-d3d11.dll"),
	      "executable path buffers grow instead of truncating long installation paths");
	reset();
	state.loadSuccess = true;
	text = inspectWindowsGraphicsModule("libobs-d3d11.dll");
	check(state.loadCalls == 1 && state.frees == 1 && has(text, "loaded during a diagnostic retry"),
	      "a successful diagnostic load releases its own module reference exactly once");
	reset();
	state.filenameError = 5;
	text = inspectWindowsGraphicsModule("libobs-d3d11.dll");
	check(state.loadCalls == 0 && has(text, "could not be checked") && has(text, "Windows error 5"),
	      "unresolved executable paths do not trigger a relative DLL load");
	reset();
	text = inspectWindowsGraphicsModule(nullptr);
	check(state.loadCalls == 0 && has(text, "Windows error 87"), "null module names fail safely before loading");
	reset();
	text = inspectWindowsGraphicsModule("..\\unrelated.dll");
	check(state.loadCalls == 0 && has(text, "could not be checked"), "the diagnostic cannot escape the executable's DLL directory");
	reset();
	state.executable.assign(32768, L'x');
	text = inspectWindowsGraphicsModule("libobs-d3d11.dll");
	check(state.loadCalls == 0 && has(text, "Windows error 206"), "overlong executable paths produce a bounded diagnostic failure");
#endif
	std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
	return failures ? 1 : 0;
}
