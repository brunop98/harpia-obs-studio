#pragma once

// Laying out a filmstrip so no frame is ever shown twice.
//
// A source gets a fixed number of thumbnails (sixty), spread evenly over its
// duration. Every timeline drew them by stepping across the bar in fixed tile
// widths and asking "which thumbnail covers this pixel?" -- which is right
// while a tile is narrower than a thumbnail's slice of time, and wrong the
// moment you zoom past that. Zooming in widens each slice, so several
// consecutive tiles fall inside one slice and the SAME FRAME is drawn three,
// five, ten times in a row. A strip of identical pictures is worse than no
// strip: it looks like the video is frozen there.
//
// Two regimes, one rule -- a frame appears at most once:
//
//   Zoomed out (a slice is narrower than a tile): keep the fixed grid, which is
//   anchored to the clip so tiles do not slide about as the view scrolls, and
//   skip any tile that would repeat its predecessor.
//
//   Zoomed in (a slice is wider than a tile): stop stepping by tiles and step
//   by THUMBNAIL. Each one is drawn once, centred on the moment it was actually
//   taken from -- (i + 0.5) of its slice, which is where TimelineThumbs seeks.
//   The gaps that opens up are honest: they are the resolution the strip has.
//
// Pure geometry, no painting, so the rule is one function that all three
// timelines call and a test can check without a window.

#include <QRect>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace harpia {

// One thumbnail to draw, and the x it goes at. Width is the caller's tileW.
struct StripTile {
	int index = 0;
	int x = 0;
};

// Tiles for the pixel range [fromX, toX) of an area that spans `areaX`..
// `areaX + areaW` and shows `srcLenMs` of a source starting at `srcStartMs`.
// `count` thumbnails cover `sourceDurMs` evenly.
inline QVector<StripTile> filmstripTiles(int count, qint64 sourceDurMs, qint64 srcStartMs,
					 qint64 srcLenMs, int areaX, int areaW, int tileW, int gap,
					 int fromX, int toX)
{
	QVector<StripTile> out;
	if (count <= 0 || areaW <= 0 || tileW <= 0 || srcLenMs <= 0 || sourceDurMs <= 0 || toX <= fromX)
		return out;

	const double sliceMs = double(sourceDurMs) / double(count);
	const double pxPerMs = double(areaW) / double(srcLenMs);
	const double pxPerSlice = sliceMs * pxPerMs;
	const int step = tileW + std::max(0, gap);

	// The thumbnails whose slice OVERLAPS what this clip contains. A trimmed
	// clip must not show a frame from footage it does not have -- and the pixel
	// at its right edge maps to the first instant AFTER the clip, so without
	// this the last tile came from the next slice along.
	const int iFirst = std::clamp(int(double(srcStartMs) / sliceMs), 0, count - 1);
	const int iLast = std::clamp(int(double(srcStartMs + srcLenMs - 1) / sliceMs), 0, count - 1);

	if (pxPerSlice <= double(step)) {
		// Zoomed out: the fixed grid is still finer than the strip, so it is
		// already showing different frames. Anchored to areaX rather than to the
		// viewport, or every scroll would shuffle the tiles sideways.
		const int first = areaX + std::max(0, (fromX - areaX) / step) * step;
		int last = -1;
		for (int x = first; x < toX; x += step) {
			const double f =
				std::clamp((double(x) + tileW / 2.0 - areaX) / double(areaW), 0.0, 1.0);
			const qint64 ms = srcStartMs + qint64(f * double(srcLenMs));
			const int i = std::clamp(int(double(ms) / sliceMs), iFirst, iLast);
			if (i == last)
				continue; // the rule, even here: never the same frame twice
			last = i;
			out.push_back({i, x});
		}
		return out;
	}

	// Zoomed in: one tile per thumbnail, at the moment it was taken from.
	const qint64 lastVisMs = srcStartMs + qint64(double(toX - areaX) / pxPerMs);
	const qint64 firstVisMs = srcStartMs + qint64(double(fromX - areaX) / pxPerMs);
	const int i0 = std::clamp(int(double(firstVisMs) / sliceMs) - 1, iFirst, iLast);
	const int i1 = std::clamp(int(double(lastVisMs) / sliceMs) + 1, iFirst, iLast);
	for (int i = i0; i <= i1; ++i) {
		// Where the frame belongs: TimelineThumbs seeks to the middle of each
		// slice, so that is the moment the picture actually shows.
		const double centreMs = (double(i) + 0.5) * sliceMs;
		int cx = areaX + int(std::lround((centreMs - double(srcStartMs)) * pxPerMs));
		// A clip shorter than one slice has exactly one frame available and its
		// midpoint can fall outside; pull it in rather than draw nothing, since
		// the nearest frame is still a better answer than an empty strip.
		cx = std::clamp(cx, areaX + tileW / 2, areaX + areaW - tileW / 2);
		out.push_back({i, cx - tileW / 2});
	}
	return out;
}

} // namespace harpia
