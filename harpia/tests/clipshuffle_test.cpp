// Randomising clip order: many versions of one edit, fast, and always a Ctrl+Z
// from the last one.
//
// The pure part (ClipShuffle.hpp) is checked exhaustively enough to trust:
// every result is a permutation, pinned slots stay pinned, each strength moves
// about as much as it says, "avoid the same order" really avoids it, and the
// re-layout keeps every clip's own fields and every slot's timing. Then the
// widget: which lanes it touches, that a locked lane is left alone, that the
// selection follows its clips, and -- the point of the feature -- that each
// press is exactly one undo step and a press that changes nothing is none.
#include "editor/timeline/ClipShuffle.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QMouseEvent>

#include <cstdio>
#include <random>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static bool isPermutation(const QVector<int> &o)
{
	QVector<bool> seen(o.size(), false);
	for (int v : o) {
		if (v < 0 || v >= o.size() || seen[v])
			return false;
		seen[v] = true;
	}
	return true;
}

static TlClip clip(int source, qint64 start, qint64 len)
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = source;
	c.srcStartMs = 100 * source;
	c.srcEndMs = c.srcStartMs + len;
	c.outStartMs = start;
	c.volume = 0.5 + 0.01 * source;
	return c;
}

// n clips of varied lengths, laid end to end with a 250 ms gap after the third.
static QVector<TlClip> sequence(int n)
{
	QVector<TlClip> out;
	qint64 t = 1000;
	for (int i = 0; i < n; ++i) {
		const qint64 len = 1000 + 300 * (i % 4);
		out.append(clip(i + 1, t, len));
		t += len + (i == 2 ? 250 : 0);
	}
	return out;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	std::mt19937_64 rng(12345);

	std::printf("\n-- every result is a permutation, and pins hold --\n");
	{
		bool allPerm = true, pinsHeld = true;
		for (int trial = 0; trial < 200; ++trial) {
			const int n = 2 + trial % 12;
			ShuffleOptions o;
			o.keep = ShuffleOptions::Keep(trial % 4);
			o.strength = ShuffleOptions::Strength(trial % 3);
			const QVector<bool> mv = movableSlots(n, o, {});
			const QVector<int> ord = shuffledOrder(mv, o, rng);
			allPerm = allPerm && isPermutation(ord) && ord.size() == n;
			for (int k = 0; k < n; ++k)
				if (!mv[k] && ord[k] != k)
					pinsHeld = false;
		}
		ok(allPerm, "200 shuffles of 2..13 clips at every keep/strength are all permutations");
		ok(pinsHeld, "and a pinned slot (first, last, both) never moves");
	}

	std::printf("\n-- the strengths mean what they say --\n");
	{
		const int n = 30;
		double lowSum = 0, medSum = 0, highSum = 0;
		int trials = 300;
		for (int i = 0; i < trials; ++i) {
			ShuffleOptions o;
			const QVector<bool> mv(n, true);
			o.strength = ShuffleOptions::Strength::Low;
			lowSum += displacedSlots(shuffledOrder(mv, o, rng));
			o.strength = ShuffleOptions::Strength::Medium;
			medSum += displacedSlots(shuffledOrder(mv, o, rng));
			o.strength = ShuffleOptions::Strength::High;
			highSum += displacedSlots(shuffledOrder(mv, o, rng));
		}
		const double low = lowSum / trials, med = medSum / trials, high = highSum / trials;
		std::printf("     of 30 clips, displaced on average: low %.1f, medium %.1f, high %.1f\n", low,
			    med, high);
		ok(low >= 2 && low <= 8, "Low trades a few neighbours (about a fifth of the clips)");
		ok(med > low && med <= 20, "Medium moves about half");
		ok(high > med && high >= 26, "High moves nearly everything (a full Fisher-Yates)");
	}

	std::printf("\n-- avoid the same order --\n");
	{
		// Three clips at High: a plain shuffle hands the identity back 1 in 6.
		ShuffleOptions o;
		o.avoidSameOrder = true;
		int same = 0;
		for (int i = 0; i < 300; ++i)
			if (displacedSlots(shuffledOrder(QVector<bool>(3, true), o, rng)) < 2)
				++same;
		ok(same == 0, "with it on, 300 shuffles of three clips never return the same order");
		o.avoidSameOrder = false;
		same = 0;
		for (int i = 0; i < 300; ++i)
			if (displacedSlots(shuffledOrder(QVector<bool>(3, true), o, rng)) == 0)
				++same;
		ok(same > 10, "with it off, the same order does come back now and then (the control)");
		// Two movable clips: the only other order is the swap, so it is found.
		o.avoidSameOrder = true;
		ok(displacedSlots(shuffledOrder(QVector<bool>(2, true), o, rng)) == 2,
		   "two movable clips swap rather than the retry loop giving up");
		ok(displacedSlots(shuffledOrder(QVector<bool>(1, true), o, rng)) == 0 &&
			   shuffledOrder(QVector<bool>(), o, rng).isEmpty(),
		   "one clip, or none, is left alone");
	}

	std::printf("\n-- the re-layout keeps the clips and the slots' timing --\n");
	{
		const QVector<TlClip> before = sequence(8);
		QVector<int> order = {7, 0, 6, 1, 5, 2, 4, 3};
		const QVector<TlClip> after = applyOrder(before, order);
		bool fieldsKept = after.size() == 8;
		for (int k = 0; k < 8 && fieldsKept; ++k) {
			TlClip a = after[k], b = before[order[k]];
			a.outStartMs = b.outStartMs; // the one field allowed to differ
			fieldsKept = (a == b);
		}
		ok(fieldsKept, "every clip is the same clip apart from where it starts");
		ok(after[0].outStartMs == before[0].outStartMs, "the sequence still starts where it did");
		bool contiguous = true;
		for (int k = 0; k + 1 < 8; ++k) {
			const qint64 gap = after[k + 1].outStartMs - after[k].outEndMs();
			if (gap != (k == 2 ? 250 : 0))
				contiguous = false;
		}
		ok(contiguous, "clips still butt up, and the 250 ms gap is still after the third slot");
		qint64 sumBefore = 0, sumAfter = 0;
		for (int k = 0; k < 8; ++k) {
			sumBefore += before[k].outDurationMs();
			sumAfter += after[k].outDurationMs();
		}
		ok(sumBefore == sumAfter && after.last().outEndMs() == before.last().outEndMs(),
		   "so the edit is exactly as long as it was");

		// A transition overlap (negative gap) stays an overlap.
		QVector<TlClip> tr = sequence(3);
		tr[1].outStartMs -= 400; // overlaps the first by 400 ms
		tr[2].outStartMs -= 400;
		const QVector<TlClip> tra = applyOrder(tr, {2, 1, 0});
		ok(tra[1].outStartMs == tra[0].outEndMs() - 400, "a 400 ms overlap after slot 0 is still 400 ms");
	}

	std::printf("\n-- in the timeline: lanes, locks, selection, undo --\n");
	{
		TimelineView v;
		v.resize(900, 400);
		v.show();
		QApplication::processEvents();
		int commits = 0;
		QObject::connect(&v, &TimelineView::editCommitted, [&commits]() { ++commits; });

		TimelineModel m;
		TlTrack v2;
		v2.kind = TlTrack::Kind::Video;
		v2.name = QStringLiteral("V2");
		v2.clips = sequence(4);
		TlTrack v1;
		v1.kind = TlTrack::Kind::Video;
		v1.name = QStringLiteral("V1");
		v1.clips = sequence(12);
		TlTrack a1;
		a1.kind = TlTrack::Kind::Audio;
		a1.name = QStringLiteral("A1");
		a1.clips = sequence(5);
		m.tracks = {v2, v1, a1};
		v.setModel(m);

		const auto sourcesOf = [&v](int t) {
			QVector<int> s;
			for (const TlClip &c : v.model().tracks[t].clips)
				s.append(c.sourceId);
			return s;
		};
		const auto sortedByStart = [&v](int t) {
			const auto &cl = v.model().tracks[t].clips;
			for (int i = 0; i + 1 < cl.size(); ++i)
				if (cl[i].outStartMs > cl[i + 1].outStartMs)
					return false;
			return true;
		};

		ShuffleOptions o; // High, nothing kept, avoid same
		commits = 0;
		const int moved = v.shuffleClips(o, 777);
		std::printf("     nothing selected: %d clips moved\n", moved);
		ok(moved >= 2 + 8, "with nothing selected every video lane is shuffled");
		ok(sourcesOf(2) == sourcesOf(2) && v.model().tracks[2].clips[0].sourceId == 1 &&
			   v.model().tracks[2].clips[4].sourceId == 5,
		   "and the audio lane is not");
		ok(commits == 1, "one undo step for the whole press");
		ok(sortedByStart(0) && sortedByStart(1), "the clips vector is in slot order afterwards");

		// A locked lane is left alone even when it is the only target.
		TimelineModel lm = v.model();
		lm.tracks[0].locked = lm.tracks[1].locked = true;
		v.setModel(lm);
		commits = 0;
		ok(v.shuffleClips(o, 778) == 0, "every video lane locked: nothing moves");
		ok(commits == 0, "and no undo step is recorded for a press that changed nothing");
		lm.tracks[0].locked = lm.tracks[1].locked = false;
		v.setModel(lm);

		// Keep first & last on the long lane, by header selection.
		{
			const QRect h = v.headerToggleRectForTest(1, 0);
			// Select V1's header (a click away from its toggles).
			QMouseEvent pr(QEvent::MouseButtonPress, QPointF(v.contentRectForTest().x() - 6, h.top() - 12),
				       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
			QApplication::sendEvent(&v, &pr);
			QMouseEvent rl(QEvent::MouseButtonRelease, pr.position(), Qt::LeftButton, Qt::NoButton,
				       Qt::NoModifier);
			QApplication::sendEvent(&v, &rl);
			ok(v.selectedHeaderTrack() == 1, "V1's header is selected");
			const QVector<int> beforeV2 = sourcesOf(0);
			const int first = v.model().tracks[1].clips.first().sourceId;
			const int last = v.model().tracks[1].clips.last().sourceId;
			ShuffleOptions k;
			k.keep = ShuffleOptions::Keep::FirstAndLast;
			commits = 0;
			ok(v.shuffleClips(k, 779) >= 8, "a header selection shuffles that lane");
			ok(sourcesOf(0) == beforeV2, "and only that lane");
			ok(v.model().tracks[1].clips.first().sourceId == first &&
				   v.model().tracks[1].clips.last().sourceId == last,
			   "the intro and the ending stayed put");
			ok(commits == 1, "one undo step");
		}

		// Selected clips only: the selection follows its clips.
		{
			v.setModel(v.model()); // clears the selection
			const auto &cl = v.model().tracks[1].clips;
			v.selectClip(1, 2);
			v.addToSelection(1, 5);
			v.addToSelection(1, 8);
			QVector<int> selSources;
			for (const auto &p : v.selectedPairs())
				selSources.append(cl[p.second].sourceId);
			std::sort(selSources.begin(), selSources.end());
			// Whatever three clips sit at those indices after the shuffles above.
			QVector<int> expect = {cl[2].sourceId, cl[5].sourceId, cl[8].sourceId};
			std::sort(expect.begin(), expect.end());
			ok(selSources.size() == 3 && selSources == expect, "three clips selected on V1");

			QVector<int> othersBefore;
			for (int i = 0; i < cl.size(); ++i)
				if (i != 2 && i != 5 && i != 8)
					othersBefore.append(cl[i].sourceId);
			ShuffleOptions s;
			s.selectedOnly = true;
			const int mv = v.shuffleClips(s, 780);
			ok(mv == 2 || mv == 3, "only the selected clips changed places");
			const auto &cl2 = v.model().tracks[1].clips;
			QVector<int> othersAfter;
			for (int i = 0; i < cl2.size(); ++i)
				if (i != 2 && i != 5 && i != 8)
					othersAfter.append(cl2[i].sourceId);
			ok(othersAfter == othersBefore, "every unselected clip is in the slot it had");
			QVector<int> selAfter;
			for (const auto &p : v.selectedPairs())
				selAfter.append(cl2[p.second].sourceId);
			std::sort(selAfter.begin(), selAfter.end());
			ok(selAfter == expect, "and the selection still names the same three clips");
		}
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
