#pragma once

// "PC audio from": which programs' sound the recorder takes when Record PC
// Audio is on. One choice, shared by every preset.
//
//   All apps              the whole mix, as before
//   Only these apps       a capture per app (a game, a browser): music playing
//                         anywhere else stays out of the file
//   All apps except one   everything but one app -- the music player, left out
//                         once, for every game after
//
// Apps are remembered by Windows executable name ("spotify.exe") or macOS
// bundle identifier ("com.spotify.client"). Normalized keys survive launches;
// the native Mac filter compares bundle identifiers without case sensitivity.
// Per-app capture uses Windows process loopback or macOS 13+ ScreenCaptureKit.
// Unsupported backends fall back to the whole mix.
//
// Qt-free and pure: what to capture for a choice is decided here, so the rules
// are checkable without a native sound device or libobs.

#include <string>
#include <vector>

namespace harpia {

// The libobs source that records one app, or all but one (core/AppAudioSource).
constexpr const char *kAppAudioSourceId = "harpia_app_audio_capture";
// How many apps "Only these apps" takes. Each is a capture of its own, on an
// audio channel of its own, and libobs has a fixed number of those.
constexpr int kMaxPcAudioApps = 8;

enum class PcAudioFrom { All = 0, OnlyApps = 1, AllExceptApp = 2 };

struct PcAudioChoice {
	PcAudioFrom from = PcAudioFrom::All;
	// Both are kept whichever mode is on: trying the other mode and coming
	// back loses nothing.
	std::vector<std::string> onlyApps; // "chrome.exe", ... for Only these apps
	std::string exceptApp;             // "spotify.exe" for All apps except one

	bool operator==(const PcAudioChoice &o) const
	{
		return from == o.from && onlyApps == o.onlyApps && exceptApp == o.exceptApp;
	}
	bool operator!=(const PcAudioChoice &o) const { return !(*this == o); }
};

// "C:\Games\Game.EXE" or " Game.exe " -> "game.exe": the file name, trimmed,
// in lower case. A macOS bundle identifier is kept as a normalized identifier;
// native matching is case-insensitive as are these saved keys.
inline std::string normalizeAppExe(const std::string &s)
{
	const size_t slash = s.find_last_of("/\\");
	std::string name = slash == std::string::npos ? s : s.substr(slash + 1);
	const size_t first = name.find_first_not_of(" \t\r\n");
	const size_t last = name.find_last_not_of(" \t\r\n");
	name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
	for (char &ch : name)
		if (ch >= 'A' && ch <= 'Z')
			ch = char(ch - 'A' + 'a');
	return name;
}

// How the mode is saved: "all", "only", "except". Anything else reads as All.
inline const char *pcAudioFromKey(PcAudioFrom f)
{
	switch (f) {
	case PcAudioFrom::OnlyApps:
		return "only";
	case PcAudioFrom::AllExceptApp:
		return "except";
	case PcAudioFrom::All:
		break;
	}
	return "all";
}
inline PcAudioFrom pcAudioFromKey(const std::string &key)
{
	if (key == "only")
		return PcAudioFrom::OnlyApps;
	if (key == "except")
		return PcAudioFrom::AllExceptApp;
	return PcAudioFrom::All;
}

// One capture to make: the whole mix, one app, or everything but one app.
struct PcAudioCapture {
	std::string exe;      // empty: the whole mix
	bool exclude = false; // everything BUT `exe`

	bool wholeMix() const { return exe.empty(); }
	bool operator==(const PcAudioCapture &o) const { return exe == o.exe && exclude == o.exclude; }
};

// What to record for `c`. `appsAvailable` says whether this system can single
// apps out at all; without it, the whole mix. "Only these apps" with none
// picked records no PC sound (the picker does not let that happen by
// accident); "All apps except one" with none picked is all apps.
inline std::vector<PcAudioCapture> pcAudioCaptures(const PcAudioChoice &c, bool appsAvailable)
{
	if (!appsAvailable || c.from == PcAudioFrom::All)
		return {PcAudioCapture()};
	if (c.from == PcAudioFrom::AllExceptApp) {
		PcAudioCapture one;
		one.exe = normalizeAppExe(c.exceptApp);
		one.exclude = !one.exe.empty();
		return {one};
	}
	std::vector<PcAudioCapture> out;
	for (const std::string &app : c.onlyApps) {
		PcAudioCapture one;
		one.exe = normalizeAppExe(app);
		if (one.exe.empty())
			continue;
		bool seen = false;
		for (const PcAudioCapture &o : out)
			seen = seen || o.exe == one.exe;
		if (!seen && int(out.size()) < kMaxPcAudioApps)
			out.push_back(one);
	}
	return out;
}

} // namespace harpia
