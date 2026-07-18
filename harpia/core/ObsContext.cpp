#include "ObsContext.hpp"

#include <obs.h>
#include <util/platform.h>

#include <cstdlib>
#include <string>

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

	// Default install layout next to the executable. obs_add_module_path
	// resolves paths relative to the module search roots libobs already knows
	// about (the bundle directory). These cover the common per-OS layouts.
#if defined(_WIN32)
	obs_add_module_path("../../obs-plugins/64bit", "../../data/obs-plugins/%module%");
	obs_add_module_path("obs-plugins/64bit", "data/obs-plugins/%module%");
#elif defined(__APPLE__)
	obs_add_module_path("../PlugIns", "../PlugIns/%module%.plugin/Contents/Resources");
#else
	obs_add_module_path("../../obs-plugins/64bit", "../../data/obs-plugins/%module%");
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

void ObsContext::shutdown()
{
	if (!initialized_)
		return;
	obs_shutdown();
	initialized_ = false;
	modulesLoaded_ = false;
}

} // namespace harpia
