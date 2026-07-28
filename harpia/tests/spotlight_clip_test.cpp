// An Inverse Selection effect clip must actually dim something.
//
// It never did. FxSpec carried a SpotlightSpec and the compositor applied it,
// and the Inspector said "edit the areas in the section below" — but that
// section wrote to the PROJECT's spotlight, a different object. Nothing in the
// program ever wrote fx.spot, so the clip was permanently inert and the panel
// pointed you somewhere else. This drives the real panel and reads the real
// preview.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"
#include "editor/EditorWidgets.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QThread>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static void settle(int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms) {
		QApplication::processEvents();
		QThread::msleep(5);
	}
}

// setPlayhead() is a programmatic setter: it emits nothing, so the preview goes
// on showing whatever it had — at startup a raw SOURCE frame that never went
// through the compositor at all. Clicking the ruler is what a user does and
// what actually forces a composite. Measuring before one is how an earlier
// version of this test read a stale frame as its baseline and believed it.
static void scrubTo(TimelineView *tv, qint64 ms)
{
	const QPoint p(tv->xForMs(ms), 14);
	QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), tv->mapToGlobal(p), Qt::LeftButton,
			  Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(tv, &press);
	QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(p), tv->mapToGlobal(p), Qt::LeftButton,
			Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(tv, &rel);
}

// A spotlight leaves the middle alone and darkens the rest, so a single number
// for the whole frame would let "it all went black" pass as success. Measure a
// centre column and an edge column and require the centre to survive.
struct Probe {
	double centre = -1, edge = -1;
};
static Probe probe(PreviewCanvas *pc)
{
	const QImage f = pc->currentFrame();
	Probe p;
	if (f.isNull())
		return p;
	auto band = [&](double x0, double x1) {
		double s = 0;
		int n = 0;
		for (int y = f.height() * 2 / 5; y < f.height() * 3 / 5; ++y)
			for (int x = int(x0 * f.width()); x < int(x1 * f.width()); ++x) {
				s += QColor(f.pixel(x, y)).lightness();
				++n;
			}
		return n ? s / n : -1;
	};
	p.centre = band(0.46, 0.54); // inside a 0.4-wide centred mask
	p.edge = band(0.01, 0.09);   // well outside it
	return p;
}

static QPushButton *button(QWidget *w, const QString &text)
{
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text() == text)
			return b;
	return nullptr;
}

// Three checkboxes in this window say "Enabled" — the clip's, the effect's and
// the spotlight's. Find the one that sits under the Inverse Selection header
// rather than trusting the order findChildren happens to return.
static QCheckBox *spotlightEnabledBox(QWidget *w)
{
	for (QCheckBox *b : w->findChildren<QCheckBox *>()) {
		if (b->text() != QStringLiteral("Enabled"))
			continue;
		for (QWidget *a = b->parentWidget(); a; a = a->parentWidget())
			for (QLabel *l : a->findChildren<QLabel *>())
				if (l->text().startsWith(QStringLiteral("Inverse Selection")))
					return b;
	}
	return nullptr;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	VideoEditorWindow w(QStringLiteral(SRC_MEDIA));
	w.resize(1400, 900);
	w.show();
	QApplication::processEvents();
	QPushButton *full = button(&w, QStringLiteral("Full Editing"));
	ok(full != nullptr, "the Full Editing mode button is there");
	if (full) {
		full->click();
		settle(400);
	}

	TimelineView *tv = w.findChild<TimelineView *>();
	PreviewCanvas *pc = w.findChild<PreviewCanvas *>();
	ok(tv && pc, "the timeline and the preview exist");
	if (!tv || !pc)
		return 1;

	// Build on what the editor already seeded — the source clip on a video
	// track. That is a real, evenly lit frame; every attempt to synthesise one
	// out of a caption box gave a mostly-black picture with nothing to dim.
	TimelineModel m = tv->model();
	ok(!m.tracks.isEmpty() && !m.tracks[0].clips.isEmpty(),
	   "entering Full Editing seeds the source onto a track");
	if (m.tracks.isEmpty() || m.tracks[0].clips.isEmpty())
		return 1;

	TlTrack fxTrack;
	fxTrack.kind = TlTrack::Kind::Effect;
	fxTrack.name = QStringLiteral("FX");
	TlClip e;
	e.type = TlClip::Type::Effect;
	e.outStartMs = 0;
	e.srcStartMs = 0;
	e.srcEndMs = 1000; // deliberately SHORTER than the picture below it
	e.fx.type = FxType::InverseSelection;
	e.fx.enabled = true;
	fxTrack.clips.append(e);
	m.tracks.prepend(fxTrack); // index 0 is the top lane; an effect grades below it
	tv->setModel(m);
	scrubTo(tv, 500); // inside the effect clip, and forces the first composite
	tv->selectClip(0, 0);
	settle(700);

	const Probe plain = probe(pc);
	std::printf("     before: centre %.1f  edge %.1f\n", plain.centre, plain.edge);
	ok(plain.centre > 40 && plain.edge > 40, "the picture is bright enough to dim");

	QCheckBox *on = spotlightEnabledBox(&w);
	QPushButton *add = button(&w, QStringLiteral("Add"));
	ok(on && add, "the panel's Enabled box and Add button are reachable");
	if (!on || !add)
		return 1;

	on->click();
	add->click();
	settle(700);

	// The edit must have landed on the CLIP, not on the project-wide spec. These
	// two are the assertions that actually separate "fixed" from "broken", so
	// they run whatever happened to the selection — guarding them behind a
	// non-null selection is how the first negative control came back green on
	// everything that mattered.
	const TlClip *sel = tv->selectedClipPtr();
	const TlClip *fxClip = (!tv->model().tracks.isEmpty() && !tv->model().tracks[0].clips.isEmpty())
				       ? &tv->model().tracks[0].clips[0]
				       : nullptr;
	std::printf("     clip spot: %s   project masks=%d\n",
		    fxClip ? qPrintable(QStringLiteral("enabled=%1 masks=%2")
						.arg(int(fxClip->fx.spot.enabled))
						.arg(fxClip->fx.spot.masks.size()))
			   : "<gone>",
		    int(tv->model().spotlight.masks.size()));
	ok(sel != nullptr, "the effect clip is still selected");
	ok(fxClip && fxClip->fx.spot.active(), "the clip's own spotlight is now active");
	ok(tv->model().spotlight.masks.isEmpty(), "and the project-wide one was left alone");

	const Probe lit = probe(pc);
	std::printf("     after:  centre %.1f  edge %.1f\n", lit.centre, lit.edge);
	ok(lit.edge < plain.edge - 15, "outside the mask got darker");
	ok(lit.centre > plain.centre - 8, "inside it did not");
	ok(lit.centre > lit.edge + 20, "so it is a spotlight, not an overall fade");

	// The whole point of a clip rather than the project-wide spec: past its end
	// the dimming stops.
	scrubTo(tv, 2000); // the effect clip ended at 1000
	settle(700);
	const Probe after = probe(pc);
	std::printf("     past the clip's end: centre %.1f  edge %.1f\n", after.centre, after.edge);
	// Compare the edge against the CENTRE of the same frame, not against the
	// earlier one: a different moment has different content, so "brighter than
	// before" is satisfied by a still-dimmed edge over a brighter shot. Only
	// "the edge is no longer the dark one" actually means the dimming stopped.
	ok(after.edge > after.centre - 25, "beyond the clip the edge is no longer dimmed");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
