#pragma once

// What each capture mode actually supports.
//
// The recorder grew three modes and about a dozen options, and most options
// only apply to some of them: Follow Mouse needs a region to pan, the screen
// border needs a whole display to draw around, and none of the video options
// mean anything at all when only audio is being recorded. Left to individual
// `if (mode == Region)` checks scattered through the window and the preset
// editor, those rules drift -- and a setting that is visibly on while doing
// nothing is the exact confusion this file exists to prevent.
//
// So the rules live here, in one table, as pure functions. The window and the
// preset editor both ask this rather than deciding for themselves, and
// modecapabilities_test pins the answers.

#include <QString>

namespace harpia {

// What a preset's captureMode integer means. Stored as an int in the preset
// (and in the JSON) so an unknown future value degrades to Monitor rather than
// refusing to load.
enum class RecordMode {
	Monitor = 0,   // the whole display
	Region = 1,    // a rectangle on it
	AudioOnly = 2, // no picture at all
};

inline RecordMode recordModeFromInt(int v)
{
	switch (v) {
	case 1:
		return RecordMode::Region;
	case 2:
		return RecordMode::AudioOnly;
	default:
		return RecordMode::Monitor;
	}
}

inline int recordModeToInt(RecordMode m)
{
	return int(m);
}

inline QString recordModeLabel(RecordMode m)
{
	switch (m) {
	case RecordMode::Region:
		return QStringLiteral("Custom Region");
	case RecordMode::AudioOnly:
		return QStringLiteral("Audio Only");
	case RecordMode::Monitor:
		break;
	}
	return QStringLiteral("Entire Monitor");
}

// The tag stored in the combo's item data, and read back from it.
inline const char *recordModeTag(RecordMode m)
{
	switch (m) {
	case RecordMode::Region:
		return "region";
	case RecordMode::AudioOnly:
		return "audio";
	case RecordMode::Monitor:
		break;
	}
	return "monitor";
}

// ---- the table -------------------------------------------------------------
//
// Each of these answers one question: does this option do anything in this
// mode? A control whose answer is false must be visibly unavailable, not
// merely ignored.

// Is there a picture at all? The gate for everything below it.
inline bool modeHasVideo(RecordMode m)
{
	return m != RecordMode::AudioOnly;
}

// Which display to capture. Meaningless with no picture; in Region mode it
// still matters, because the region lives on a particular display.
inline bool modeUsesMonitor(RecordMode m)
{
	return modeHasVideo(m);
}

// The draggable capture rectangle and everything hanging off it.
inline bool modeUsesRegion(RecordMode m)
{
	return m == RecordMode::Region;
}

// Follow Mouse pans the region to keep the cursor framed. Entire Monitor has
// nowhere to pan to, and Audio Only has nothing to pan.
inline bool modeSupportsFollowMouse(RecordMode m)
{
	return m == RecordMode::Region;
}

// "Pause when the pointer leaves the region" needs a region to leave.
inline bool modeSupportsRegionLeavePause(RecordMode m)
{
	return m == RecordMode::Region;
}

// The coloured border around the recorded display. Region has its own frame
// overlay instead, and Audio Only has nothing to outline.
inline bool modeSupportsScreenBorder(RecordMode m)
{
	return m == RecordMode::Monitor;
}

// Zoom and Spotlight both work on the captured picture, so both need one --
// but neither cares whether it is a display or a region.
inline bool modeSupportsZoom(RecordMode m)
{
	return modeHasVideo(m);
}
inline bool modeSupportsSpotlight(RecordMode m)
{
	return modeHasVideo(m);
}

// The cursor highlight and click ripples are drawn onto the desktop for the
// capture to pick up. With no capture there is nothing to pick them up.
inline bool modeSupportsMouseFx(RecordMode m)
{
	return modeHasVideo(m);
}

// The webcam is written as its own file, so it does not strictly need the
// screen -- but "record audio only" plus a camera is a contradiction the user
// would have to unpick later, and the mode's whole point is not writing video.
inline bool modeSupportsWebcam(RecordMode m)
{
	return modeHasVideo(m);
}

// The countdown, the idle auto-pause and "record only one application" are
// about WHEN to record rather than what, so they apply everywhere -- including
// to an audio-only take, where waiting three seconds before the microphone
// goes live is just as useful.
inline bool modeSupportsCountdown(RecordMode)
{
	return true;
}
inline bool modeSupportsIdlePause(RecordMode)
{
	return true;
}
inline bool modeSupportsAppFocus(RecordMode)
{
	return true;
}

// Video format, codec, frame rate, bitrate and GPU encoding: all describe a
// picture. An audio-only recording is written as M4A/AAC regardless.
inline bool modeUsesVideoEncoder(RecordMode m)
{
	return modeHasVideo(m);
}

// What an audio-only recording is written as. One format rather than a picker:
// AAC in an MP4 container is the only audio encoder FFmpeg is guaranteed to
// have (MP3 needs libmp3lame, which the bundled build may lack), it plays
// everywhere, and anyone who wants MP3 or WAV can run Extract Audio Only on the
// result -- which already offers exactly those, and already knows which of them
// this build can write.
inline const char *audioOnlyExtension()
{
	return "m4a";
}

// A one-line explanation for the control that is switched off, so the reason is
// on screen rather than in someone's head. Empty when the control applies.
inline QString modeDisabledReason(RecordMode m, bool controlApplies)
{
	if (controlApplies)
		return QString();
	switch (m) {
	case RecordMode::AudioOnly:
		return QStringLiteral("Not available while recording audio only.");
	case RecordMode::Monitor:
		return QStringLiteral("Only available when recording a Custom Region.");
	case RecordMode::Region:
		return QStringLiteral("Only available when recording an Entire Monitor.");
	}
	return QString();
}

} // namespace harpia
