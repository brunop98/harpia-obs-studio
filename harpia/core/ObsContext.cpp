#include "ObsContext.hpp"
#if defined(__APPLE__)
#include <QCoreApplication>
#include <QByteArray>
#include <QString>
#endif

#include "GraphicsModuleDiagnostics.hpp"
#include "AppAudioSource.hpp"
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
#elif defined(__APPLE__)
	static const QByteArray module = (QCoreApplication::applicationDirPath() +
		QStringLiteral("/../Frameworks/") + QString::fromUtf8(DL_OPENGL)).toUtf8();
	return module.constData();
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
	// Capture framing uses device pixels at 1:1 scale. libobs otherwise stores
	// scene items relative to the canvas, automatically shrinking/repositioning
	// them when resetVideo switches from a monitor to a smaller Custom Region.
	// Set this before creating any scene, including secondary area captures.
	obs_data_t *sceneSettings = obs_data_create();
	obs_data_set_bool(sceneSettings, "AbsoluteCoordinates", true);
	obs_apply_private_data(sceneSettings);
	obs_data_release(sceneSettings);

	initialized_ = true;
	blog(LOG_INFO, "[harpia] backend: OBS %s; renderer module='%s'; scene coordinates=absolute device pixels",
	     obs_get_version_string(), renderModule());
#if defined(_WIN32)
	// This inventory does not depend on loading D3D11. It remains useful when
	// the renderer DLL failed and there is no graphics context to ask for a GPU.
	for (DWORD i = 0; i < 32; ++i) {
		DISPLAY_DEVICEW display = {};
		display.cb = sizeof(display);
		if (!EnumDisplayDevicesW(nullptr, i, &display, 0)) {
			break;
		}
		blog(LOG_INFO,
		     "[harpia] Windows display adapter #%lu: '%s'; display='%s'; desktop=%s; primary=%s; flags=0x%lx",
		     static_cast<unsigned long>(i), graphics_module_diagnostics::utf8(display.DeviceString).c_str(),
		     graphics_module_diagnostics::utf8(display.DeviceName).c_str(),
		     (display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ? "yes" : "no",
		     (display.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? "yes" : "no",
		     static_cast<unsigned long>(display.StateFlags));
	}
#endif
	return true;
}

void ObsContext::addModulePaths()
{
	// Environment overrides let a dev point at a build tree without installing.
	const char *pluginsPath = std::getenv("OBS_PLUGINS_PATH");
	const char *pluginsDataPath = std::getenv("OBS_PLUGINS_DATA_PATH");
	if (pluginsPath && pluginsDataPath) {
		std::string dataWithModule = std::string(pluginsDataPath) + "/%module%";
		blog(LOG_INFO, "[harpia] plugin override: binaries='%s'; data='%s'", pluginsPath,
		     dataWithModule.c_str());
		obs_add_module_path(pluginsPath, dataWithModule.c_str());
	}

#if defined(__APPLE__)
	// macOS bundle layout — a single, unambiguous path.
	// libobs registers the absolute NSBundle plugin path itself. Harpia's
	// former relative entry did not point inside .plugin/Contents/MacOS.
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
		blog(LOG_INFO, "[harpia] plugin path: binaries='%s'; data='%s'", binStr.c_str(), dataPattern.c_str());
		obs_add_module_path(binStr.c_str(), dataPattern.c_str());
	}

	// Fallback for any unusual layout the candidates didn't match: keep the
	// original relative registration (single entry, so still no duplication).
	if (registered.empty()) {
		blog(LOG_WARNING, "[harpia] no plugin folder found beside executable; using relative plugin paths");
		obs_add_module_path("obs-plugins/64bit", "data/obs-plugins/%module%");
	}
#endif
}

void ObsContext::loadModules()
{
	if (modulesLoaded_)
		return;

	addModulePaths();
	// These modules call OBS Studio's frontend API while loading. Harpia owns
	// its interface and does not install those callbacks; frontend-tools can
	// dereference a null frontend configuration before startup completes.
	obs_add_disabled_module("frontend-tools");
	obs_add_disabled_module("obs-websocket");

	struct obs_module_failure_info mfi;
	obs_load_all_modules2(&mfi);
	obs_log_loaded_modules();
	obs_post_load_modules();
	for (size_t i = 0; i < mfi.count; ++i) {
		blog(LOG_WARNING, "[harpia] plugin failed to load: %s", mfi.failed_modules[i]);
	}
	obs_module_failure_info_free(&mfi);
	// Harpia's own: the per-app PC audio capture, where Windows has it.
	app_audio::registerSource();

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
		blog(LOG_INFO,
		     "[harpia] video reset request: module='%s'; adapter=%u; canvas=%ux%u; output=%ux%u; fps=%u/%u; format=NV12; colorspace=709; range=partial; scale=bicubic; gpu-conversion=yes",
		     ovi.graphics_module, ovi.adapter, bw, bh, ow, oh, ovi.fps_num, ovi.fps_den);
		const int result = obs_reset_video(&ovi);
		blog(result == OBS_VIDEO_SUCCESS ? LOG_INFO : LOG_WARNING,
		     "[harpia] video reset result: %d (canvas=%ux%u; output=%ux%u)", result, bw, bh, ow, oh);
		return result;
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
			lastVideoResetError() = describeVideoResetError(first, baseWidth, baseHeight, renderModule());
#if defined(_WIN32)
			if (first == OBS_VIDEO_MODULE_NOT_FOUND && *renderModule())
				lastVideoResetError() = "OBS could not load its graphics module (OBS error -5). " +
						       inspectWindowsGraphicsModule(renderModule());
#endif
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
	logVideoDiagnostics("video reset completed");
	return ret;
}

void ObsContext::logVideoDiagnostics(const char *reason) const
{
	const char *context = reason ? reason : "snapshot";
	struct obs_video_info video = {};
	if (!initialized_ || !obs_get_video_info(&video)) {
		blog(LOG_WARNING, "[harpia] video diagnostics (%s): engine unavailable; %s", context,
		     describeVideoNotReady().c_str());
		return;
	}
	blog(LOG_INFO,
	     "[harpia] video diagnostics (%s): module='%s'; adapter=%u; canvas=%ux%u; output=%ux%u; fps=%u/%u; format=%s; colorspace=%s; range=%s; scale=%d; gpu-conversion=%s",
	     context, video.graphics_module ? video.graphics_module : "<unknown>", video.adapter, video.base_width,
	     video.base_height, video.output_width, video.output_height, video.fps_num, video.fps_den,
	     get_video_format_name(video.output_format), get_video_colorspace_name(video.colorspace),
	     get_video_range_name(video.output_format, video.range), static_cast<int>(video.scale_type),
	     video.gpu_conversion ? "yes" : "no");
	obs_enter_graphics();
	if (gs_get_context()) {
		const char *api = gs_get_device_name();
		const char *gpu = gs_get_renderer();
		const char *driver = gs_get_driver_version();
		blog(LOG_INFO,
		     "[harpia] graphics: API='%s'; GPU='%s'; driver='%s'; dedicated-memory-MiB=%llu; shared-memory-MiB=%llu (0=unreported)",
		     api ? api : "<unreported>", gpu ? gpu : "<unreported>", driver ? driver : "<unreported>",
		     static_cast<unsigned long long>(gs_get_gpu_dmem() / (1024 * 1024)),
		     static_cast<unsigned long long>(gs_get_gpu_smem() / (1024 * 1024)));
	} else {
		blog(LOG_WARNING, "[harpia] graphics diagnostics: no active graphics context");
	}
	obs_leave_graphics();
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

	// The engine's own data (data/libobs: the shaders). Without it the video
	// engine cannot start either, and the next check would blame the graphics
	// module; this one says where the problem is -- an incomplete program folder.
	if (char *effect = obs_find_data_file("default.effect"))
		bfree(effect);
	else
		missing.push_back("the video engine's data (data/libobs/default.effect, two folders up from the program) "
				  "-- is the program folder complete?");

	// Keep the actual startup failure: a missing module and a graphics driver
	// refusing device creation need different remedies.
	if (!videoReady_)
		missing.push_back(describeVideoNotReady());

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
