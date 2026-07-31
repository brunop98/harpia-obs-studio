// The filmstrip must never show the same frame twice.
//
// A source gets sixty thumbnails spread over its duration. All three timelines
// drew them by stepping across the bar in fixed tile widths and asking which
// thumbnail covered each pixel -- correct while a tile is wider than a
// thumbnail's slice of time, and wrong the moment you zoom past that. Zooming
// in widens the slice, several consecutive tiles land inside one, and the same
// picture is drawn five or ten times running. It reads as though the video is
// frozen there.
//
// So the property under test is a property of the WHOLE LAYOUT, not of any one
// tile: across everything drawn, no index appears twice. Checked at a range of
// zooms, because the bug only exists past a threshold and a single zoom level
// would have missed it -- which is exactly how it shipped.
#include "editor/Filmstrip.hpp"

#include <QSet>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// How many times the most-repeated index appears.
static int worstRepeat(const QVector<StripTile> &tiles)
{
	QHash<int, int> seen;
	int worst = 0;
	for (const StripTile &t : tiles)
		worst = std::max(worst, ++seen[t.index]);
	return worst;
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const int count = 60;              // what TimelineThumbs produces
	const qint64 dur = 10 * 60 * 1000; // a ten-minute source
	const int areaW = 900;             // the bar, in pixels
	const int tileW = 64, gap = 1;

	std::printf("\n-- no frame appears twice, at any zoom --\n");
	{
		// From fully zoomed out to 500x. At 1x a slice is 15 px and the grid is
		// coarser than the strip; by 10x a slice is 150 px and the old code drew
		// the same frame twice or three times over.
		bool everRepeated = false;
		int worstSeen = 0;
		for (double zoom : {1.0, 2.0, 4.0, 8.0, 16.0, 40.0, 100.0, 250.0, 500.0}) {
			const qint64 visible = std::max<qint64>(1, qint64(dur / zoom));
			for (int startPct = 0; startPct <= 80; startPct += 20) {
				const qint64 start = (dur - visible) * startPct / 100;
				const auto tiles = filmstripTiles(count, dur, start, visible, 0, areaW,
								  tileW, gap, 0, areaW);
				const int w = worstRepeat(tiles);
				worstSeen = std::max(worstSeen, w);
				if (w > 1) {
					everRepeated = true;
					std::printf("     REPEAT at zoom %.0fx start %d%%: one frame "
						    "drawn %d times\n",
						    zoom, startPct, w);
				}
			}
		}
		std::printf("     worst repeat across 45 layouts: %d\n", worstSeen);
		ok(!everRepeated, "no layout draws any frame more than once");
	}

	std::printf("\n-- CONTROL: the old fixed-step layout really did repeat --\n");
	{
		// Without this the section above proves nothing about the bug: it has to
		// be shown that these inputs can tell the two apart. This is the loop
		// that shipped, transcribed.
		const double zoom = 40.0;
		const qint64 visible = qint64(dur / zoom);
		const double sliceMs = double(dur) / count;
		const int step = tileW + gap;
		QHash<int, int> seen;
		int worst = 0;
		for (int x = 0; x < areaW; x += step) {
			const double f = double(x + tileW / 2) / areaW;
			const qint64 ms = qint64(f * double(visible));
			const int i = std::clamp(int(double(ms) / sliceMs), 0, count - 1);
			worst = std::max(worst, ++seen[i]);
		}
		std::printf("     old layout at 40x: one frame drawn %d times\n", worst);
		ok(worst > 1, "CONTROL: the previous arithmetic repeats at this zoom");
	}

	std::printf("\n-- zoomed out, it still fills the bar --\n");
	{
		// The fix must not make the ordinary case sparse. At 1x a ten-minute
		// source across 900 px has a slice every 15 px, far finer than a 64 px
		// tile, so the strip should be as full as it ever was.
		const auto tiles = filmstripTiles(count, dur, 0, dur, 0, areaW, tileW, gap, 0, areaW);
		const int step = tileW + gap;
		std::printf("     1x -> %d tiles across %d px (a full grid would be %d)\n",
			    int(tiles.size()), areaW, areaW / step);
		ok(tiles.size() >= areaW / step - 1, "the grid is still full when zoomed out");
		ok(worstRepeat(tiles) == 1, "and still has no duplicates");
		// Left to right, no going back: tiles are laid out in time order.
		bool ordered = true;
		for (int i = 1; i < tiles.size(); ++i)
			if (tiles[i].x <= tiles[i - 1].x || tiles[i].index <= tiles[i - 1].index)
				ordered = false;
		ok(ordered, "and runs left to right in time order");
	}

	std::printf("\n-- zoomed in, each frame sits where it was taken --\n");
	{
		// 60x: a slice is 900 px, wider than the whole bar, so at most a frame or
		// two is visible -- and each must sit at ITS moment, not at a grid
		// position, or the strip would lie about when that frame happens.
		const double zoom = 60.0;
		const qint64 visible = qint64(dur / zoom);
		const qint64 start = dur / 2;
		const auto tiles =
			filmstripTiles(count, dur, start, visible, 0, areaW, tileW, gap, 0, areaW);
		std::printf("     %.0fx -> %d tile(s) for a %lld ms window\n", zoom, int(tiles.size()),
			    (long long)visible);
		ok(worstRepeat(tiles) <= 1, "no repeats");
		bool placedRight = true;
		for (const StripTile &t : tiles) {
			// Where the thumbnail's own moment falls in the bar.
			const double centreMs = (t.index + 0.5) * (double(dur) / count);
			const int want = int((centreMs - start) * areaW / double(visible)) - tileW / 2;
			if (std::abs(t.x - want) > 2)
				placedRight = false;
		}
		ok(placedRight, "each is centred on the moment it was taken from");
	}

	std::printf("\n-- a trimmed clip only shows its own footage --\n");
	{
		// The clip covers the middle fifth of the source. Thumbnails from
		// outside that are frames this clip does not contain.
		const qint64 clipStart = dur * 2 / 5, clipLen = dur / 5;
		const auto tiles =
			filmstripTiles(count, dur, clipStart, clipLen, 0, 400, tileW, gap, 0, 400);
		// The right criterion is that the frame's SLICE overlaps the clip, not
		// that its midpoint is inside: a clip shorter than one slice has only
		// one frame available, whose midpoint may well sit outside it, and the
		// nearest frame beats an empty strip.
		const double sliceMs = double(dur) / count;
		bool inside = true;
		for (const StripTile &t : tiles) {
			const double from = t.index * sliceMs, to = from + sliceMs;
			if (to <= clipStart || from >= clipStart + clipLen)
				inside = false;
		}
		std::printf("     middle fifth -> %d tiles, indices %d..%d\n", int(tiles.size()),
			    tiles.isEmpty() ? -1 : tiles.front().index,
			    tiles.isEmpty() ? -1 : tiles.back().index);
		ok(!tiles.isEmpty(), "it draws something");
		ok(inside, "and only frames that fall inside the trimmed range");
		ok(worstRepeat(tiles) <= 1, "still no repeats");
	}

	std::printf("\n-- nothing to draw --\n");
	{
		ok(filmstripTiles(0, dur, 0, dur, 0, areaW, tileW, gap, 0, areaW).isEmpty(),
		   "no thumbnails, no tiles");
		ok(filmstripTiles(count, 0, 0, dur, 0, areaW, tileW, gap, 0, areaW).isEmpty(),
		   "a source with no duration draws nothing");
		ok(filmstripTiles(count, dur, 0, dur, 0, 0, tileW, gap, 0, areaW).isEmpty(),
		   "nor does an area with no width");
		ok(filmstripTiles(count, dur, 0, dur, 0, areaW, tileW, gap, 50, 50).isEmpty(),
		   "nor an empty visible range");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
