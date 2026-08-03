// Handing a Trim / Multi-Cut result to Full editing.
//
// The point of the feature is that NOTHING is lost on the way across: every
// cut boundary survives as its own clip (so transitions have two clips to sit
// between, and every edit point is still grabbable), and the assembled result
// plays back exactly as it did in Multi-Cut.
//
// The part that rots silently is the output-time arithmetic. A mis-accumulated
// offset leaves a gap or an overlap that looks like a decoding fault rather
// than an off-by-one -- so the joints are what this test is mostly about.
#include "editor/SendToFull.hpp"

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static CutSegment seg(int src, qint64 a, qint64 b, double sp = 1.0)
{
	CutSegment s;
	s.sourceId = src;
	s.srcStartMs = a;
	s.srcEndMs = b;
	s.speed = sp;
	return s;
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::printf("\n-- one clip per cut, not one merged clip --\n");
	{
		const QVector<CutSegment> cuts = {seg(1, 1000, 3000), seg(1, 8000, 9500),
						  seg(1, 20000, 21000)};
		const QVector<TlClip> got = clipsFromSegments(cuts, 0);
		std::printf("     3 cuts -> %d clip(s)\n", int(got.size()));
		ok(got.size() == 3, "three cuts arrive as three clips");
		// Collapsing them into one would throw away the edit points Multi-Cut
		// just made, and leave nothing for a transition to sit between.
		ok(got[0].srcStartMs == 1000 && got[0].srcEndMs == 3000, "the first keeps its own range");
		ok(got[2].srcStartMs == 20000 && got[2].srcEndMs == 21000, "and so does the last");
	}

	std::printf("\n-- the joints: butt-jointed, no gaps, no overlaps --\n");
	{
		const QVector<TlClip> got =
			clipsFromSegments({seg(1, 0, 2000), seg(1, 5000, 5500), seg(1, 9000, 12000)}, 0);
		ok(got[0].outStartMs == 0, "the first starts at zero");
		for (int i = 1; i < got.size(); ++i) {
			const qint64 prevEnd = got[i - 1].outStartMs + got[i - 1].outDurationMs();
			std::printf("     clip %d ends at %lld, clip %d starts at %lld\n", i - 1,
				    (long long)prevEnd, i, (long long)got[i].outStartMs);
			ok(got[i].outStartMs == prevEnd, "each clip starts exactly where the last ended");
		}
		const qint64 total = sentDurationMs(got);
		ok(total == 2000 + 500 + 3000, "and the whole thing runs as long as the cuts did");
	}

	std::printf("\n-- speed shortens the OUTPUT, not the source range --\n");
	{
		const QVector<TlClip> got = clipsFromSegments({seg(1, 0, 4000, 2.0), seg(1, 4000, 5000)}, 0);
		std::printf("     a 4 s cut at 2x occupies %lld ms of timeline\n",
			    (long long)got[0].outDurationMs());
		ok(got[0].srcEndMs - got[0].srcStartMs == 4000, "the source range is untouched");
		ok(got[0].outDurationMs() == 2000, "but it takes half the time on the timeline");
		ok(got[0].speed == 2.0, "and the clip carries the speed, so playback matches");
		// The joint has to use the SPED-UP length, or everything after a
		// non-1x cut sits in the wrong place.
		ok(got[1].outStartMs == 2000, "the next clip starts after the sped-up length");
	}

	std::printf("\n-- appending after existing work --\n");
	{
		// The chosen behaviour: sending never lands on top of what is already
		// in Full editing. Sending twice piles up -- visible and undoable.
		const QVector<TlClip> got = clipsFromSegments({seg(1, 0, 1000), seg(1, 2000, 3000)}, 7500);
		ok(got[0].outStartMs == 7500, "the first clip starts where the timeline ended");
		ok(got[1].outStartMs == 8500, "and the rest follow on from there");
	}

	std::printf("\n-- a multi-source Multi-Cut keeps its sources --\n");
	{
		const QVector<TlClip> got =
			clipsFromSegments({seg(1, 0, 1000), seg(4, 500, 1500), seg(1, 9000, 9200)}, 0);
		ok(got[0].sourceId == 1 && got[1].sourceId == 4 && got[2].sourceId == 1,
		   "each clip points at the source its cut came from");
	}

	std::printf("\n-- Trim sends one clip, with its crop --\n");
	{
		const QRect crop(100, 50, 640, 360);
		const QVector<TlClip> got = clipsFromTrim(3, 2000, 6000, 1.0, true, crop, 0);
		ok(got.size() == 1, "a trim is one clip");
		ok(got[0].sourceId == 3 && got[0].srcStartMs == 2000 && got[0].srcEndMs == 6000,
		   "carrying the in and out points");
		ok(got[0].crop == crop, "and the crop rectangle, which is source pixels in both models");

		// CONTROL: crop OFF must not smuggle a stale rectangle across -- that
		// would silently reframe the video on arrival.
		const QVector<TlClip> noCrop = clipsFromTrim(3, 2000, 6000, 1.0, false, crop, 0);
		ok(noCrop[0].crop.isNull(), "CONTROL: with crop off, no crop travels");
	}

	std::printf("\n-- degenerate input --\n");
	{
		ok(clipsFromSegments({}, 0).isEmpty(), "no cuts, no clips");
		// A zero-length cut would arrive as a clip too small to see or grab.
		const QVector<TlClip> got =
			clipsFromSegments({seg(1, 100, 100), seg(1, 0, 500), seg(1, 800, 700)}, 0);
		std::printf("     3 cuts, 2 of them empty/backwards -> %d clip(s)\n", int(got.size()));
		ok(got.size() == 1, "empty and backwards cuts are skipped, not emitted");
		ok(got[0].outStartMs == 0, "and the survivor still starts at the beginning");
		ok(sentDurationMs({}) == 0, "an empty send runs for no time");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
