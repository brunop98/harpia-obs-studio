// Hit-testing must not get quadratically slower as a project grows.
//
// Every "where is this on screen" question goes through msToX(), which asks
// spanMs(), which walked every clip on every track. A single mouse-move asks
// that question hundreds of times -- once per clip considered -- so the cost of
// moving the pointer grew with the SQUARE of the clip count. Measured on 1200
// clips before the fix: 14.4 ms per pointer motion, with no repaint involved.
// Nothing crashed and nothing looked wrong; the editor just felt bad.
//
// SpanGuard already existed to cache that measurement for the duration of a
// paint. Two things were wrong with how it was used:
//
//   - the hover and press paths did not hold one at all;
//   - it was not re-entrant. A guard taken by a helper restored "measure
//     again" on the way out, switching OFF a guard its caller was still
//     holding. That is a slowdown with no symptom whatsoever -- the numbers
//     stay correct, and only a profile shows it.
//
// A wall-clock threshold would be flaky on a loaded machine, so the assertion
// here is about SHAPE: quadruple the clips and the cost must not quadruple in
// turn. Quadratic would be 16x.
#include "editor/timeline/TimelineModel.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>

#include <algorithm>
#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static TimelineView *build(int perTrack)
{
	auto *tv = new TimelineView;
	tv->resize(1400, 420);
	TimelineModel m;
	for (int k = 0; k < 4; ++k) {
		TlTrack t;
		t.kind = k < 3 ? TlTrack::Kind::Video : TlTrack::Kind::Audio;
		for (int i = 0; i < perTrack; ++i) {
			TlClip c;
			c.type = TlClip::Type::Video;
			c.sourceId = 1 + k;
			c.srcStartMs = i * 1000;
			c.srcEndMs = (i + 1) * 1000;
			c.outStartMs = qint64(i) * 1000;
			if (t.kind == TlTrack::Kind::Audio)
				c.peaks.fill(0.5f, 600);
			t.clips.append(c);
		}
		m.tracks.append(t);
	}
	tv->setModel(m);
	tv->show();
	QApplication::processEvents();
	return tv;
}

// Nanoseconds per idle mouse-move, with no repaint in the loop.
static double hoverCost(TimelineView *tv, int n)
{
	// Warm: the first event through builds fonts and caches.
	for (int i = 0; i < 20; ++i) {
		const QPoint p(300 + (i % 500), 80 + (i % 200));
		QMouseEvent e(QEvent::MouseMove, QPointF(p), tv->mapToGlobal(p), Qt::NoButton,
			      Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(tv, &e);
	}
	QElapsedTimer t;
	t.start();
	for (int i = 0; i < n; ++i) {
		const QPoint p(300 + (i % 500), 80 + (i % 200));
		QMouseEvent e(QEvent::MouseMove, QPointF(p), tv->mapToGlobal(p), Qt::NoButton,
			      Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(tv, &e);
	}
	return double(t.nsecsElapsed()) / n;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- the span cache survives a nested guard --\n");
	{
		// Directly, because the consequence is invisible: with a guard held,
		// spanMs() must keep answering from the cache even after an inner guard
		// has come and gone.
		TimelineView *tv = build(40);
		const qint64 real = tv->spanMsForTest();
		{
			const TimelineView::SpanGuardForTest outer(tv);
			ok(tv->spanCacheForTest() == real, "an outer guard caches the span");
			{
				const TimelineView::SpanGuardForTest inner(tv);
				ok(tv->spanCacheForTest() == real, "and so does an inner one");
			}
			// The bug. Before this, the inner guard's destructor wrote -1 and
			// everything the caller did afterwards went back to walking every
			// clip -- silently, and only in the paths that nest.
			ok(tv->spanCacheForTest() == real,
			   "and the cache is STILL live after the inner one is gone");
		}
		ok(tv->spanCacheForTest() == -1, "the outermost guard does release it");
		tv->deleteLater();
	}

	std::printf("\n-- hovering does not get quadratically dearer --\n");
	{
		TimelineView *small = build(50);
		TimelineView *big = build(200); // 4x the clips
		const double a = hoverCost(small, 400);
		const double b = hoverCost(big, 400);
		const double ratio = b / std::max(1.0, a);
		std::printf("     50/track: %8.0f ns    200/track: %8.0f ns    ratio %.2f\n", a, b,
			    ratio);
		// Quadratic in the clip count would be about 16x for 4x the clips; the
		// pre-fix code measured close to that. Linear-ish is about 4x, and the
		// bound is set above it with room for a noisy machine -- the point is to
		// catch a return to quadratic, not to police small changes.
		ok(ratio < 8.0, "four times the clips costs far less than sixteen times the work");
		// And in absolute terms it has to stay off the critical path. Very
		// loose, because a shared CI box can be slow, but 1 ms per pointer
		// motion is where an editor starts to feel sticky.
		std::printf("     absolute: %.3f ms per pointer motion at 800 clips\n", b / 1e6);
		ok(b < 1e6, "a mouse-move over a busy timeline stays well under a millisecond");
		small->deleteLater();
		big->deleteLater();
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
