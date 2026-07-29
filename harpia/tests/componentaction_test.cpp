// Buttons that belong to a component.
//
// "Reset transform" used to be a widget in the Inspector, placed by the window
// next to the values it acted on and owned by neither. It is the Transform
// component's own action now, which is the general mechanism: a component
// declares a button, the panel draws it without knowing what it does, and the
// window runs it over every selected clip in one undo step.
//
// The ways that can go wrong are all invisible from a screenshot:
//
//   - the button drawn but wired to nothing, so it looks fine and does nothing;
//   - the reset landing on the primary clip only, with five selected;
//   - the pose reset but the keyframes left, so the next frame puts it straight
//     back and the button looks broken on exactly the clips that needed it;
//   - a user-written component's action reaching the clip's fields, which would
//     mean it does something different depending on what it is attached to.
#include "editor/EditorWidgets.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/component/ComponentPanel.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QElapsedTimer>
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
static QPushButton *button(QWidget *w, const QString &text)
{
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text() == text)
			return b;
	return nullptr;
}

static void buildClips(TimelineView *tv, int n)
{
	TimelineModel m;
	TlTrack t;
	t.kind = TlTrack::Kind::Video;
	for (int i = 0; i < n; ++i) {
		TlClip c;
		c.type = TlClip::Type::Text;
		c.srcStartMs = 0;
		c.srcEndMs = 1000;
		c.outStartMs = i * 1000;
		c.text.text = QStringLiteral("C%1").arg(i);
		// Moved, zoomed and animated, so a reset has something to undo and the
		// keyframes have something to fight it with.
		c.posX = 0.1;
		c.posY = 0.9;
		c.scale = 2.5;
		c.rotation = 33;
		TlKeyframe k;
		k.tMs = 0;
		k.tf.posX = 0.1;
		k.tf.posY = 0.9;
		k.tf.scale = 2.5;
		k.tf.rotation = 33;
		k.tf.opacity = 1.0;
		c.keys.append(k);
		t.clips.append(c);
	}
	m.tracks.append(t);
	tv->setModel(m);
	// setModel records no undo step, so without closing an entry here the undo
	// below would jump past these clips to whatever the previous block left.
	emit tv->editCommitted();
	QApplication::processEvents();
}
static const TlClip &clipAt(TimelineView *tv, int i)
{
	return tv->model().tracks[0].clips[i];
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
	TimelineView *tv = w.findChild<TimelineView *>();
	ComponentPanel *panel = w.findChild<ComponentPanel *>();
	ok(tv && panel, "the editor came up with a component panel");
	if (!tv || !panel)
		return 1;

	const QString kXf = QStringLiteral("harpia.transform");

	std::printf("\n-- the component declares the button --\n");
	{
		const ComponentType *t = ComponentRegistry::instance().find(kXf);
		ok(t != nullptr, "Transform is registered");
		if (!t)
			return 1;
		ok(t->actions.size() == 1, "with one action");
		ok(!t->actions.isEmpty() && t->actions[0].id == QStringLiteral("reset"),
		   "which is the reset");
		ok(!t->actions.isEmpty() && t->actions[0].run != nullptr,
		   "and it carries its own implementation, not a name the window has to know");
	}

	std::printf("\n-- and the panel draws it inside that component --\n");
	{
		buildClips(tv, 1);
		tv->selectClip(0, 0);
		settle(400);
		QPushButton *b = button(panel, QStringLiteral("Reset transform"));
		ok(b != nullptr, "the button is in the component panel");
		// It used to live in the Inspector proper. Anywhere else in the window
		// would mean the old one is still there too.
		int inWindow = 0;
		for (QPushButton *p : w.findChildren<QPushButton *>())
			if (p->text() == QStringLiteral("Reset transform"))
				++inWindow;
		std::printf("     buttons named \"Reset transform\" in the window: %d\n", inWindow);
		ok(inWindow == 1, "exactly one — the old hand-placed one is gone");
	}

	std::printf("\n-- pressing it resets the clip, keyframes and all --\n");
	{
		buildClips(tv, 1);
		tv->selectClip(0, 0);
		settle(400);
		QPushButton *b = button(panel, QStringLiteral("Reset transform"));
		if (!b) {
			std::printf("  FAIL no button to press\n");
			return 1;
		}
		b->click();
		settle(500);
		eq(clipAt(tv, 0).scale, 1.0, "zoom back to 1");
		eq(clipAt(tv, 0).posX, 0.5, "centred across");
		eq(clipAt(tv, 0).posY, 0.5, "and down");
		eq(clipAt(tv, 0).rotation, 0.0, "not rotated");
		// Without this the keys win on the very next frame and the button looks
		// like it did nothing at all.
		ok(clipAt(tv, 0).keys.isEmpty(), "and the pose animation is cleared, not left to win");
	}

	std::printf("\n-- with several selected it reaches all of them --\n");
	{
		buildClips(tv, 3);
		tv->selectClip(0, 0);
		tv->addToSelection(0, 1);
		tv->addToSelection(0, 2);
		settle(500);
		QPushButton *b = button(panel, QStringLiteral("Reset transform"));
		if (!b) {
			std::printf("  FAIL no button to press\n");
			return 1;
		}
		b->click();
		settle(900); // the snapshot is debounced
		int done = 0;
		for (int i = 0; i < 3; ++i)
			if (std::abs(clipAt(tv, i).scale - 1.0) < 1e-9 &&
			    std::abs(clipAt(tv, i).posX - 0.5) < 1e-9 && clipAt(tv, i).keys.isEmpty())
				++done;
		std::printf("     clips reset: %d of 3\n", done);
		ok(done == 3, "all three, not just the primary");

		QMetaObject::invokeMethod(&w, "undo");
		settle(600);
		int back = 0;
		for (int i = 0; i < 3; ++i)
			if (std::abs(clipAt(tv, i).scale - 2.5) < 1e-9)
				++back;
		std::printf("     clips restored by one undo: %d of 3\n", back);
		ok(back == 3, "and one undo puts all three back");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
