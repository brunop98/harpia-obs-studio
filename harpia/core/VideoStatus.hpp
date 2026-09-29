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

inline std::string describeVideoResetError(int code, uint32_t w, uint32_t h)
{
	const std::string size = std::to_string(w) + "x" + std::to_string(h);
	switch (code) {
	case -5: // OBS_VIDEO_MODULE_NOT_FOUND
		return "OBS could not load its graphics module (libobs-d3d11.dll next to harpia.exe). This "
		       "happens when the app starts while a build is still copying files. Restart Harpia; if it "
		       "happens again, rebuild or reinstall.";
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
