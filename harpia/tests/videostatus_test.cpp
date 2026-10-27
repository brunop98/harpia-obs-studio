// A loader failure must not invent a build race, and a driver refusal must
// retain its real cause in the recorder's readiness report.
#include "core/VideoStatus.hpp"

#include <cstdio>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}
bool contains(const std::string &text, const char *part)
{
	return text.find(part) != std::string::npos;
}
}

int main()
{
	using namespace harpia;
	const auto dll = describeVideoResetError(-5, 1280, 720, "libobs-d3d11.dll");
	check(contains(dll, "libobs-d3d11.dll") && contains(dll, "dependencies"),
	      "a module-load failure identifies the renderer and its dependency possibility");
	check(contains(dll, "Visual C++ x64") && contains(dll, "fresh folder"),
	      "Windows recovery recommends the full package and the correct runtime architecture");
	check(contains(dll, "LoadLibrary failed") && contains(dll, "Info") && contains(dll, "Windows error"),
	      "the report points to the loader entry containing the actual Windows error");
	check(!contains(dll, "while a build") && !contains(dll, "still copying"),
	      "the report does not assume an unproven build-copy race");
	const auto opengl = describeVideoResetError(-5, 1280, 720, "libobs-opengl.so.30");
	check(contains(opengl, "libobs-opengl.so.30") && !contains(opengl, "d3d11") &&
	      !contains(opengl, "Visual C++"), "other platforms name their actual renderer and library-loader recovery");
	check(contains(describeVideoResetError(-5, 1280, 720, ""), "no renderer was configured"),
	      "an unconfigured build has its own explanation");
	lastVideoResetError() = describeVideoResetError(-2, 1280, 720);
	const auto driver = describeVideoNotReady();
	check(contains(driver, "driver") && !contains(driver, "did not load") && !contains(driver, "d3d11"),
	      "readiness preserves a graphics-driver refusal instead of mislabeling it as a missing DLL");
	lastVideoResetError() = dll;
	check(contains(describeVideoNotReady(), "LoadLibrary failed"),
	      "readiness retains the precise loader troubleshooting guidance");
	lastVideoResetError().clear();
	check(contains(describeVideoNotReady(), "has not initialized") && !contains(describeVideoNotReady(), "did not load"),
	      "an engine that has not initialized is not labeled as a failed DLL load");
	check(contains(describeVideoResetError(-3, 1917, 881), "1917x881") &&
	      contains(describeVideoResetError(-4, 1280, 720), "still using"),
	      "invalid dimensions and an active capture retain their distinct explanations");
	check(contains(describeVideoResetError(-123, 1280, 720), "error -123"),
	      "an unknown error keeps its original numeric code");
	return failures ? 1 : 0;
}
