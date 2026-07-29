// The clip's pose, now a pinned row in the component list.
//
// Zoom, Position, Rotation and Opacity used to be a separate Transform section
// in the Inspector. They are the Transform component now — pinned first, not
// removable, exactly as Unity treats a GameObject's Transform, and for the same
// reason: a clip without a pose is not a thing.
//
// What has to remain true after moving them:
//   - the old rows are gone, so there is one place to look and not two;
//   - editing the new row still moves the picture;
//   - it goes through the SAME path the preview drag uses, so auto-keyframe and
//     the keyframe editor are unaffected;
//   - the row shows the pose AT THE PLAYHEAD on an animated clip, not the
//     resting value, which is what the old panel did and what the picture shows.
#include "editor/EditorWidgets.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/component/ComponentPanel.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QDoubleSpinBox>
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
static void eq(double got, double want, const char *w, double tol = 1e-6)
{
	const bool g = std::abs(got - want) <= tol;
	std::printf("  %s %s (got %g, want %g)\n", g ? "PASS" : "FAIL", w, got, want);
	if (!g)
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
static void scrubTo(TimelineView *tv, qint64 ms)
{
	const QPoint p(tv->xForMs(ms), 14);
	QMouseEvent a(QEvent::MouseButtonPress, QPointF(p), tv->mapToGlobal(p), Qt::LeftButton,
		      Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(tv, &a);
	QMouseEvent b(QEvent::MouseButtonRelease, QPointF(p), tv->mapToGlobal(p), Qt::LeftButton,
		      Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(tv, &b);
}
static double meanLuma(const QImage &f)
{
	if (f.isNull())
		return -1;
	double s = 0;
	int n = 0;
	for (int y = 0; y < f.height(); y += 3)
		for (int x = 0; x < f.width(); x += 3) {
			s += QColor(f.pixel(x, y)).lightness();
			++n;
		}
	return n ? s / n : -1;
}
static QPushButton *button(QWidget *w, const QString &text)
{
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text() == text)
			return b;
	return nullptr;
}
static bool hasLabel(QWidget *w, const QString &text)
{
	for (QLabel *l : w->findChildren<QLabel *>())
		if (l->text() == text)
			return true;
	return false;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	VideoEditorWindow w(QStringLiteral(SRC_MEDIA));
	w.resize(1400, 900);
	w.show();
	QApplication::processEvents();
	if (QPushButton *full = button(&w, QStringLiteral("Full Editing"))) {
		full->click();
		settle(400);
	}
	TimelineView *tv = w.findChild<TimelineView *>();
	PreviewCanvas *pc = w.findChild<PreviewCanvas *>();
	ComponentPanel *panel = w.findChild<ComponentPanel *>();
	ok(tv && pc && panel, "the editor came up with a component panel");
	if (!tv || !pc || !panel)
		return 1;

	tv->selectClip(0, 0);
	scrubTo(tv, 500);
	settle(700);

	std::printf("\n-- the pose is in the component list --\n");
	ok(hasLabel(panel, QStringLiteral("Transform")), "there is a Transform row");
	for (const QString &row : {QStringLiteral("Zoom"), QStringLiteral("Position X"),
				   QStringLiteral("Position Y"), QStringLiteral("Rotation"),
				   QStringLiteral("Opacity")})
		ok(hasLabel(panel, row), qPrintable(QStringLiteral("  with %1").arg(row)));

	std::printf("\n-- and NOT in a second panel of its own --\n");
	{
		// The whole point of the move: one place to look. If the old section
		// were still built, these labels would exist outside the panel too.
		int outside = 0;
		for (QLabel *l : w.findChildren<QLabel *>()) {
			if (l->text() != QStringLiteral("Zoom") &&
			    l->text() != QStringLiteral("Position X") &&
			    l->text() != QStringLiteral("Opacity"))
				continue;
			bool insidePanel = false;
			for (QWidget *a = l->parentWidget(); a; a = a->parentWidget())
				if (a == panel)
					insidePanel = true;
			if (!insidePanel)
				++outside;
		}
		std::printf("     pose labels outside the component panel: %d\n", outside);
		ok(outside == 0, "the old Transform section is gone");
	}

	std::printf("\n-- Speed stayed behind, on purpose --\n");
	// It is the clip's playback rate, which changes its LENGTH on the timeline.
	// Not a pose, and not the same thing as the Speed component.
	ok(hasLabel(&w, QStringLiteral("Speed")), "the clip's Speed row is still in the Inspector");

	std::printf("\n-- the row cannot be taken away --\n");
	{
		// A pose row with a remove button would be offering nonsense. Count the
		// remove buttons: with no components added there should be none at all.
		int removes = 0;
		for (QPushButton *b : panel->findChildren<QPushButton *>())
			if (b->toolTip().contains(QStringLiteral("Remove this component")))
				++removes;
		ok(removes == 0, "no remove button on the pinned row");
	}

	std::printf("\n-- editing it moves the picture --\n");
	{
		const double before = meanLuma(pc->currentFrame());
		TlTransform tf = tv->selectedClipPtr()->transformAt(500);
		tf.scale = 2.5;
		emit panel->transformEdited(tf);
		settle(700);
		const double after = meanLuma(pc->currentFrame());
		std::printf("     zoom 1.0 -> 2.5: mean luma %.1f -> %.1f\n", before, after);
		eq(tv->selectedClipPtr()->transformAt(500).scale, 2.5, "the clip took the new zoom");
		ok(std::abs(after - before) > 1.0, "and the preview repainted");
	}

	std::printf("\n-- it reads the pose AT THE PLAYHEAD, not the resting value --\n");
	{
		// Animate the clip, then check the row follows the playhead. Reading the
		// base pose instead would show a number that disagrees with the screen —
		// the same fault the Spotlight fields had.
		TimelineModel m = tv->model();
		TlClip &c = m.tracks[0].clips[0];
		c.setBaseTransform(TlTransform{});
		c.keys.clear();
		TlKeyframe k0;
		k0.tMs = 0;
		k0.tf.scale = 1.0;
		TlKeyframe k1;
		k1.tMs = 2000;
		k1.tf.scale = 3.0;
		c.keys << k0 << k1; // every channel is pinned by default
		tv->setModel(m);
		tv->selectClip(0, 0);

		scrubTo(tv, 1000); // halfway: scale should read about 2
		settle(700);
		double shown = -1;
		for (QDoubleSpinBox *s : panel->findChildren<QDoubleSpinBox *>())
			if (std::abs(s->value() - 2.0) < 0.35)
				shown = s->value();
		const double actual = tv->selectedClipPtr()->transformAt(1000).scale;
		std::printf("     at 1s the clip's zoom is %.3f; a row shows %.3f\n", actual, shown);
		ok(shown > 0, "a row on screen is showing the interpolated zoom, not 1.0");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
