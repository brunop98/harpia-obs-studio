// Which options apply in which capture mode.
//
// This is the table the window and the preset editor both consult instead of
// deciding for themselves. It exists because the alternative -- an
// `if (mode == Region)` at each of a dozen call sites -- drifts, and the
// symptom of drift is a setting that is visibly switched on while doing
// absolutely nothing. That has bitten this project before.
//
// So the checks below are mostly a restatement of the intended table, which is
// the point: they are the thing that fails when someone adds a mode and forgets
// a rule. The interesting ones are the invariants at the bottom, which hold no
// matter how many modes get added later.
#include "core/ModeCapabilities.hpp"

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const RecordMode mon = RecordMode::Monitor;
	const RecordMode reg = RecordMode::Region;
	const RecordMode aud = RecordMode::AudioOnly;

	std::printf("\n-- the stored value --\n");
	{
		ok(recordModeFromInt(0) == mon && recordModeFromInt(1) == reg &&
			   recordModeFromInt(2) == aud,
		   "0/1/2 map to Monitor/Region/Audio Only");
		// A preset written by a NEWER build must still load. Falling back to
		// Monitor is the safe answer: it is the default and it always works.
		ok(recordModeFromInt(99) == mon, "an unknown mode degrades to Monitor rather than failing");
		ok(recordModeFromInt(-1) == mon, "including a negative one");
		for (RecordMode m : {mon, reg, aud})
			ok(recordModeFromInt(recordModeToInt(m)) == m, "and every mode round-trips");
	}

	std::printf("\n-- audio only has no picture --\n");
	{
		ok(!modeHasVideo(aud), "no video");
		ok(!modeUsesMonitor(aud), "so no display to choose");
		ok(!modeUsesRegion(aud), "no region");
		ok(!modeSupportsZoom(aud), "nothing to zoom");
		ok(!modeSupportsSpotlight(aud), "nothing to light");
		ok(!modeSupportsFollowMouse(aud), "nothing to follow");
		ok(!modeSupportsMouseFx(aud), "no capture to draw the cursor into");
		ok(!modeSupportsScreenBorder(aud), "nothing to outline");
		ok(!modeSupportsWebcam(aud), "and no camera — 'audio only' plus a video file is a contradiction");
		ok(!modeUsesVideoEncoder(aud), "no video encoder is created at all");
	}

	std::printf("\n-- but it still records --\n");
	{
		// The mode is about WHAT is captured, not about how the recording is
		// controlled. Everything in that second group still applies.
		ok(modeSupportsCountdown(aud), "a countdown before the microphone goes live still helps");
		ok(modeSupportsIdlePause(aud), "idle auto-pause still applies");
		ok(modeSupportsAppFocus(aud), "so does pausing when an app loses focus");
		ok(QString(audioOnlyExtension()) == QStringLiteral("m4a"),
		   "and it is written as M4A, the one audio encoder FFmpeg always has");
	}

	std::printf("\n-- region-only options --\n");
	{
		ok(modeSupportsFollowMouse(reg) && !modeSupportsFollowMouse(mon),
		   "Follow Mouse needs a region to pan");
		ok(modeSupportsRegionLeavePause(reg) && !modeSupportsRegionLeavePause(mon),
		   "so does 'pause when the pointer leaves'");
		ok(modeUsesRegion(reg) && !modeUsesRegion(mon), "and the region frame itself");
	}

	std::printf("\n-- monitor-only options --\n");
	{
		ok(modeSupportsScreenBorder(mon) && !modeSupportsScreenBorder(reg),
		   "the screen border outlines a display; a region has its own frame");
	}

	std::printf("\n-- shared by both video modes --\n");
	{
		// Zoom and Spotlight were each Full-Screen-only at some point and are
		// not any more. This is what stops that regressing quietly.
		ok(modeSupportsZoom(mon) && modeSupportsZoom(reg), "Zoom works in both video modes");
		ok(modeSupportsSpotlight(mon) && modeSupportsSpotlight(reg), "so does the Spotlight");
		ok(modeSupportsMouseFx(mon) && modeSupportsMouseFx(reg), "and the mouse effects");
		ok(modeUsesMonitor(reg), "a region still needs to know WHICH display it is on");
	}

	std::printf("\n-- invariants that survive a new mode --\n");
	{
		// These are the checks that still mean something after someone adds a
		// fourth mode, because they are relationships rather than a list.
		for (RecordMode m : {mon, reg, aud}) {
			const char *name = qUtf8Printable(recordModeLabel(m));
			if (!modeHasVideo(m)) {
				const bool clean = !modeUsesRegion(m) && !modeSupportsZoom(m) &&
						   !modeSupportsSpotlight(m) && !modeSupportsFollowMouse(m) &&
						   !modeSupportsScreenBorder(m) && !modeSupportsMouseFx(m) &&
						   !modeUsesVideoEncoder(m);
				std::printf("     %s has no video\n", name);
				ok(clean, "a mode without video enables NO picture option");
			}
			// Anything that pans a region needs a region to pan.
			ok(!modeSupportsFollowMouse(m) || modeUsesRegion(m),
			   "following implies there is a region");
			// Anything that leaves a region needs one too.
			ok(!modeSupportsRegionLeavePause(m) || modeUsesRegion(m),
			   "leaving a region implies there is a region");
			// A region always belongs to a display.
			ok(!modeUsesRegion(m) || modeUsesMonitor(m), "a region implies a display");
		}
	}

	std::printf("\n-- the tags and labels --\n");
	{
		ok(QString(recordModeTag(mon)) == QStringLiteral("monitor") &&
			   QString(recordModeTag(reg)) == QStringLiteral("region") &&
			   QString(recordModeTag(aud)) == QStringLiteral("audio"),
		   "every mode has its own combo tag");
		ok(recordModeLabel(aud) == QStringLiteral("Audio Only"), "and a human label");
	}

	std::printf("\n-- the reason a control is off --\n");
	{
		ok(modeDisabledReason(aud, true).isEmpty(), "an available control needs no excuse");
		ok(!modeDisabledReason(aud, false).isEmpty(), "an unavailable one explains itself");
		ok(modeDisabledReason(mon, false).contains(QStringLiteral("Custom Region")),
		   "and in Entire Monitor it names the mode that WOULD offer it");
		ok(modeDisabledReason(aud, false).contains(QStringLiteral("audio only")),
		   "while audio only says so plainly");
	}

	std::printf("\n%s (%d failures)\n",
		    failures ? "FAILURES" : "all mode-capability checks passed", failures);
	return failures ? 1 : 0;
}
