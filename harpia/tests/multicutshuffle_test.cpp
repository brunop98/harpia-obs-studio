// Multi-Cut: "Randomize order of selected cuts". The selected cuts trade places
// among their own positions, every other cut stays where it is, the order is
// always different, the sequence re-times itself, and the window hears about
// it once (one undo step).
#include "editor/TrackEditor.hpp"
#include "editor/timeline/ClipShuffle.hpp"

#include <QApplication>

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

static CutSegment cut(int source, qint64 lenMs)
{
	CutSegment c;
	c.sourceId = source;
	c.srcStartMs = 0;
	c.srcEndMs = lenMs;
	return c;
}

static QVector<int> sources(const TrackEditor &t)
{
	QVector<int> out;
	for (const CutSegment &s : t.segments())
		out.append(s.sourceId);
	return out;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	TrackEditor t;
	t.resize(800, 200);
	t.setDuration(60000);
	// Six cuts of different lengths, so a reorder is visible in the timing.
	t.setSegments({cut(1, 1000), cut(2, 2000), cut(3, 3000), cut(4, 4000), cut(5, 5000), cut(6, 6000)});
	int changes = 0;
	QObject::connect(&t, &TrackEditor::segmentsChanged, [&changes]() { ++changes; });

	std::printf("\n-- fewer than two selected: nothing --\n");
	{
		ok(t.shuffleSelected(1) == 0 && changes == 0, "no selection: nothing moves, nothing is announced");
		t.setSegments(t.segments()); // clears the selection
	}

	std::printf("\n-- the selected cuts trade places among their own positions --\n");
	{
		// Select cuts 1, 3 and 5 (sources 2, 4, 6) -- the only public way to
		// build a multi-selection without a mouse is through the model, so the
		// test selects by re-applying and then using the widget's own setter.
		t.setSegments({cut(1, 1000), cut(2, 2000), cut(3, 3000), cut(4, 4000), cut(5, 5000), cut(6, 6000)});
		t.selectForTest({1, 3, 5});
		ok(t.selectedIndices() == QList<int>({1, 3, 5}), "three cuts selected");
		changes = 0;
		const int moved = t.shuffleSelected(4242);
		const QVector<int> s = sources(t);
		std::printf("     order now: %d %d %d %d %d %d (moved %d)\n", s[0], s[1], s[2], s[3], s[4], s[5], moved);
		ok(moved >= 2, "the selected cuts changed places");
		ok(s[0] == 1 && s[2] == 3 && s[4] == 5, "the unselected cuts did not move");
		QVector<int> sel = {s[1], s[3], s[5]};
		std::sort(sel.begin(), sel.end());
		ok(sel == QVector<int>({2, 4, 6}), "positions 1, 3 and 5 still hold the same three cuts");
		ok(changes == 1, "one segmentsChanged: one undo step");
		ok(t.selectedIndices() == QList<int>({1, 3, 5}), "the selection still names those positions");
		// Re-timing is the sequence's own: the output start of cut 2 is the
		// length of whatever now sits at position 0 and 1.
		ok(t.outputStartOf(2) == 1000 + t.segments()[1].outDurationMs(), "the sequence re-timed itself");
	}

	std::printf("\n-- always a different order --\n");
	{
		t.setSegments({cut(1, 1000), cut(2, 2000), cut(3, 3000)});
		t.selectForTest({0, 1, 2});
		int same = 0;
		for (int i = 0; i < 100; ++i) {
			const QVector<int> before = sources(t);
			t.shuffleSelected(quint64(i + 7));
			if (sources(t) == before)
				++same;
		}
		ok(same == 0, "100 shuffles of three cuts never hand back the same order");
		t.selectForTest({0, 1});
		const QVector<int> before = sources(t);
		t.shuffleSelected(99);
		ok(sources(t)[0] == before[1] && sources(t)[1] == before[0] && sources(t)[2] == before[2],
		   "two selected: they swap, the third stays");
	}

	std::printf("\n-- the Randomize command: nothing selected means every cut --\n");
	{
		TrackEditor u;
		u.resize(800, 200);
		u.setDuration(60000);
		u.setSegments({cut(1, 1000), cut(2, 2000), cut(3, 3000), cut(4, 4000), cut(5, 5000), cut(6, 6000)});
		int announced = 0;
		QObject::connect(&u, &TrackEditor::segmentsChanged, [&announced]() { ++announced; });
		const QVector<int> before = sources(u);
		const int moved = u.shuffle(ShuffleOptions(), 5);
		QVector<int> after = sources(u);
		ok(moved >= 4, "with nothing selected, most cuts move (High strength)");
		ok(after != before, "the order changed");
		QVector<int> sortedAfter = after;
		std::sort(sortedAfter.begin(), sortedAfter.end());
		ok(sortedAfter == before, "every cut is still there once");
		ok(announced == 1, "and the window hears about it once (one undo step)");

		// Keep the first and the last: only the middle moves.
		u.setSegments({cut(1, 1000), cut(2, 2000), cut(3, 3000), cut(4, 4000), cut(5, 5000), cut(6, 6000)});
		ShuffleOptions keep;
		keep.keep = ShuffleOptions::Keep::FirstAndLast;
		u.shuffle(keep, 9);
		after = sources(u);
		ok(after.first() == 1 && after.last() == 6, "keep first and last is honoured");

		// Two selected: the selection wins over "all".
		u.setSegments({cut(1, 1000), cut(2, 2000), cut(3, 3000), cut(4, 4000), cut(5, 5000), cut(6, 6000)});
		u.selectForTest({0, 5});
		u.shuffle(ShuffleOptions(), 3);
		after = sources(u);
		ok(after[0] == 6 && after[5] == 1 && after[1] == 2 && after[4] == 5,
		   "with two selected, only those two trade places");

		TrackEditor one;
		one.setSegments({cut(1, 1000)});
		ok(one.shuffle(ShuffleOptions()) == 0, "a single cut has nothing to shuffle");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
