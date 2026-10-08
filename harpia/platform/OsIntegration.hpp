#pragma once

// How Harpia presents itself to the operating system. Windows does the work
// (platform/win/WinOsIntegration.cpp); elsewhere these do nothing.
//
//   * A taskbar identity (AppUserModelID), so the running window and a pinned
//     shortcut group as one app and any future notification carries Harpia's
//     name and icon. An installer that makes a Start-menu shortcut has to put
//     the same id on it (System.AppUserModel.ID), or they will not group.
//   * Out of Windows 11 power throttling. While recording, Harpia's window is
//     usually minimised or hidden; Windows then puts the process into
//     Efficiency mode (EcoQoS) and coarsens its timers, which is how a
//     recording gets uneven frame pacing and dropped frames.
//   * The graphics card, on laptops with two: the same per-app choice as
//     Windows Settings > Display > Graphics. Off (Automatic) unless chosen,
//     because capturing a screen wired to one GPU from the other can come out
//     black on some machines -- so it is a setting to try, not a default.
//     Read by Windows when the process starts: a change applies next launch.

#include <QString>
#include <QStringList>

namespace harpia::os_integration {

inline constexpr const char *kAppUserModelId = "Harpia.RecorderEditor";

// First thing in main(), before any window. Returns a line for the log saying
// what was applied (empty where there is nothing to do).
QString applyAtStartup();

enum class GpuPreference { Automatic = 0, PowerSaving = 1, HighPerformance = 2 };

// Whether this platform has the per-app graphics preference (Windows only).
bool gpuPreferenceSupported();
// This executable's saved preference, and setting it. False on failure, or
// where unsupported.
GpuPreference gpuPreference();
bool setGpuPreference(GpuPreference p);

// --- the registry value, as text (pure, so it can be tested anywhere) ------
//
// Windows keeps one string per executable under
// HKCU\Software\Microsoft\DirectX\UserGpuPreferences, holding several
// "Name=Value;" settings ("GpuPreference=2;" next to e.g. "VRROptimizeEnable=0;").
// Only GpuPreference is Harpia's to touch; the rest are kept as they are.

inline GpuPreference gpuPreferenceIn(const QString &value)
{
	for (const QString &part : value.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
		const QString p = part.trimmed();
		if (!p.startsWith(QLatin1String("GpuPreference="), Qt::CaseInsensitive))
			continue;
		const int n = p.mid(int(sizeof("GpuPreference=") - 1)).trimmed().toInt();
		return n == 1 ? GpuPreference::PowerSaving
		       : n == 2 ? GpuPreference::HighPerformance
				: GpuPreference::Automatic;
	}
	return GpuPreference::Automatic;
}

// The value with `p` written in. Automatic removes the entry -- Windows reads
// a missing one as "Let Windows decide" -- so an empty result means the
// registry value can go altogether.
inline QString withGpuPreference(const QString &value, GpuPreference p)
{
	QStringList keep;
	for (const QString &part : value.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
		const QString t = part.trimmed();
		if (!t.isEmpty() && !t.startsWith(QLatin1String("GpuPreference="), Qt::CaseInsensitive))
			keep << t;
	}
	if (p != GpuPreference::Automatic)
		keep << QStringLiteral("GpuPreference=%1").arg(int(p));
	return keep.isEmpty() ? QString() : keep.join(QLatin1Char(';')) + QLatin1Char(';');
}

inline const char *gpuPreferenceName(GpuPreference p)
{
	switch (p) {
	case GpuPreference::PowerSaving: return "Power saving (integrated graphics)";
	case GpuPreference::HighPerformance: return "High performance (dedicated graphics)";
	default: return "Automatic (Windows decides)";
	}
}

} // namespace harpia::os_integration
