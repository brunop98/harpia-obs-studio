// The keyframe list, and the two model rules behind it.
//
// Two layers, because they fail differently. The widget can show the wrong
// times, lose your selection or offer a Prev that goes nowhere. The rules
// underneath can leave a mask claiming to be animated by a single key, which
// silently freezes the Inspector's number fields — that one is invisible until
// someone wonders why typing a value does nothing.
#include "editor/KeyList.hpp"
#include "editor/timeline/EffectClip.hpp"
#include "editor/timeline/Spotlight.hpp"

#include <QApplication>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void eq(double got, double want, const char *w)
{
	const bool good = std::abs(got - want) < 1e-9;
	std::printf("  %s %s (got %g, want %g)\n", good ? "PASS" : "FAIL", w, got, want);
	if (!good)
		++failures;
}

// The widget exposes no accessors on purpose; find its parts the way the user
// sees them — by what the buttons say they do.
static QPushButton *btn(KeyList *k, const QString &tipFragment)
{
	for (QPushButton *b : k->findChildren<QPushButton *>())
		if (b->toolTip().contains(tipFragment, Qt::CaseInsensitive))
			return b;
	return nullptr;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	// ---- the widget ----------------------------------------------------
	std::printf("\n-- the list shows what it is given --\n");
	{
		KeyList k(QStringLiteral("the thing"));
		auto *list = k.findChild<QListWidget *>();
		ok(list != nullptr, "it has a list");
		if (!list)
			return 1;

		k.setTimes({}, 0);
		ok(list->count() == 1 && list->item(0)->text().contains(QStringLiteral("Not animated")),
		   "empty says so rather than showing nothing");

		// Deliberately out of order: the caller should not have to sort.
		k.setTimes({3000, 500, 1750}, 1750);
		ok(list->count() == 3, "three keys listed");
		ok(list->item(0)->text().contains(QStringLiteral("0:00.500")) &&
			   list->item(2)->text().contains(QStringLiteral("0:03.000")),
		   "sorted by time, whatever order they arrived in");
		ok(list->item(1)->text().startsWith(QStringLiteral(">")),
		   "the key at the playhead is marked");
		ok(!list->item(0)->text().startsWith(QStringLiteral(">")),
		   "and the others are not");
	}

	std::printf("\n-- Prev and Next only offer somewhere to go --\n");
	{
		KeyList k(QStringLiteral("the thing"));
		auto *prev = btn(&k, QStringLiteral("previous key"));
		auto *next = btn(&k, QStringLiteral("next key"));
		ok(prev && next, "both buttons exist");
		if (!prev || !next)
			return 1;

		k.setTimes({1000, 2000, 3000}, 2000);
		ok(prev->isEnabled() && next->isEnabled(), "in the middle, both work");
		k.setTimes({1000, 2000, 3000}, 500);
		ok(!prev->isEnabled() && next->isEnabled(), "before the first, only Next");
		k.setTimes({1000, 2000, 3000}, 5000);
		ok(prev->isEnabled() && !next->isEnabled(), "after the last, only Prev");
		k.setTimes({}, 0);
		ok(!prev->isEnabled() && !next->isEnabled(), "with no keys, neither");

		// Sitting exactly ON a key must still step off it, not stick.
		k.setTimes({1000, 2000, 3000}, 2000);
		QSignalSpy jumps(&k, &KeyList::jumpRequested);
		prev->click();
		ok(jumps.count() == 1 && jumps.at(0).at(0).toLongLong() == 1000,
		   "Prev from a key goes to the one before it, not to itself");
		next->click();
		ok(jumps.count() == 2 && jumps.at(1).at(0).toLongLong() == 3000,
		   "and Next to the one after");
	}

	std::printf("\n-- Remove reports the time, not the row --\n");
	{
		KeyList k(QStringLiteral("the thing"));
		auto *list = k.findChild<QListWidget *>();
		auto *del = btn(&k, QStringLiteral("Remove the selected"));
		ok(del != nullptr, "the remove button exists");
		if (!del || !list)
			return 1;

		k.setTimes({1000, 2000, 3000}, 0);
		ok(!del->isEnabled(), "nothing selected, nothing to remove");
		list->setCurrentRow(1);
		ok(del->isEnabled(), "selecting a row enables it");
		QSignalSpy removed(&k, &KeyList::removeRequested);
		del->click();
		ok(removed.count() == 1 && removed.at(0).at(0).toLongLong() == 2000,
		   "it reports 2000 ms");

		// Selection follows the TIME across a refresh. Keeping the row instead
		// would silently move the selection when an earlier key is added.
		k.setTimes({1000, 2000, 3000}, 0);
		list->setCurrentRow(1); // 2000
		k.setTimes({250, 1000, 2000, 3000}, 0);
		ok(list->currentRow() == 2, "adding an earlier key keeps 2000 selected");
		QSignalSpy removed2(&k, &KeyList::removeRequested);
		del->click();
		ok(removed2.count() == 1 && removed2.at(0).at(0).toLongLong() == 2000,
		   "so Remove still means the key you picked");
	}

	// ---- the rules underneath -------------------------------------------
	std::printf("\n-- a mask's keys --\n");
	{
		SpotMask m;
		m.pose.cx = 0.25;
		ok(m.keys.isEmpty(), "starts un-animated");

		m.setKeyAt(0);
		// With one key present poseAt() answers from it and ignores `pose`
		// entirely, so a second key has to be given its value directly -- which
		// is what the panel and the preview drag both do.
		m.keys.front().pose.cx = 0.25;
		SpotKey second;
		second.tMs = 1000;
		second.pose = m.keys.front().pose;
		second.pose.cx = 0.75;
		m.keys.append(second);
		ok(m.keys.size() == 2, "two keys");
		eq(m.keys[0].pose.cx, 0.25, "the first holds the original pose");

		// poseAt, not pose: an animated mask's visible pose is interpolated, and
		// Add key must store what is on screen.
		m.setKeyAt(500);
		ok(m.keys.size() == 3, "a key in the middle");
		eq(m.keys[1].pose.cx, 0.5, "records the INTERPOLATED pose, not the base one");

		// Adding on an existing time replaces rather than duplicating.
		m.setKeyAt(500);
		ok(m.keys.size() == 3, "adding at the same time replaces it");

		m.removeKeyAt(500);
		ok(m.keys.size() == 2, "removing one leaves two");
		m.removeKeyAt(1000);
		ok(m.keys.isEmpty(), "removing down to ONE clears the list...");
		eq(m.pose.cx, 0.25, "...and folds that key's pose back into the static one");
		// This is the bit that matters: with a single key left behind, poseAt()
		// would return it forever and the panel's fields would look broken.
		eq(m.poseAt(9999).cx, 0.25, "so the pose is now what the fields say");

		m.removeKeyAt(4242); // a time with no key
		ok(m.keys.isEmpty() && std::abs(m.pose.cx - 0.25) < 1e-9,
		   "removing a time that has no key changes nothing");
	}

	std::printf("\n-- an effect's keys --\n");
	{
		FxSpec fx;
		fx.type = FxType::Brightness;
		fx.params = fxDefaults(FxType::Brightness);
		fx.params[QStringLiteral("amount")] = 0.2;

		fx.setKeyAt(0, fx.params);
		QMap<QString, double> later = fx.params;
		later[QStringLiteral("amount")] = 0.8;
		fx.setKeyAt(1000, later);
		ok(fx.keys.size() == 2, "two keys");
		eq(fx.paramsAt(500).value(QStringLiteral("amount")), 0.5,
		   "the value between them is interpolated");

		fx.setKeyAt(1000, fx.params); // same time again
		ok(fx.keys.size() == 2, "adding at an existing time replaces it");

		fx.removeKeyAt(1000);
		ok(fx.keys.isEmpty(), "down to one key clears the list...");
		eq(fx.params.value(QStringLiteral("amount")), 0.2,
		   "...and the survivor's value becomes the static one");
		eq(fx.paramsAt(9999).value(QStringLiteral("amount")), 0.2,
		   "so the spin boxes drive it again");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
