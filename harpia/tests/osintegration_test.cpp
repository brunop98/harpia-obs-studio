// How Harpia presents itself to the OS (platform/OsIntegration.hpp).
//
//   * The graphics-card preference is one setting inside a string Windows
//     shares with others ("VRROptimizeEnable=0;GpuPreference=2;"): reading
//     finds it, writing replaces only it, and Automatic removes it (an empty
//     result means the registry value can go).
//   * Off Windows there is nothing to set: no startup line, no preference.
//   * The System page shows what is saved, saves a new pick, says a restart
//     applies it -- and when saving fails, goes back to what is really in
//     effect instead of showing a choice that did not stick.
#include "platform/OsIntegration.hpp"
#include "ui/SystemSettingsWidget.hpp"

#include <QApplication>
#include <QComboBox>
#include <QLabel>

#include <cstdio>

using namespace harpia;
using os_integration::GpuPreference;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	using os_integration::gpuPreferenceIn;
	using os_integration::withGpuPreference;

	std::printf("\n-- reading the registry value --\n");
	ok(gpuPreferenceIn(QString()) == GpuPreference::Automatic, "nothing saved: Automatic");
	ok(gpuPreferenceIn(QStringLiteral("GpuPreference=2;")) == GpuPreference::HighPerformance, "2 is high performance");
	ok(gpuPreferenceIn(QStringLiteral("VRROptimizeEnable=0;GpuPreference=1;")) == GpuPreference::PowerSaving,
	   "1 is power saving, found among other settings");
	ok(gpuPreferenceIn(QStringLiteral("gpupreference=2")) == GpuPreference::HighPerformance,
	   "any letter case, no trailing semicolon");
	ok(gpuPreferenceIn(QStringLiteral("GpuPreference=0;")) == GpuPreference::Automatic, "0 is Windows decides");
	ok(gpuPreferenceIn(QStringLiteral("SwapEffectUpgradeEnable=1;")) == GpuPreference::Automatic,
	   "other settings only: Automatic");

	std::printf("\n-- writing it --\n");
	ok(withGpuPreference(QString(), GpuPreference::HighPerformance) == QStringLiteral("GpuPreference=2;"),
	   "a new value");
	const QString mixed = QStringLiteral("VRROptimizeEnable=0;GpuPreference=1;");
	const QString high = withGpuPreference(mixed, GpuPreference::HighPerformance);
	std::printf("     %s -> %s\n", qPrintable(mixed), qPrintable(high));
	ok(high == QStringLiteral("VRROptimizeEnable=0;GpuPreference=2;"), "only GpuPreference changes");
	ok(withGpuPreference(high, GpuPreference::Automatic) == QStringLiteral("VRROptimizeEnable=0;"),
	   "Automatic removes it and keeps the rest");
	ok(withGpuPreference(QStringLiteral("GpuPreference=2;"), GpuPreference::Automatic).isEmpty(),
	   "and with nothing else left the value can go");
	bool roundTrip = true;
	for (GpuPreference p : {GpuPreference::Automatic, GpuPreference::PowerSaving, GpuPreference::HighPerformance})
		roundTrip = roundTrip && gpuPreferenceIn(withGpuPreference(mixed, p)) == p;
	ok(roundTrip, "what is written reads back, for every choice");

	std::printf("\n-- off Windows --\n");
#ifndef Q_OS_WIN
	ok(os_integration::applyAtStartup().isEmpty(), "nothing to apply at startup");
	ok(!os_integration::gpuPreferenceSupported(), "no graphics preference (the System page stays hidden)");
	ok(os_integration::gpuPreference() == GpuPreference::Automatic &&
		   !os_integration::setGpuPreference(GpuPreference::HighPerformance),
	   "reads Automatic, and saving says it did not");
#endif

	std::printf("\n-- the System page --\n");
	{
		GpuPreference stored = GpuPreference::HighPerformance;
		int writes = 0;
		bool failNext = false;
		SystemSettingsWidget page(
			nullptr, [&]() { return stored; },
			[&](GpuPreference p) {
				++writes;
				if (failNext)
					return false;
				stored = p;
				return true;
			});
		QComboBox *combo = page.gpuCombo();
		ok(combo->count() == 3, "three choices");
		ok(GpuPreference(combo->currentData().toInt()) == GpuPreference::HighPerformance, "shows what is saved");

		const int powerRow = combo->findData(int(GpuPreference::PowerSaving));
		combo->setCurrentIndex(powerRow);
		emit combo->activated(powerRow);
		ok(writes == 1 && stored == GpuPreference::PowerSaving, "picking one saves it");
		std::printf("     status: %s\n", qPrintable(page.statusLabel()->text()));
		ok(page.statusLabel()->text().contains(QStringLiteral("Restart")), "and says a restart applies it");

		emit combo->activated(powerRow);
		ok(writes == 1, "picking the same one again writes nothing");

		failNext = true;
		const int autoRow = combo->findData(int(GpuPreference::Automatic));
		combo->setCurrentIndex(autoRow);
		emit combo->activated(autoRow);
		ok(writes == 2 && stored == GpuPreference::PowerSaving, "a failed save changes nothing");
		ok(GpuPreference(combo->currentData().toInt()) == GpuPreference::PowerSaving,
		   "and the box goes back to what is really in effect");
		ok(page.statusLabel()->text().contains(QStringLiteral("Could not")), "saying so");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
