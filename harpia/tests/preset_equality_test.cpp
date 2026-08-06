// Preset equality, field by field.
//
// This exists for one caller: Cancel in the preset editor, which asks "did
// anything actually change?" before warning that it is about to throw work
// away. Get it wrong in the direction that matters -- a field left out of the
// comparison -- and two different presets compare EQUAL, so Cancel discards a
// real edit silently. That is exactly the bug the prompt was added to prevent,
// reintroduced one layer down.
//
// So every field is pinned here by changing it and demanding a difference. A
// field added to Preset and forgotten in operator== fails nothing on its own --
// nothing can detect that automatically in C++17 without reflection -- but the
// moment someone adds it to this list, it fails loudly. The comment on
// operator== says to do both.
#include "model/Preset.hpp"

#include <cstdio>
#include <functional>
#include <vector>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// Change one field, and the two presets must stop being equal.
static void pin(const char *field, const std::function<void(Preset &)> &change)
{
	Preset a = Preset::makeDefault("/tmp/out");
	Preset b = a;
	if (a != b) {
		std::printf("  FAIL %s: two copies were not equal to begin with\n", field);
		++failures;
		return;
	}
	change(b);
	const bool differs = (a != b);
	std::printf("  %s %s\n", differs ? "PASS" : "FAIL", field);
	if (!differs)
		++failures;
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::printf("\n-- the baseline --\n");
	{
		const Preset a = Preset::makeDefault("/tmp/out");
		const Preset b = a;
		ok(a == b, "a copy equals its original");
		ok(!(a != b), "and != agrees with ==");

		Preset c = a;
		c.name = "different";
		ok(!(a == c) && (a != c), "one changed field is enough to differ");
	}

	std::printf("\n-- every field the editor can change --\n");
	pin("id", [](Preset &p) { p.id = "other"; });
	pin("name", [](Preset &p) { p.name = "other"; });
	pin("format", [](Preset &p) { p.format = RecordingFormat::MKV; });
	pin("codec", [](Preset &p) { p.codec = VideoCodec::AV1; });
	pin("frameRateMode", [](Preset &p) { p.frameRateMode = FrameRateMode::VFR; });
	pin("fps", [](Preset &p) { p.fps += 1; });
	pin("outputFolder", [](Preset &p) { p.outputFolder = "/elsewhere"; });
	pin("monitorIndex", [](Preset &p) { p.monitorIndex += 1; });
	pin("gpuCompression", [](Preset &p) { p.gpuCompression = !p.gpuCompression; });
	pin("videoBitrateKbps", [](Preset &p) { p.videoBitrateKbps += 1000; });
	pin("audioBitrateKbps", [](Preset &p) { p.audioBitrateKbps += 32; });
	pin("recordDesktopAudio", [](Preset &p) { p.recordDesktopAudio = !p.recordDesktopAudio; });
	pin("micDeviceIds", [](Preset &p) { p.micDeviceIds.push_back("mic-1"); });
	pin("desktopVolume", [](Preset &p) { p.desktopVolume = 0.5; });
	pin("micVolumes", [](Preset &p) { p.micVolumes["mic-1"] = 0.5; });
	pin("showMouseCursor", [](Preset &p) { p.showMouseCursor = !p.showMouseCursor; });
	pin("showMouseArea", [](Preset &p) { p.showMouseArea = !p.showMouseArea; });
	pin("mouseHighlightColor", [](Preset &p) { p.mouseHighlightColor = "#000000"; });
	pin("mouseHighlightSize", [](Preset &p) { p.mouseHighlightSize += 1; });
	pin("recordMouseClicks", [](Preset &p) { p.recordMouseClicks = !p.recordMouseClicks; });
	pin("leftClickColor", [](Preset &p) { p.leftClickColor = "#000000"; });
	pin("rightClickColor", [](Preset &p) { p.rightClickColor = "#000000"; });
	pin("followMouse", [](Preset &p) { p.followMouse = !p.followMouse; });
	pin("followPaddingPct", [](Preset &p) { p.followPaddingPct += 1; });
	pin("followSmoothness", [](Preset &p) { p.followSmoothness += 1; });
	pin("followAxis", [](Preset &p) { p.followAxis += 1; });
	pin("followProfile", [](Preset &p) { p.followProfile += 1; });
	pin("followShortcut", [](Preset &p) { p.followShortcut = "F7"; });
	pin("spotlightEnabled", [](Preset &p) { p.spotlightEnabled = !p.spotlightEnabled; });
	pin("spotlightStartOn", [](Preset &p) { p.spotlightStartOn = !p.spotlightStartOn; });
	pin("spotlightSize", [](Preset &p) { p.spotlightSize += 1; });
	pin("spotlightDarkPct", [](Preset &p) { p.spotlightDarkPct += 1; });
	pin("spotlightRoundness", [](Preset &p) { p.spotlightRoundness -= 1; });
	pin("spotlightShortcut", [](Preset &p) { p.spotlightShortcut = "F8"; });
	pin("zoomEnabled", [](Preset &p) { p.zoomEnabled = !p.zoomEnabled; });
	pin("zoomPercent", [](Preset &p) { p.zoomPercent += 10; });
	pin("zoomAnimMs", [](Preset &p) { p.zoomAnimMs += 10; });
	pin("zoomFollowSmoothness", [](Preset &p) { p.zoomFollowSmoothness += 1; });
	pin("zoomFollowPaddingPct", [](Preset &p) { p.zoomFollowPaddingPct += 1; });
	pin("zoomFollowAxis", [](Preset &p) { p.zoomFollowAxis += 1; });
	pin("zoomShortcut", [](Preset &p) { p.zoomShortcut = "F3"; });
	pin("webcamEnabled", [](Preset &p) { p.webcamEnabled = !p.webcamEnabled; });
	pin("webcamDeviceId", [](Preset &p) { p.webcamDeviceId = "cam-1"; });
	pin("webcamWidth", [](Preset &p) { p.webcamWidth += 1; });
	pin("webcamHeight", [](Preset &p) { p.webcamHeight += 1; });
	pin("webcamFps", [](Preset &p) { p.webcamFps += 1; });
	pin("webcamUseCustomFolder", [](Preset &p) { p.webcamUseCustomFolder = !p.webcamUseCustomFolder; });
	pin("webcamFolder", [](Preset &p) { p.webcamFolder = "/cam"; });
	pin("filenameTemplate", [](Preset &p) { p.filenameTemplate = "{Year}"; });
	pin("idleTimeoutSeconds", [](Preset &p) { p.idleTimeoutSeconds += 1; });
	pin("regionLeavePauseSeconds", [](Preset &p) { p.regionLeavePauseSeconds += 1; });
	pin("captureMode", [](Preset &p) { p.captureMode += 1; });
	pin("recordingCounter", [](Preset &p) { p.recordingCounter += 1; });
	pin("countdownSeconds", [](Preset &p) { p.countdownSeconds += 1; });
	pin("minRecordingSeconds", [](Preset &p) { p.minRecordingSeconds += 1; });
	pin("googleDriveLink", [](Preset &p) { p.googleDriveLink = "https://example.com"; });
	pin("showScreenBorder", [](Preset &p) { p.showScreenBorder = !p.showScreenBorder; });
	pin("screenBorderColor", [](Preset &p) { p.screenBorderColor = "#000000"; });
	pin("screenBorderThickness", [](Preset &p) { p.screenBorderThickness += 1; });
	pin("regionMoveHandle", [](Preset &p) { p.regionMoveHandle = !p.regionMoveHandle; });

	std::printf("\n-- the containers, where a shallow compare would pass --\n");
	{
		// A vector and a map compared by size alone, or not compared at all,
		// is the classic way this goes wrong: the mic list is the field most
		// likely to be edited and the least likely to be noticed.
		Preset a = Preset::makeDefault("/tmp/out");
		a.micDeviceIds = {"a", "b"};
		Preset b = a;
		b.micDeviceIds = {"b", "a"};
		ok(a != b, "reordering the mic list is a change, not the same list");

		b.micDeviceIds = {"a", "b"};
		ok(a == b, "and putting it back makes them equal again");

		a.micVolumes["a"] = 0.5;
		b.micVolumes["a"] = 0.6;
		ok(a != b, "a volume that differs by 0.1 is a change");
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "all preset-equality checks passed",
		    failures);
	return failures ? 1 : 0;
}
