// The keyframe editor shows every channel the clip actually animates.
//
// It had four tabs -- Position, Scale, Rotation, Opacity -- and a clip can be
// animated in more ways than that. A keyed component property (a Blur radius, a
// shader's colour) moved in the preview with nowhere in this window to see it,
// let alone retime it: the diamonds were on the timeline and the numbers were
// in the Inspector, and neither is where you go to work on an animation.
//
// What is pinned here is the RULE, not the four defaults: a tab exists exactly
// when that property has keys. Both directions matter -- a tab for a property
// with no keys is a lane you cannot add to (the key list is created by the
// Inspector's diamond, on the component), and a keyed property with no tab is
// the bug this fixes.
#include "editor/timeline/KeyframeEditor.hpp"

#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"

#include <QApplication>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void eq(qint64 got, qint64 want, const char *w)
{
	const bool good = got == want;
	std::printf("  %s %s (got %lld, want %lld)\n", good ? "PASS" : "FAIL", w, (long long)got,
		    (long long)want);
	if (!good)
		++failures;
}

// A ten-second clip with a Blur whose radius is keyed twice, and a second
// component with nothing keyed at all.
static TlClip keyedClip()
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.srcStartMs = 0;
	c.srcEndMs = 10000;
	c.outStartMs = 2000;

	ComponentInstance blur;
	blur.typeId = QStringLiteral("harpia.blur");
	blur.instanceId = QStringLiteral("b1");
	blur.keys.insert(QStringLiteral("radius"),
			 {PropKey{0, 0.0, TlEase::Linear, 0.42, 0.58},
			  PropKey{4000, 1.0, TlEase::EaseInOut, 0.42, 0.58}});

	ComponentInstance quiet;
	quiet.typeId = QStringLiteral("harpia.speed");
	quiet.instanceId = QStringLiteral("s1");
	quiet.props.insert(QStringLiteral("factor"), 2.0);

	c.components << blur << quiet;
	return c;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	registerBuiltinComponents(ComponentRegistry::instance());

	KeyframeEditor ed;

	std::printf("\n-- a clip with no component keys has the four channels --\n");
	{
		TlClip plain;
		plain.srcEndMs = 5000;
		ed.setClip(plain, QStringLiteral("plain"));
		eq(ed.tabCountForTest(), 4, "four tabs, as before");
		ok(ed.tabTextForTest(0) == QStringLiteral("Position") &&
			   ed.tabTextForTest(3) == QStringLiteral("Opacity"),
		   "and they are the transform channels");
	}

	std::printf("\n-- a keyed component property gets one too --\n");
	{
		ed.setClip(keyedClip(), QStringLiteral("keyed"));
		eq(ed.tabCountForTest(), 5, "one extra tab");
		std::printf("     tab 4: \"%s\"\n", qPrintable(ed.tabTextForTest(4)));
		ok(ed.tabTextForTest(4).contains(QStringLiteral("Blur")),
		   "named for the component that owns it");
		ok(ed.tabTextForTest(4).contains(QStringLiteral("Radius"), Qt::CaseInsensitive),
		   "and for the property");
		// The control: the Speed component is there too, and has no keys.
		bool speedTab = false;
		for (int i = 0; i < ed.tabCountForTest(); ++i)
			if (ed.tabTextForTest(i).contains(QStringLiteral("Speed")))
				speedTab = true;
		ok(!speedTab, "a component with nothing keyed gets NO tab");

		KeyframeLane *lane = ed.laneForTest(4);
		ok(lane && lane->isComponentLane(), "the tab holds a component lane");
		eq(lane->keyCount(), 2, "showing that property's keys");
		eq(lane->keyTimeAt(1), 4000, "at their clip-relative times");
		ok(ed.laneForTest(0) && !ed.laneForTest(0)->isComponentLane(),
		   "while the transform lanes are unchanged");
	}

	std::printf("\n-- and the tab goes when the last key does --\n");
	{
		TlClip c = keyedClip();
		c.components[0].keys.remove(QStringLiteral("radius"));
		ed.setClip(c, QStringLiteral("unkeyed"));
		eq(ed.tabCountForTest(), 4, "back to the four channels");
	}

	std::printf("\n-- editing through the lane --\n");
	{
		ed.setClip(keyedClip(), QStringLiteral("keyed"));
		KeyframeLane *lane = ed.laneForTest(4);
		ed.showTabForTest(4);

		// Adding a key must not change the picture: it pins what the property
		// is ALREADY doing at that instant. Half-way between 0.0 and 1.0 on a
		// linear leg is 0.5.
		const int k = lane->addKeyAtForOwner(2000);
		ok(k >= 0, "a key can be added at the playhead");
		eq(lane->keyCount(), 3, "and the lane shows it");
		const TlClip &c = lane->clip();
		const QVector<PropKey> &keys = c.components[0].keys[QStringLiteral("radius")];
		ok(keys[k].tMs == 2000 && std::abs(keys[k].v - 0.5) < 1e-6,
		   "seeded from what the property was already doing, so adding it changed "
		   "nothing on screen");

		lane->removeKeyAtForOwner(k);
		eq(lane->keyCount(), 2, "and it can be removed again");

		// Down to none, the property must stop looking animated: propAt() reads
		// "has a keys entry" as "is animated", so an empty vector would leave
		// the static value permanently overridden by nothing.
		lane->removeKeyAtForOwner(1);
		lane->removeKeyAtForOwner(0);
		ok(!lane->clip().components[0].keys.contains(QStringLiteral("radius")),
		   "and the last one taken away un-keys the property outright");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
