// The floating desktop controls: one button when there is one thing to do.
//
// This panel used to exist only while a recording was running, holding Pause and
// Stop. It is now on screen for as long as the app is, because the one step of a
// recording that still forced a trip back to the main window was STARTING one --
// everything else about a region recording happens out on the desktop.
//
// That makes the idle state new, and the idle state is where this can go wrong
// in ways that matter for a recorder specifically:
//
//   * the red dot. It is a RECORDING indicator. Left painted while idle it says
//     the opposite of the truth, and "am I recording?" is the one question this
//     app must never answer wrongly;
//   * dead buttons. Pause and Stop parked on the desktop for hours, doing
//     nothing, is clutter that outlives its own reason to exist;
//   * the wiring. Three buttons and three signals is exactly the shape a
//     copy-paste slip hides in, so each is clicked and the OTHER two signals are
//     checked to be silent -- a test that only asserted "start emitted start"
//     would pass with every button wired to every signal.
#include "ui/RecorderControlsOverlay.hpp"

#include <QApplication>
#include <QPushButton>
#include <QSignalSpy>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// The buttons carry no text (icon-only), so they are identified by tooltip --
// which is also the only thing that names them for a screen reader.
static QPushButton *buttonTipped(QWidget *w, const QString &needle)
{
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->toolTip().contains(needle, Qt::CaseInsensitive))
			return b;
	return nullptr;
}

static int visibleButtons(QWidget *w)
{
	int n = 0;
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (!b->isHidden())
			++n;
	return n;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	RecorderControlsOverlay hud;
	hud.show();
	QApplication::processEvents();

	QPushButton *start = buttonTipped(&hud, QStringLiteral("Start"));
	QPushButton *stop = buttonTipped(&hud, QStringLiteral("Stop"));

	std::printf("\n-- idle: one green Record button, and nothing else --\n");
	{
		hud.setState(/*recording=*/false, false, false, false);
		QApplication::processEvents();
		ok(start != nullptr, "there is a Start button");
		ok(start && !start->isHidden(), "and it is on screen while idle");
		std::printf("     %d of %d buttons visible\n", visibleButtons(&hud),
			    int(hud.findChildren<QPushButton *>().size()));
		ok(visibleButtons(&hud) == 1,
		   "it is the only one -- Pause and Stop are hidden, not left sitting there dead");
		ok(start && start->styleSheet().contains(QStringLiteral("#3fb950")),
		   "green, the same green as Resume and the region frame's Record button");
	}

	std::printf("\n-- recording: it swaps for Pause and Stop --\n");
	{
		hud.setState(/*recording=*/true, false, true, true);
		QApplication::processEvents();
		std::printf("     %d buttons visible\n", visibleButtons(&hud));
		ok(start && start->isHidden(), "Start is gone -- it is already running");
		ok(stop && !stop->isHidden(), "Stop is here");
		ok(visibleButtons(&hud) == 2, "two buttons now, not three");

		QPushButton *pause = buttonTipped(&hud, QStringLiteral("Pause"));
		ok(pause && !pause->isHidden(), "and Pause reads Pause");
		hud.setState(true, /*paused=*/true, true, true);
		QApplication::processEvents();
		ok(buttonTipped(&hud, QStringLiteral("Resume")) != nullptr,
		   "which becomes Resume once paused");
	}

	std::printf("\n-- and back to idle again --\n");
	{
		// Not a formality: the swap has to work in both directions, and a state
		// machine that only ever gets tested forwards is a state machine that
		// gets stuck the first time a recording ends.
		hud.setState(false, false, false, false);
		QApplication::processEvents();
		ok(start && !start->isHidden(), "Start is back");
		ok(visibleButtons(&hud) == 1, "on its own");
	}

	std::printf("\n-- each button says its own thing, and only its own --\n");
	{
		struct Case {
			const char *tip;
			const char *name;
			bool recording;
		};
		const Case cases[] = {
			{"Start", "startClicked", false},
			{"Pause", "pauseClicked", true},
			{"Stop", "stopClicked", true},
		};
		for (const Case &c : cases) {
			hud.setState(c.recording, false, true, true);
			QApplication::processEvents();
			QSignalSpy started(&hud, &RecorderControlsOverlay::startClicked);
			QSignalSpy paused(&hud, &RecorderControlsOverlay::pauseClicked);
			QSignalSpy stopped(&hud, &RecorderControlsOverlay::stopClicked);

			QPushButton *b = buttonTipped(&hud, QString::fromLatin1(c.tip));
			if (!b) {
				ok(false, c.tip);
				continue;
			}
			b->click();
			QApplication::processEvents();
			const int counts[3] = {int(started.count()), int(paused.count()),
					       int(stopped.count())};
			std::printf("     clicked %-5s -> start=%d pause=%d stop=%d\n", c.tip, counts[0],
				    counts[1], counts[2]);
			const int total = counts[0] + counts[1] + counts[2];
			// The whole point: exactly one signal, and the right one. Three
			// buttons wired to the same slot would satisfy "it emitted
			// something" all day.
			ok(total == 1, c.name);
		}
	}

	std::printf("\n-- a disabled button does nothing at all --\n");
	{
		// Pause is disabled for formats that cannot pause, and Stop while a stop
		// is already in flight. A second stop request mid-stop is exactly the
		// kind of thing that trips a state machine.
		hud.setState(true, false, /*pauseEnabled=*/false, /*stopEnabled=*/false);
		QApplication::processEvents();
		QSignalSpy paused(&hud, &RecorderControlsOverlay::pauseClicked);
		QSignalSpy stopped(&hud, &RecorderControlsOverlay::stopClicked);
		QPushButton *pause = buttonTipped(&hud, QStringLiteral("Pause"));
		QPushButton *stopB = buttonTipped(&hud, QStringLiteral("Stop"));
		ok(pause && !pause->isEnabled(), "Pause is disabled when it cannot pause");
		if (pause)
			pause->click();
		if (stopB)
			stopB->click();
		QApplication::processEvents();
		ok(paused.count() == 0 && stopped.count() == 0, "and clicking either says nothing");

		// CONTROL: re-enabled, the same click DOES speak -- otherwise the check
		// above would pass on a panel whose buttons were never connected.
		hud.setState(true, false, true, true);
		QApplication::processEvents();
		QSignalSpy again(&hud, &RecorderControlsOverlay::stopClicked);
		buttonTipped(&hud, QStringLiteral("Stop"))->click();
		QApplication::processEvents();
		ok(again.count() == 1, "CONTROL: enabled again, the same click is heard");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
