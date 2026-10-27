#pragma once

// Why the video engine is not running, in words -- shared by ObsContext (which
// sees obs_reset_video fail) and RecordingController (which has to tell the
// user when it refuses to record). No libobs include: the codes are libobs'
// OBS_VIDEO_* values, restated so this stays a plain header.

#include <cstdint>
#include <string>

namespace harpia {

// The last video reset that failed and left no video behind, described; empty
// once one succeeds. Written by ObsContext::resetVideo.
inline std::string &lastVideoResetError()
{
	static std::string s;
	return s;
}

inline std::string describeVideoNotReady()
{
	return lastVideoResetError().empty()
		       ? "the video engine has not initialized -- see Error Logs for details"
		       : "the video engine -- " + lastVideoResetError();
}

// Pass the configured renderer name; the default preserves older Windows callers.
inline std::string describeVideoResetError(int code, uint32_t w, uint32_t h,
					 const std::string &graphicsModule = "libobs-d3d11.dll")
{
	const std::string size = std::to_string(w) + "x" + std::to_string(h);
	switch (code) {
	case -5: { // OBS_VIDEO_MODULE_NOT_FOUND
		if (graphicsModule.empty())
			return "OBS could not load its graphics module: no renderer was configured in this build. "
			       "Rebuild the complete application.";
		std::string message = "OBS could not load its graphics module (" + graphicsModule + "). "
			"The module or one of its dependencies may be missing, incompatible, or blocked. "
			"Extract the complete Release or RelWithDebInfo package into a fresh folder and restart.";
		if (graphicsModule.find(".dll") != std::string::npos)
			message += " On Windows, install or repair the Microsoft Visual C++ x64 Redistributable. "
				   "Enable Info in Error Logs and look for the preceding 'LoadLibrary failed' line "
				   "for the Windows error number.";
		else
			message += " The preceding library-loader messages in Error Logs have more details.";
		return message;
	}
	case -2: // OBS_VIDEO_NOT_SUPPORTED
		return "The graphics card or driver refused to start OBS's video engine (not supported). "
		       "Updating the graphics driver usually fixes this.";
	case -3: // OBS_VIDEO_INVALID_PARAM
		return "OBS rejected a " + size + " video canvas as invalid. Try a slightly different region size.";
	case -4: // OBS_VIDEO_CURRENTLY_ACTIVE
		return "OBS could not change the video size to " + size +
		       " because something was still using the video engine. Stop other captures and try again.";
	default:
		return "OBS could not set up its video engine for a " + size +
		       " recording (error " + std::to_string(code) +
		       "). The Error Logs have the details; restarting Harpia usually clears it.";
	}
}

} // namespace harpia
