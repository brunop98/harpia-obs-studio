#include "ObsContext.hpp"
#include "VideoStatus.hpp"

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

	// Already running exactly like this? Then there is nothing to reset. Every
	// other field below is a constant, so these five are the whole request.
	if (videoReady_ && baseWidth == lastBaseW_ && baseHeight == lastBaseH_ &&
	    outWidth == lastOutW_ && outHeight == lastOutH_ && fpsNum == lastFps_)
		return OBS_VIDEO_SUCCESS;

	const auto attempt = [&](uint32_t bw, uint32_t bh, uint32_t ow, uint32_t oh) {
		struct obs_video_info ovi = {};
		ovi.graphics_module = renderModule();
		ovi.fps_num = fpsNum > 0 ? (uint32_t)fpsNum : 30;
		ovi.fps_den = 1;
		ovi.base_width = bw;
		ovi.base_height = bh;
		ovi.output_width = ow;
		ovi.output_height = oh;
		ovi.output_format = VIDEO_FORMAT_NV12;
		ovi.colorspace = VIDEO_CS_709;
		ovi.range = VIDEO_RANGE_PARTIAL;
		ovi.adapter = 0;
		ovi.gpu_conversion = true;
		ovi.scale_type = OBS_SCALE_BICUBIC;
		return obs_reset_video(&ovi);
	};

	int ret = attempt(baseWidth, baseHeight, outWidth, outHeight);
	if (ret == OBS_VIDEO_CURRENTLY_ACTIVE) {
		// Refused before touching anything: the old video is still running.
		blog(LOG_WARNING, "[harpia] obs_reset_video %ux%u refused: video is in use", baseWidth, baseHeight);
		return ret;
	}
	if (ret != OBS_VIDEO_SUCCESS) {
		// obs_reset_video tears the running video down BEFORE building the
		// new one, so a failure here leaves no video engine at all -- and the
		// "already running like this" shortcut above must not answer for it.
		videoReady_ = false;
		const int first = ret;
		blog(LOG_ERROR, "[harpia] obs_reset_video failed: %d (%ux%u -> %ux%u @ %d fps)", ret, baseWidth,
		     baseHeight, outWidth, outHeight, fpsNum);
		// Once more as asked -- a GPU busy with a mode change or a driver
		// hiccup often succeeds a moment later.
		if (ret != OBS_VIDEO_MODULE_NOT_FOUND)
			ret = attempt(baseWidth, baseHeight, outWidth, outHeight);
		// Then at sizes the colour conversion always accepts: width a multiple
		// of 4, height of 2. A region can be any size; the crop simply loses
		// the odd pixels at the edge.
		const uint32_t bw = baseWidth & ~3u, bh = baseHeight & ~1u;
		if (ret != OBS_VIDEO_SUCCESS && ret != OBS_VIDEO_MODULE_NOT_FOUND &&
		    (bw != baseWidth || bh != baseHeight || (outWidth & 3u) || (outHeight & 1u)) && bw >= 32 &&
		    bh >= 32) {
			ret = attempt(bw, bh, outWidth & ~3u, outHeight & ~1u);
			if (ret == OBS_VIDEO_SUCCESS) {
				blog(LOG_WARNING, "[harpia] video engine recovered at %ux%u", bw, bh);
				baseWidth = bw;
				baseHeight = bh;
				outWidth &= ~3u;
				outHeight &= ~1u;
			}
		}
		if (ret != OBS_VIDEO_SUCCESS) {
			lastVideoResetError() = describeVideoResetError(first, baseWidth, baseHeight);
			blog(LOG_ERROR, "[harpia] video engine is down: %s", lastVideoResetError().c_str());
			return first;
		}
	}
	lastVideoResetError().clear();
	videoReady_ = true;
	lastBaseW_ = baseWidth;
	lastBaseH_ = baseHeight;
	lastOutW_ = outWidth;
	lastOutH_ = outHeight;
	lastFps_ = fpsNum;
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

	// The video engine itself: without its graphics module (libobs-d3d11.dll
	// on Windows) there is no capture, no filters and no recording, and every
	// source fails quietly until Record crashes on the missing device.
	if (!videoReady_)
		missing.push_back(std::string("the video engine -- its graphics module (") + renderModule() +
				  ") did not load");

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
	// obs_shutdown() takes the video pipeline with it, so the cached "already
	// running like this" must not survive into a restart -- it would skip the
	// one reset that genuinely has to happen.
	videoReady_ = false;
}

} // namespace harpia
