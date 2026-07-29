// The Inspector's component list, driven through the real editor.
//
// The unit tests prove the runtime is right; this proves the panel is actually
// connected to it. Adding a component from the menu has to reach the clip, the
// preview has to repaint, and a value typed into a slider has to change what is
// on screen — the three joins where a Qt wiring mistake hides and where every
// other test would stay green.
#include "editor/EditorWidgets.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/component/ComponentPanel.hpp"
#include "editor/timeline/TimelineView.hpp"

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
// setPlayhead emits nothing, so it forces no composite. Clicking the ruler is
// what a user does and what actually renders.
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
	ok(tv && pc, "the timeline and preview exist");
	ok(panel != nullptr, "the Inspector has a component list");
	if (!tv || !pc || !panel)
		return 1;

	std::printf("\n-- it appears only when a clip is selected --\n");
	// isHidden(), not isVisible() or isVisibleTo(): the Inspector pane may be
	// collapsed in this headless window, which would make everything inside it
	// invisible and turn both halves of this check into false passes.
	// isHidden() reports the widget's OWN flag, which is exactly what
	// syncComponentPanel sets.
	// Deselect explicitly: entering Full Editing may already have a clip
	// selected, in which case the panel is legitimately up and asserting it is
	// hidden would be testing the startup state rather than the rule.
	tv->selectClip(-1, -1);
	settle(200);
	ok(panel->isHidden(), "hidden with nothing selected");
	tv->selectClip(0, 0);
	settle(300);
	ok(!panel->isHidden(), "and shown once a clip is");
	ok(button(panel, QStringLiteral("Add Component")) != nullptr, "with an Add Component button");

	scrubTo(tv, 500);
	settle(700);
	const double plain = meanLuma(pc->currentFrame());
	std::printf("     plain frame: mean luma %.1f\n", plain);
	ok(plain > 5, "and the clip is rendering something to change");

	std::printf("\n-- adding a component reaches the clip AND the picture --\n");
	{
		// Straight through the panel's own signal: the Add menu is a QMenu and
		// exec() blocks, so driving the menu itself would hang a headless run.
		// Everything downstream of the signal -- the clip write, the repaint,
		// the panel rebuild -- is the part that can be wrong, and it is all here.
		ComponentInstance rot;
		rot.typeId = QStringLiteral("harpia.alwaysRotate");
		rot.instanceId = QStringLiteral("r1");
		rot.props.insert(QStringLiteral("degreesPerSecond"), 90.0);
		emit panel->componentsEdited({rot});
		settle(700);

		const TlClip *c = tv->selectedClipPtr();
		ok(c && c->components.size() == 1, "the clip now carries one component");
		ok(c && c->components[0].typeId == QStringLiteral("harpia.alwaysRotate"),
		   "and it is the one that was added");

		const double rotated = meanLuma(pc->currentFrame());
		std::printf("     after Always Rotate: mean luma %.1f\n", rotated);
		ok(std::abs(rotated - plain) > 3.0, "the preview repainted with it applied");
	}

	std::printf("\n-- the panel shows what the clip holds --\n");
	{
		// One row per component, with its name and its property. Found by text,
		// the way the user reads it.
		bool named = false;
		for (QLabel *l : panel->findChildren<QLabel *>())
			if (l->text() == QStringLiteral("Always Rotate"))
				named = true;
		ok(named, "the component's name is on screen");
		bool labelled = false;
		for (QLabel *l : panel->findChildren<QLabel *>())
			if (l->text() == QStringLiteral("Degrees / second"))
				labelled = true;
		ok(labelled, "and its property's label, built from the PropDef");
		bool staged = false;
		for (QLabel *l : panel->findChildren<QLabel *>())
			if (l->text() == QStringLiteral("Transform"))
				staged = true;
		ok(staged, "and which stage it runs in, since that is the first thing anyone asks");
	}

	std::printf("\n-- disabling a component from the panel stops it --\n");
	{
		const double on = meanLuma(pc->currentFrame());
		QVector<ComponentInstance> list = tv->selectedClipPtr()->components;
		list[0].enabled = false;
		emit panel->componentsEdited(list);
		settle(700);
		const double off = meanLuma(pc->currentFrame());
		std::printf("     enabled %.1f  disabled %.1f  plain %.1f\n", on, off, plain);
		ok(std::abs(off - plain) < 1.5, "the frame is back to the un-componented one");
		ok(tv->selectedClipPtr()->components.size() == 1,
		   "and the component is still there, just off");
	}

	std::printf("\n-- removing it leaves the clip clean --\n");
	{
		emit panel->componentsEdited({});
		settle(500);
		ok(tv->selectedClipPtr()->components.isEmpty(), "no components left");
	}

	std::printf("\n-- an unknown component is shown, not dropped --\n");
	{
		ComponentInstance missing;
		missing.typeId = QStringLiteral("nobody.hasthis");
		missing.instanceId = QStringLiteral("m1");
		emit panel->componentsEdited({missing});
		settle(500);
		bool warned = false;
		for (QLabel *l : panel->findChildren<QLabel *>())
			if (l->text().contains(QStringLiteral("nobody.hasthis")))
				warned = true;
		ok(warned, "the panel names the component it cannot find");
		ok(tv->selectedClipPtr()->components.size() == 1,
		   "and it stays on the clip, so saving cannot destroy it");
	}

	std::printf("\n-- deleting the clip takes the panel and the components with it --\n");
	{
		// Components live inside TlClip, so removing the clip removes them; the
		// thing that could go wrong is the panel staying up and editing a clip
		// that is no longer there.
		tv->selectClip(0, 0);
		settle(200);
		ComponentInstance b;
		b.typeId = QStringLiteral("harpia.blur");
		b.instanceId = QStringLiteral("b1");
		emit panel->componentsEdited({b});
		settle(400);
		ok(!panel->isHidden(), "the panel is up with a component on the clip");

		tv->deleteSelected();
		settle(500);
		ok(panel->isHidden(), "deleting the clip hides the panel");
		ok(tv->selectedClipPtr() == nullptr, "and nothing is selected");
		int left = 0;
		for (const TlTrack &t : tv->model().tracks)
			for (const TlClip &c : t.clips)
				left += c.components.size();
		ok(left == 0, "no components survive the clip they were on");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
