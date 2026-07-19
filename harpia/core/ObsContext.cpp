#include "ObsContext.hpp"

#include <obs.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// Graphics module file names injected by CMake (see harpia/CMakeLists.txt),
// mirroring how the OBS frontend resolves its renderer.
#ifndef DL_D3D11
#define DL_D3D11 ""
#endif
#ifndef DL_OPENGL
#define DL_OPENGL ""
#endif

namespace harpia {

ObsContext::~ObsContext()
{
	shutdown();
}

const char *ObsContext::renderModule()
{
#if defined(_WIN32)
	return DL_D3D11;
#else
	return DL_OPENGL;
#endif
}

bool ObsContext::startup()
{
	if (initialized_)
		return true;

	// libobs writes per-module config under this directory.
	char configPath[512];
	if (os_get_config_path(configPath, sizeof(configPath), "harpia-recorder/plugin_config") <= 0)
		return false;
	os_mkdirs(configPath);

	if (!obs_startup("en-US", configPath, nullptr)) {
		blog(LOG_ERROR, "[harpia] obs_startup failed");
		return false;
	}

	initialized_ = true;
	return true;
}

void ObsContext::addModulePaths()
{
	// Environment overrides let a dev point at a build tree without installing.
	const char *pluginsPath = std::getenv("OBS_PLUGINS_PATH");
	const char *pluginsDataPath = std::getenv("OBS_PLUGINS_DATA_PATH");
	if (pluginsPath && pluginsDataPath) {
		std::string dataWithModule = std::string(pluginsDataPath) + "/%module%";
		obs_add_module_path(pluginsPath, dataWithModule.c_str());
	}

#if defined(__APPLE__)
	// macOS bundle layout — a single, unambiguous path.
	obs_add_module_path("../PlugIns", "../PlugIns/%module%.plugin/Contents/Resources");
#else
	// Windows/Linux: we support two install layouts (running from the OBS
	// rundir with the exe under bin/64bit, or with plugins beside the exe).
	// Both candidate paths can resolve to the SAME physical folder depending on
	// the working directory; registering that folder twice makes libobs load
	// every module twice ("Source 'x' already exists! Duplicate library?"). So
	// resolve each candidate to an absolute path against the executable's own
	// directory and register each distinct folder only once.
	namespace fs = std::filesystem;
	std::error_code ec;

	// Anchor to the executable directory (not the CWD), so paths are stable
	// regardless of where the app was launched from.
	char *marker = os_get_executable_path_ptr("marker");
	fs::path exeDir = (marker && *marker) ? fs::path(marker).parent_path() : fs::current_path(ec);
	bfree(marker);

	struct Candidate {
		const char *bin;
		const char *dataDir;
	};
	const Candidate candidates[] = {
		{"../../obs-plugins/64bit", "../../data/obs-plugins"},
		{"obs-plugins/64bit", "data/obs-plugins"},
	};

	std::vector<std::string> registered;
	for (const Candidate &c : candidates) {
		const fs::path bin = fs::weakly_canonical(exeDir / c.bin, ec);
		if (ec || !fs::exists(bin, ec))
			continue;
		const std::string binStr = bin.string();
		if (std::find(registered.begin(), registered.end(), binStr) != registered.end())
			continue; // same folder already registered — skip the duplicate load
		registered.push_back(binStr);

		const fs::path dataDir = fs::weakly_canonical(exeDir / c.dataDir, ec);
		const std::string dataPattern = dataDir.string() + "/%module%";
		obs_add_module_path(binStr.c_str(), dataPattern.c_str());
	}

	// Fallback for any unusual layout the candidates didn't match: keep the
	// original relative registration (single entry, so still no duplication).
	if (registered.empty())
		obs_add_module_path("obs-plugins/64bit", "data/obs-plugins/%module%");
#endif
}

void ObsContext::loadModules()
{
	if (modulesLoaded_)
		return;

	addModulePaths();

	struct obs_module_failure_info mfi;
	obs_load_all_modules2(&mfi);
	obs_log_loaded_modules();
	obs_post_load_modules();
	obs_module_failure_info_free(&mfi);

	modulesLoaded_ = true;
}

int ObsContext::resetVideo(uint32_t baseWidth, uint32_t baseHeight, int fpsNum, uint32_t outWidth,
			   uint32_t outHeight)
{
	if (baseWidth < 32)
		baseWidth = 1920;
	if (baseHeight < 32)
		baseHeight = 1080;
	if (outWidth < 32)
		outWidth = baseWidth;
	if (outHeight < 32)
		outHeight = baseHeight;

	struct obs_video_info ovi = {};
	ovi.graphics_module = renderModule();
	ovi.fps_num = fpsNum > 0 ? (uint32_t)fpsNum : 30;
	ovi.fps_den = 1;
	ovi.base_width = baseWidth;
	ovi.base_height = baseHeight;
	ovi.output_width = outWidth;
	ovi.output_height = outHeight;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;

	int ret = obs_reset_video(&ovi);
	if (ret != OBS_VIDEO_SUCCESS)
		blog(LOG_ERROR, "[harpia] obs_reset_video failed: %d", ret);
	return ret;
}

bool ObsContext::resetAudio()
{
	struct obs_audio_info2 ai = {};
	ai.samples_per_sec = 48000;
	ai.speakers = SPEAKERS_STEREO;
	const bool ok = obs_reset_audio2(&ai);
	if (!ok)
		blog(LOG_ERROR, "[harpia] obs_reset_audio2 failed");
	return ok;
}

namespace {

bool typeInEnum(bool (*enumFn)(size_t, const char **), const char *id)
{
	const char *cur = nullptr;
	for (size_t i = 0; enumFn(i, &cur); i++) {
		if (cur && std::strcmp(cur, id) == 0)
			return true;
	}
	return false;
}

} // namespace

std::vector<std::string> ObsContext::missingDependencies() const
{
	std::vector<std::string> missing;

	// A screen-capture source for this platform (any of the accepted ids).
#if defined(_WIN32)
	const char *captureIds[] = {"monitor_capture"};
	const char *capturePlugin = "win-capture";
#elif defined(__APPLE__)
	const char *captureIds[] = {"screen_capture", "display_capture"};
	const char *capturePlugin = "mac-capture";
#else
	const char *captureIds[] = {"xshm_input", "pipewire-screen-capture-source"};
	const char *capturePlugin = "linux-capture / linux-pipewire";
#endif
	bool haveCapture = false;
	for (const char *id : captureIds) {
		if (typeInEnum(obs_enum_input_types, id))
			haveCapture = true;
	}
	if (!haveCapture)
		missing.push_back(std::string("screen capture plugin (") + capturePlugin + ")");

	if (!typeInEnum(obs_enum_encoder_types, "obs_x264"))
		missing.push_back("H.264 video encoder (obs-x264)");
	if (!typeInEnum(obs_enum_encoder_types, "ffmpeg_aac"))
		missing.push_back("AAC audio encoder (obs-ffmpeg)");
	if (!typeInEnum(obs_enum_output_types, "ffmpeg_muxer"))
		missing.push_back("recording output (obs-ffmpeg: ffmpeg_muxer)");

	return missing;
}

void ObsContext::shutdown()
{
	if (!initialized_)
		return;
	obs_shutdown();
	initialized_ = false;
	modulesLoaded_ = false;
}

} // namespace harpia
