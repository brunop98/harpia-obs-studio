// Does the Display dropdown point at the monitor the recording area lives on?
//
// Those are two different lists. The dropdown and the capture both index OBS's
// monitor list; the region overlay, the countdown, the screen border and the
// output size all come from a QScreen. Nothing makes the two orders agree on
// Windows, so a display chosen in the dropdown and a display the overlay opens
// on can be different physical monitors -- and when they are, the region is
// drawn and clamped against the wrong geometry and the file comes out at the
// wrong resolution, with nothing on screen admitting it.
//
// The matching needs Win32 to gather its facts, but the decision does not, and
// the decision is what can be wrong. These are the cases it has to get right,
// and the one it has to REFUSE: an unsure match is worse than none, because the
// caller can fall back and log, whereas a confident wrong screen is silent.
#include "ui/MonitorMatch.hpp"

#include <QCoreApplication>

#include <cstdio>

using namespace harpia;
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
	QCoreApplication app(argc, argv);

	std::printf("\n-- the dropdown's index belongs to OBS's list --\n");
	{
		// The bug this file was written for. monitorIndex addresses the OBS
		// monitor list, and the recording uses it that way. The overlay used
		// to range-check the same number against Qt's screen count, so on a
		// machine where OBS enumerates more displays than Qt does, picking the
		// last display quietly became 0 for the overlay ONLY -- the dropdown
		// said display 3, the recording captured display 3, and the region sat
		// on display 1.
		ok(clampMonitorIndex(2, 3) == 2, "a valid OBS index survives");
		std::printf("     OBS sees 3 displays, index 2 -> %d\n", clampMonitorIndex(2, 3));
		ok(clampMonitorIndex(0, 1) == 0, "so does the only one there is");
		// Genuinely out of range still has to land somewhere real.
		ok(clampMonitorIndex(5, 3) == 0, "an index past the end falls back to the first");
		ok(clampMonitorIndex(-1, 3) == 0, "as does a negative one");
		ok(clampMonitorIndex(2, 0) == 0, "and an empty list gives the first, not a crash");
	}

	std::printf("\n-- Qt 5 names screens by their GDI name --\n");
	{
		const QVector<QtScreenDesc> screens = {
			{QStringLiteral("\\\\.\\DISPLAY1"), QPoint(0, 0)},
			{QStringLiteral("\\\\.\\DISPLAY2"), QPoint(1920, 0)},
			{QStringLiteral("\\\\.\\DISPLAY3"), QPoint(3840, 0)},
		};
		const QVector<GdiDisplayDesc> all = {
			{QStringLiteral("\\\\.\\DISPLAY1"), QStringLiteral("Dell"), QPoint(0, 0)},
			{QStringLiteral("\\\\.\\DISPLAY2"), QStringLiteral("LG"), QPoint(1920, 0)},
			{QStringLiteral("\\\\.\\DISPLAY3"), QStringLiteral("Asus"), QPoint(3840, 0)},
		};
		const int hit = matchQtScreen(screens, all[1], all);
		std::printf("     DISPLAY2 -> screen %d via %s\n", hit, lastMatchMethod());
		ok(hit == 1, "the second display maps to the second screen");
		ok(matchQtScreen(screens, all[2], all) == 2, "and the third to the third");

		// The prefix is not always carried on both sides.
		const QVector<QtScreenDesc> bare = {{QStringLiteral("DISPLAY1"), QPoint(0, 0)},
						    {QStringLiteral("DISPLAY2"), QPoint(1920, 0)},
						    {QStringLiteral("DISPLAY3"), QPoint(3840, 0)}};
		ok(matchQtScreen(bare, all[1], all) == 1, "with or without the \\\\.\\ prefix");
	}

	std::printf("\n-- Qt 6 names them by the monitor's friendly name --\n");
	{
		const QVector<QtScreenDesc> screens = {
			{QStringLiteral("DELL U2720Q"), QPoint(0, 0)},
			{QStringLiteral("LG HDR 4K"), QPoint(2560, 0)},
		};
		const QVector<GdiDisplayDesc> all = {
			{QStringLiteral("\\\\.\\DISPLAY1"), QStringLiteral("DELL U2720Q"), QPoint(0, 0)},
			{QStringLiteral("\\\\.\\DISPLAY2"), QStringLiteral("LG HDR 4K"), QPoint(2560, 0)},
		};
		const int hit = matchQtScreen(screens, all[1], all);
		std::printf("     'LG HDR 4K' -> screen %d via %s\n", hit, lastMatchMethod());
		ok(hit == 1, "a friendly name identifies the screen");
	}

	std::printf("\n-- two identical monitors: the name is not evidence --\n");
	{
		// The case that makes "just match the name" wrong. A matched pair
		// reports the SAME friendly name, so a name match picks one of them at
		// random -- half the time the overlay opens on the other monitor. It
		// has to fall through to position instead.
		const QVector<QtScreenDesc> screens = {
			{QStringLiteral("SMT22A550"), QPoint(0, 0)},
			{QStringLiteral("SMT22A550"), QPoint(1920, 0)},
		};
		const QVector<GdiDisplayDesc> all = {
			{QStringLiteral("\\\\.\\DISPLAY1"), QStringLiteral("SMT22A550"), QPoint(0, 0)},
			{QStringLiteral("\\\\.\\DISPLAY2"), QStringLiteral("SMT22A550"), QPoint(1920, 0)},
		};
		const int hit = matchQtScreen(screens, all[1], all);
		std::printf("     the right-hand twin -> screen %d via %s\n", hit, lastMatchMethod());
		ok(hit == 1, "the right-hand twin resolves to the right-hand screen");
		ok(QString::fromLatin1(lastMatchMethod()) != QStringLiteral("friendly name"),
		   "and NOT by the name they both share");
		ok(matchQtScreen(screens, all[0], all) == 0, "the left-hand one likewise");
	}

	std::printf("\n-- arrangement survives mixed DPI and odd ordering --\n");
	{
		// Qt's logical coordinates are not Windows' native ones when the
		// displays scale differently, but the left-to-right ORDER is the same,
		// which is the only thing the rank depends on.
		const QVector<QtScreenDesc> screens = {
			{QStringLiteral("?"), QPoint(0, 0)},     // 150%: 2560 native -> 1707
			{QStringLiteral("?"), QPoint(1707, 0)},  // 100%
		};
		const QVector<GdiDisplayDesc> all = {
			{QStringLiteral("\\\\.\\DISPLAY1"), QString(), QPoint(0, 0)},
			{QStringLiteral("\\\\.\\DISPLAY2"), QString(), QPoint(2560, 0)},
		};
		ok(matchQtScreen(screens, all[1], all) == 1, "the right-hand display maps right");

		// And when neither list is in left-to-right order to begin with.
		const QVector<QtScreenDesc> shuffled = {{QStringLiteral("?"), QPoint(1920, 0)},
							{QStringLiteral("?"), QPoint(0, 0)}};
		const QVector<GdiDisplayDesc> shuffledGdi = {
			{QStringLiteral("\\\\.\\DISPLAY9"), QString(), QPoint(1920, 0)},
			{QStringLiteral("\\\\.\\DISPLAY1"), QString(), QPoint(0, 0)},
		};
		const int hit = matchQtScreen(shuffled, shuffledGdi[0], shuffledGdi);
		std::printf("     the display at x=1920 -> screen %d (also at x=1920)\n", hit);
		ok(hit == 0, "position decides, not the order either list happens to be in");
	}

	std::printf("\n-- when it cannot tell, it says so --\n");
	{
		// -1 is the honest answer, and the caller logs and falls back. A guess
		// here is invisible: the overlay simply appears on the wrong monitor
		// and nothing anywhere says why.
		const QVector<QtScreenDesc> screens = {{QStringLiteral("?"), QPoint(0, 0)},
						       {QStringLiteral("?"), QPoint(1920, 0)}};
		// Three Windows displays, two Qt screens: the ranks address different
		// sets, so matching them up would be a coincidence.
		const QVector<GdiDisplayDesc> all = {
			{QStringLiteral("\\\\.\\DISPLAY1"), QString(), QPoint(0, 0)},
			{QStringLiteral("\\\\.\\DISPLAY2"), QString(), QPoint(1920, 0)},
			{QStringLiteral("\\\\.\\DISPLAY3"), QString(), QPoint(3840, 0)},
		};
		ok(matchQtScreen(screens, all[2], all) == -1,
		   "mismatched display counts refuse the arrangement guess");

		const QVector<GdiDisplayDesc> two = {all[0], all[1]};
		ok(matchQtScreen({}, two[0], two) == -1, "no screens, no match");
		const GdiDisplayDesc nameless{QString(), QString(), QPoint(0, 0)};
		ok(matchQtScreen(screens, nameless, two) == -1, "no device name, no match");
		// A display that is not in the list it is being ranked against.
		const GdiDisplayDesc stranger{QStringLiteral("\\\\.\\DISPLAY7"), QString(),
					      QPoint(99, 0)};
		ok(matchQtScreen(screens, stranger, two) == -1, "an unknown display is not ranked");
	}

	std::printf("\n-- the single-monitor case, which is nearly everyone --\n");
	{
		const QVector<QtScreenDesc> screens = {{QStringLiteral("BenQ GW2480"), QPoint(0, 0)}};
		const QVector<GdiDisplayDesc> all = {
			{QStringLiteral("\\\\.\\DISPLAY1"), QStringLiteral("BenQ GW2480"), QPoint(0, 0)}};
		ok(matchQtScreen(screens, all[0], all) == 0, "one display, one screen, no drama");
		ok(clampMonitorIndex(0, 1) == 0, "and the index needs no fixing");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
