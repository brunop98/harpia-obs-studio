// "Reset to defaults" must mean the page you are on.
//
// The Developer Panel has nine tabs and had one Reset button, which reset ALL
// of them. So finding one colour you disliked and putting it back cost you
// every other number you had tuned -- in a panel whose entire purpose is
// tuning numbers by hand. Worse, nothing said so: the button read "Reset to
// defaults" with no hint of a scope.
//
// The property is a negative one, which is the kind that rots quietly: after
// resetting tab A, tab B still holds what you put there. So the test tunes
// SEVERAL sections away from their defaults, resets exactly one, and checks
// both halves -- that one changed, and that the others did not.
#include "editor/DevPanel.hpp"
#include "editor/EditorWidgets.hpp"
#include "editor/TrackEditor.hpp"
#include "editor/VoiceoverTrack.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static QPushButton *buttonStarting(QWidget *w, const QString &prefix)
{
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text().startsWith(prefix))
			return b;
	return nullptr;
}

static bool selectTab(QTabWidget *tabs, const QString &title)
{
	for (int i = 0; i < tabs->count(); ++i)
		if (tabs->tabText(i) == title) {
			tabs->setCurrentIndex(i);
			return true;
		}
	return false;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	// A scratch org/app so the panel's QSettings never touch a real install.
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaTest"));
	QCoreApplication::setApplicationName(QStringLiteral("DevPanelResetTest"));
	QSettings().clear();

	Timeline timeline;
	TrackEditor tracks;
	VoiceoverTrack voice;
	PreviewCanvas preview;
	TimelineView full;
	DevPanel panel(&timeline, &tracks, &voice, &preview, &full);

	QTabWidget *tabs = panel.findChild<QTabWidget *>();
	ok(tabs != nullptr, "the panel has tabs");
	if (!tabs)
		return 1;
	std::printf("     %d tabs\n", tabs->count());

	// Tune three separate sections away from their shipped values, through the
	// widgets, which is what the panel writes to.
	const TimelineLayoutParams tlDefault;
	const TrackLayoutParams trDefault;
	const VoiceoverLayoutParams voDefault;
	{
		TimelineLayoutParams tl = timeline.layoutParams();
		tl.barH = tlDefault.barH + 21;
		timeline.setLayoutParams(tl);
		TrackLayoutParams tr = tracks.layoutParams();
		tr.margin = trDefault.margin + 17;
		tracks.setLayoutParams(tr);
		VoiceoverLayoutParams vo = voice.layoutParams();
		vo.trackH = voDefault.trackH + 13;
		voice.setLayoutParams(vo);
	}
	// Rebuilt so the boxes show the tuned values -- the panel reads them once,
	// at construction.
	DevPanel p2(&timeline, &tracks, &voice, &preview, &full);
	tabs = p2.findChild<QTabWidget *>();
	p2.show();

	std::printf("\n-- the button says which page it means --\n");
	{
		ok(selectTab(tabs, QStringLiteral("Trim")), "there is a Trim tab");
		QPushButton *b = buttonStarting(&p2, QStringLiteral("Reset"));
		ok(b && b->text().contains(QStringLiteral("Trim")),
		   "on the Trim tab the reset button names Trim");
		ok(selectTab(tabs, QStringLiteral("Voiceover")), "and a Voiceover tab");
		b = buttonStarting(&p2, QStringLiteral("Reset"));
		std::printf("     button now reads: %s\n", b ? qUtf8Printable(b->text()) : "(none)");
		ok(b && b->text().contains(QStringLiteral("Voiceover")),
		   "switching tab renames it, so it can never mean the wrong page");
		ok(buttonStarting(&p2, QStringLiteral("Reset all")) != nullptr,
		   "and resetting everything is still possible, just no longer the default");
	}

	std::printf("\n-- resetting one tab leaves the others alone --\n");
	{
		// Before: all three are away from their defaults.
		ok(timeline.layoutParams().barH != tlDefault.barH, "Trim starts tuned");
		ok(tracks.layoutParams().margin != trDefault.margin, "Multi-Cut starts tuned");
		ok(voice.layoutParams().trackH != voDefault.trackH, "Voiceover starts tuned");

		selectTab(tabs, QStringLiteral("Trim"));
		buttonStarting(&p2, QStringLiteral("Reset “"))->click();
		QApplication::processEvents();

		std::printf("     after resetting Trim: trim barH=%d (default %d), "
			    "multicut margin=%d (tuned %d), voice trackH=%d (tuned %d)\n",
			    timeline.layoutParams().barH, tlDefault.barH, tracks.layoutParams().margin,
			    trDefault.margin + 17, voice.layoutParams().trackH, voDefault.trackH + 13);

		ok(timeline.layoutParams().barH == tlDefault.barH, "Trim went back to its default");
		ok(tracks.layoutParams().margin == trDefault.margin + 17,
		   "and Multi-Cut kept every one of its numbers");
		ok(voice.layoutParams().trackH == voDefault.trackH + 13,
		   "as did Voiceover -- which is the whole point");
	}

	std::printf("\n-- and a different tab resets a different thing --\n");
	{
		// The check above would also pass if the button did nothing at all to
		// the other tabs because it did nothing to ANYTHING. So reset a second,
		// different page and watch that one move instead.
		selectTab(tabs, QStringLiteral("Voiceover"));
		buttonStarting(&p2, QStringLiteral("Reset “"))->click();
		QApplication::processEvents();
		ok(voice.layoutParams().trackH == voDefault.trackH, "Voiceover went back");
		ok(tracks.layoutParams().margin == trDefault.margin + 17,
		   "CONTROL: and Multi-Cut, untouched by either reset, is still tuned");
	}

	std::printf("\n-- reset all really does mean all --\n");
	{
		ok(tracks.layoutParams().margin != trDefault.margin,
		   "Multi-Cut is still the odd one out going in");
		buttonStarting(&p2, QStringLiteral("Reset all"))->click();
		QApplication::processEvents();
		ok(tracks.layoutParams().margin == trDefault.margin, "and now it is back too");
		ok(timeline.layoutParams().barH == tlDefault.barH, "with Trim still at its default");
	}

	QSettings().clear();
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
