// During playback the playhead moves 30 times a second. Each track widget
// used to repaint itself whole for that -- every filmstrip tile, segment,
// label and waveform -- to move a 1-2 px line. Now only the band the line
// leaves and the band it enters are repainted, and the whole widget only
// when the line leaves the visible area.
//
// Pinned for the three widgets outside Full editing: the Multi-Cut track
// editor, the voiceover track and the Simple Trim bar. The Full-editing
// timeline already had this.
#include "editor/EditorWidgets.hpp"
#include "editor/TrackEditor.hpp"
#include "editor/VoiceoverTrack.hpp"

#include <QApplication>
#include <QPaintEvent>
#include <QTest>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// Records the union of what was repainted.
class PaintSpy : public QObject {
public:
	QRect painted;
	int events = 0;
	bool eventFilter(QObject *, QEvent *e) override
	{
		if (e->type() == QEvent::Paint) {
			painted |= static_cast<QPaintEvent *>(e)->rect();
			++events;
		}
		return false;
	}
	void reset()
	{
		painted = QRect();
		events = 0;
	}
};

// Move the playhead a little (one playback tick) and report how wide the
// repaint was, as a fraction of the widget.
template<class W, class Set>
static double tickWidth(W &w, PaintSpy &spy, Set set)
{
	QApplication::processEvents();
	spy.reset();
	set();
	QApplication::processEvents();
	return spy.events == 0 ? 0.0 : double(spy.painted.width()) / std::max(1, w.width());
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);

	std::printf("\n-- Multi-Cut track editor --\n");
	{
		TrackEditor te;
		te.resize(1000, 220);
		te.setDuration(60000);
		CutSegment a;
		a.srcEndMs = 20000;
		CutSegment b;
		b.srcStartMs = 30000;
		b.srcEndMs = 50000;
		te.setSegments({a, b});
		PaintSpy spy;
		te.installEventFilter(&spy);
		te.show();
		QTest::qWaitForWindowExposed(&te);
		te.setPlayhead(10000);
		const double f = tickWidth(te, spy, [&] { te.setPlayhead(10033); });
		std::printf("     one tick repainted %.1f%% of the width\n", f * 100.0);
		ok(spy.events > 0, "the move is repainted");
		ok(f > 0.0 && f < 0.05, "and only a thin band, not the whole widget");
		te.clearPlayhead();
		const double full = tickWidth(te, spy, [&] { te.setPlayhead(5000); });
		ok(full > 0.9, "the first placement after a clear repaints everything, as before");
	}

	std::printf("\n-- voiceover track --\n");
	{
		VoiceoverTrack vt;
		vt.resize(1000, 80);
		vt.setOutputDuration(60000);
		PaintSpy spy;
		vt.installEventFilter(&spy);
		vt.show();
		QTest::qWaitForWindowExposed(&vt);
		vt.setPlayhead(10000);
		const double f = tickWidth(vt, spy, [&] { vt.setPlayhead(10033); });
		std::printf("     one tick repainted %.1f%% of the width\n", f * 100.0);
		ok(spy.events > 0 && f > 0.0 && f < 0.05, "a playback tick repaints only a thin band");
	}

	std::printf("\n-- Simple Trim bar --\n");
	{
		Timeline tl;
		tl.resize(1000, 90);
		tl.setDuration(60000);
		PaintSpy spy;
		tl.installEventFilter(&spy);
		tl.show();
		QTest::qWaitForWindowExposed(&tl);
		tl.setPlayhead(10000);
		const double f = tickWidth(tl, spy, [&] { tl.setPlayhead(10033); });
		std::printf("     one tick repainted %.1f%% of the width\n", f * 100.0);
		ok(spy.events > 0 && f > 0.0 && f < 0.05, "a playback tick repaints only a thin band");
		const double none = tickWidth(tl, spy, [&] { tl.setPlayhead(10033); });
		ok(none == 0.0, "setting the same position again repaints nothing");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
