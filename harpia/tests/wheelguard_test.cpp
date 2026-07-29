// The wheel scrolls the Inspector; it never edits what is under the pointer.
//
// Qt's spin boxes, sliders and combo boxes all take the wheel as input. In a
// scrolling panel that is a trap: the pointer happens to be over a control
// while you scroll past it and a number changes that nobody decided to change.
// Worse, it is silent — there is nothing on screen to notice.
//
// What has to hold, and what each check is really guarding:
//
//   - a wheel over a slider or spin box in the Inspector leaves the value
//     alone (the bug);
//   - and still scrolls the panel, or the wheel would be dead over half the
//     Inspector, which is its own kind of broken;
//   - the panel's own scroll BAR keeps working, since it is a QAbstractSlider
//     too and a careless filter would kill it;
//   - the preview keeps its wheel-to-zoom, because it is not in the panel.
#include "editor/EditorWidgets.hpp"
#include "editor/ParamSlider.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>
#include <QWheelEvent>

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
static QPushButton *button(QWidget *w, const QString &text)
{
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text() == text)
			return b;
	return nullptr;
}

// A wheel notch delivered to the widget under the pointer, as the window system
// would. Sent to `target` directly so the test controls exactly who gets it.
static void wheel(QWidget *target, int notches = -3)
{
	const QPoint local(target->width() / 2, target->height() / 2);
	QWheelEvent e(QPointF(local), QPointF(target->mapToGlobal(local)), QPoint(0, 0),
		      QPoint(0, notches * 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
		      false);
	QApplication::sendEvent(target, &e);
	QApplication::processEvents();
}

static void buildOneClip(TimelineView *tv)
{
	TimelineModel m;
	TlTrack t;
	t.kind = TlTrack::Kind::Video;
	TlClip c;
	c.type = TlClip::Type::Text;
	c.srcStartMs = 0;
	c.srcEndMs = 4000;
	c.outStartMs = 0;
	c.text.text = QStringLiteral("wheel me");
	// A component with a slider to aim at.
	ComponentInstance ci;
	ci.typeId = QStringLiteral("harpia.blur");
	ci.instanceId = QStringLiteral("b");
	ci.props.insert(QStringLiteral("radius"), 0.4);
	c.components.append(ci);
	t.clips.append(c);
	m.tracks.append(t);
	tv->setModel(m);
	QApplication::processEvents();
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	VideoEditorWindow w(QStringLiteral(SRC_MEDIA));
	w.resize(1300, 900);
	w.show();
	QApplication::processEvents();
	if (QPushButton *full = button(&w, QStringLiteral("Full Editing"))) {
		full->click();
		settle(400);
	}
	// The Inspector pane starts collapsed, so its scroll area has no content to
	// scroll and every check below would be measuring a hidden panel.
	if (QPushButton *insp = button(&w, QStringLiteral("Inspector"))) {
		insp->click();
		settle(300);
	} else if (QPushButton *fx = button(&w, QStringLiteral("Effects"))) {
		fx->click();
		settle(300);
	}
	TimelineView *tv = w.findChild<TimelineView *>();
	ok(tv != nullptr, "the editor came up in Full editing");
	if (!tv)
		return 1;
	buildOneClip(tv);
	tv->selectClip(0, 0);
	settle(700);

	// The Inspector's scroll area is the one holding a ParamSlider.
	QScrollArea *panel = nullptr;
	for (QScrollArea *sa : w.findChildren<QScrollArea *>())
		if (!sa->findChildren<ParamSlider *>().isEmpty())
			panel = sa;
	ok(panel != nullptr, "found the Inspector's scroll area");
	if (!panel)
		return 1;

	QList<ParamSlider *> sliders = panel->findChildren<ParamSlider *>();
	std::printf("     sliders in the panel: %d\n", int(sliders.size()));
	ok(!sliders.isEmpty(), "with sliders in it");
	if (sliders.isEmpty())
		return 1;

	std::printf("\n-- the wheel over a value control changes nothing --\n");
	{
		ParamSlider *ps = sliders.first();
		const double before = ps->value();
		// Every inner control of the row, one at a time: the wheel lands on
		// whichever is under the pointer, and guarding only the outer widget
		// would leave the real ones exposed.
		for (QWidget *inner : ps->findChildren<QWidget *>())
			wheel(inner);
		wheel(ps);
		settle(200);
		std::printf("     value %.4f -> %.4f\n", before, ps->value());
		ok(std::abs(ps->value() - before) < 1e-9, "the slider kept its value");
	}
	{
		// And the bare spin boxes elsewhere in the Inspector, which are not
		// ParamSliders and would need guarding separately if this were done
		// per-widget rather than by type.
		QDoubleSpinBox *sb = nullptr;
		for (QDoubleSpinBox *b : panel->findChildren<QDoubleSpinBox *>())
			if (b->isEnabled() && !b->isHidden())
				sb = b;
		if (sb) {
			const double before = sb->value();
			wheel(sb);
			settle(150);
			std::printf("     spin box %.4f -> %.4f\n", before, sb->value());
			ok(std::abs(sb->value() - before) < 1e-9, "a plain spin box too");
		} else {
			std::printf("     (no visible spin box to try)\n");
		}
	}

	std::printf("\n-- and it scrolls the panel instead --\n");
	{
		// Force the panel to overflow. Offscreen at this window size the
		// Inspector happens to fit, and a scroll test against a panel with
		// nothing to scroll would pass for the wrong reason.
		panel->setFixedHeight(140);
		settle(200);
		QScrollBar *bar = panel->verticalScrollBar();
		std::printf("     scroll range 0..%d\n", bar->maximum());
		if (bar->maximum() <= 0) {
			// Nothing to scroll means this cannot be tested, and quietly
			// passing would be worse than saying so.
			std::printf("  FAIL the panel does not scroll, so this proves nothing\n");
			++failures;
		} else {
			bar->setValue(0);
			QApplication::processEvents();
			ParamSlider *ps = sliders.first();
			for (QWidget *inner : ps->findChildren<QWidget *>())
				wheel(inner, -3);
			settle(200);
			std::printf("     scrolled to %d\n", bar->value());
			ok(bar->value() > 0, "the wheel over a slider scrolled the panel");
		}
	}

	std::printf("\n-- the scroll bar itself still works --\n");
	{
		QScrollBar *bar = panel->verticalScrollBar();
		if (bar->maximum() > 0) {
			bar->setValue(0);
			QApplication::processEvents();
			// A QScrollBar IS a QAbstractSlider; a filter that catches those by
			// base class alone would stop the panel scrolling from its own bar.
			wheel(bar, -3);
			settle(200);
			std::printf("     scrolled to %d\n", bar->value());
			ok(bar->value() > 0, "wheeling the scroll bar still scrolls");
		}
	}

	std::printf("\n-- the preview keeps its wheel-to-zoom --\n");
	{
		PreviewCanvas *pc = w.findChild<PreviewCanvas *>();
		ok(pc != nullptr, "the preview is there");
		if (pc) {
			const double s0 = tv->model().tracks[0].clips[0].scale;
			wheel(pc, 3);
			settle(300);
			const double s1 = tv->model().tracks[0].clips[0].scale;
			std::printf("     clip scale %.3f -> %.3f\n", s0, s1);
			ok(std::abs(s1 - s0) > 1e-6,
			   "scrolling the preview still zooms — the guard is scoped");
		}
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
