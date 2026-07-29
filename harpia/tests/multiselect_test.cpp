// Editing several clips at once.
//
// The Inspector shows the components every selected clip HAS IN COMMON, an em
// dash where they disagree, and applies an edit to all of them in one undo
// step. The things that can go wrong are all invisible from a screenshot:
//
//   - a component on only some clips shown as if it were on all, so editing it
//     silently creates it on the others;
//   - one clip's value displayed as though it spoke for the rest;
//   - an edit reaching only the primary clip;
//   - five clips changed and five undos needed to put them back.
#include "editor/EditorWidgets.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/component/ComponentPanel.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
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
static bool hasLabelContaining(QWidget *w, const QString &frag)
{
	for (QLabel *l : w->findChildren<QLabel *>())
		if (l->text().contains(frag))
			return true;
	return false;
}
// The em dash lives in the spin box's line edit, which is where setMixed puts
// it — a value of 0 would look identical from the outside.
static bool showsMixedDash(QWidget *panel)
{
	for (QLineEdit *le : panel->findChildren<QLineEdit *>())
		if (le->text().contains(QChar(0x2014)))
			return true;
	return false;
}

// Three clips on one video track, with the components each is given.
static void buildClips(TimelineView *tv, const QVector<QVector<QString>> &perClip)
{
	TimelineModel m;
	TlTrack t;
	t.kind = TlTrack::Kind::Video;
	for (int i = 0; i < perClip.size(); ++i) {
		TlClip c;
		c.type = TlClip::Type::Text;
		c.srcStartMs = 0;
		c.srcEndMs = 1000;
		c.outStartMs = i * 1000;
		c.text.text = QStringLiteral("C%1").arg(i);
		for (const QString &id : perClip[i]) {
			ComponentInstance ci;
			ci.typeId = id;
			ci.instanceId = QStringLiteral("%1-%2").arg(id).arg(i);
			c.components.append(ci);
		}
		t.clips.append(c);
	}
	m.tracks.append(t);
	tv->setModel(m);
	// setModel is a programmatic setter and records no undo step, so without
	// this the first undo of the block below would jump past these clips to
	// whatever the previous block left behind. Close an entry on the built
	// model so "undo" means "undo the edit", which is what is being tested.
	emit tv->editCommitted();
	QApplication::processEvents();
}

static const TlClip &clipAt(TimelineView *tv, int i)
{
	return tv->model().tracks[0].clips[i];
}
static double propOf(TimelineView *tv, int clip, const QString &type, const QString &key)
{
	for (const ComponentInstance &ci : clipAt(tv, clip).components)
		if (ci.typeId == type)
			return ci.props.value(key).toDouble();
	return -999;
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
	ComponentPanel *panel = w.findChild<ComponentPanel *>();
	ok(tv && panel, "the editor came up with a component panel");
	if (!tv || !panel)
		return 1;

	const QString kBlur = QStringLiteral("harpia.blur");
	const QString kRot = QStringLiteral("harpia.alwaysRotate");

	std::printf("\n-- the header says how many are selected --\n");
	{
		buildClips(tv, {{kBlur}, {kBlur}, {kBlur}});
		tv->selectClip(0, 0);
		settle(300);
		ok(!hasLabelContaining(panel, QStringLiteral("clips selected")),
		   "one clip: no count, because there is nothing to disambiguate");
		tv->addToSelection(0, 1);
		tv->addToSelection(0, 2);
		settle(300);
		ok(hasLabelContaining(panel, QStringLiteral("3 clips selected")),
		   "three clips: it says so");
	}

	std::printf("\n-- only components ALL of them have are shown --\n");
	{
		// Blur on all three; Always Rotate on the first two only.
		buildClips(tv, {{kBlur, kRot}, {kBlur, kRot}, {kBlur}});
		tv->selectClip(0, 0);
		tv->addToSelection(0, 1);
		tv->addToSelection(0, 2);
		settle(400);
		ok(hasLabelContaining(panel, QStringLiteral("Blur")), "Blur is listed");
		bool rotRow = false;
		for (QLabel *l : panel->findChildren<QLabel *>())
			if (l->text() == QStringLiteral("Always Rotate"))
				rotRow = true;
		ok(!rotRow, "Always Rotate is NOT, because one clip does not have it");
		ok(hasLabelContaining(panel, QStringLiteral("Not shared")),
		   "and it is named as not shared rather than vanishing without a word");
		ok(hasLabelContaining(panel, QStringLiteral("Always Rotate")),
		   "by name, in that line");
	}

	std::printf("\n-- agreeing values show, disagreeing ones show an em dash --\n");
	{
		buildClips(tv, {{kBlur}, {kBlur}});
		tv->selectClip(0, 0);
		settle(200);
		// Give them different radii, one clip at a time.
		emit panel->propertyEdited(kBlur, 0, QStringLiteral("radius"), 0.2);
		settle(300);
		tv->selectClip(0, 1);
		settle(200);
		emit panel->propertyEdited(kBlur, 0, QStringLiteral("radius"), 0.8);
		settle(300);
		eq(propOf(tv, 0, kBlur, QStringLiteral("radius")), 0.2, "clip 0 kept its own value");
		eq(propOf(tv, 1, kBlur, QStringLiteral("radius")), 0.8, "and clip 1 its own");

		tv->selectClip(0, 0);
		tv->addToSelection(0, 1);
		settle(400);
		ok(showsMixedDash(panel), "with both selected the radius reads as an em dash");

		// And when they DO agree, the number is shown rather than a dash.
		emit panel->propertyEdited(kBlur, 0, QStringLiteral("radius"), 0.5);
		settle(400);
		ok(!showsMixedDash(panel), "once they match, the value is shown again");
	}

	std::printf("\n-- editing a mixed field gives every clip that value --\n");
	{
		eq(propOf(tv, 0, kBlur, QStringLiteral("radius")), 0.5, "clip 0 took it");
		eq(propOf(tv, 1, kBlur, QStringLiteral("radius")), 0.5, "clip 1 took it too");
	}

	std::printf("\n-- and it is ONE undo, not one per clip --\n");
	{
		buildClips(tv, {{kBlur}, {kBlur}, {kBlur}});
		tv->selectClip(0, 0);
		tv->addToSelection(0, 1);
		tv->addToSelection(0, 2);
		settle(400);
		emit panel->propertyEdited(kBlur, 0, QStringLiteral("radius"), 0.75);
		settle(900); // the snapshot is debounced
		for (int i = 0; i < 3; ++i)
			eq(propOf(tv, i, kBlur, QStringLiteral("radius")), 0.75,
			   "all three took the value");

		// The slot directly: the Undo shortcut goes through the registry, and
		// this test is about what undo RESTORES, not about how it is invoked.
		QMetaObject::invokeMethod(&w, "undo");
		settle(600);
		int reverted = 0;
		for (int i = 0; i < 3; ++i)
			if (std::abs(propOf(tv, i, kBlur, QStringLiteral("radius")) - 0.75) > 1e-9)
				++reverted;
		std::printf("     clips reverted by one undo: %d of 3\n", reverted);
		ok(reverted == 3, "one undo put all three back together");
	}

	std::printf("\n-- batch operations reach every clip --\n");
	{
		buildClips(tv, {{kBlur}, {kBlur}, {kBlur}});
		tv->selectClip(0, 0);
		tv->addToSelection(0, 1);
		tv->addToSelection(0, 2);
		settle(400);

		emit panel->componentEnableChanged(kBlur, 0, false);
		settle(400);
		int off = 0;
		for (int i = 0; i < 3; ++i)
			for (const ComponentInstance &ci : clipAt(tv, i).components)
				if (!ci.enabled)
					++off;
		ok(off == 3, "disable turned it off on all three");

		emit panel->componentDuplicated(kBlur, 0);
		settle(400);
		int two = 0;
		for (int i = 0; i < 3; ++i)
			if (clipAt(tv, i).components.size() == 2)
				++two;
		ok(two == 3, "duplicate gave all three a second one");

		emit panel->componentRemoved(kBlur, 1);
		settle(400);
		int one = 0;
		for (int i = 0; i < 3; ++i)
			if (clipAt(tv, i).components.size() == 1)
				++one;
		ok(one == 3, "and removing the second took it off all three");

		emit panel->propertyEdited(kBlur, 0, QStringLiteral("radius"), 0.9);
		settle(300);
		emit panel->componentReset(kBlur, 0);
		settle(400);
		int atDefault = 0;
		for (int i = 0; i < 3; ++i)
			if (std::abs(propOf(tv, i, kBlur, QStringLiteral("radius")) - 0.15) < 1e-9)
				++atDefault;
		ok(atDefault == 3, "reset put all three back to the declared default");
	}

	std::printf("\n-- adding a component puts it on every selected clip --\n");
	{
		emit panel->componentAdded(kRot);
		settle(400);
		int got = 0;
		for (int i = 0; i < 3; ++i)
			for (const ComponentInstance &ci : clipAt(tv, i).components)
				if (ci.typeId == kRot)
					++got;
		ok(got == 3, "all three have it now");
	}

	std::printf("\n-- copy from one, paste to all --\n");
	{
		buildClips(tv, {{kBlur}, {kBlur}, {kBlur}});
		tv->selectClip(0, 0);
		settle(200);
		emit panel->propertyEdited(kBlur, 0, QStringLiteral("radius"), 0.33);
		settle(300);
		emit panel->componentCopied(kBlur, 0);
		tv->selectClip(0, 1);
		tv->addToSelection(0, 2);
		settle(300);
		emit panel->componentPasted(kBlur, 0);
		settle(400);
		eq(propOf(tv, 1, kBlur, QStringLiteral("radius")), 0.33, "clip 1 took the pasted value");
		eq(propOf(tv, 2, kBlur, QStringLiteral("radius")), 0.33, "and clip 2 as well");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
