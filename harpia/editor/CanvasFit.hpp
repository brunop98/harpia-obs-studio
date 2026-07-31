#pragma once

// Fitting a source of one shape into a canvas of another, without stretching.
//
// Multi-Cut assembles cuts from several files into ONE output, and that output
// has one resolution: the primary source's. A cut from a differently-shaped
// file is scaled down to fit inside that canvas and centred, with bars either
// side -- which is exactly what runVideoCutsMulti() does when it encodes.
//
// This exists because the PREVIEW was not doing the same thing. It sized itself
// to whichever source was selected, so adding a second file of another
// resolution re-shaped the canvas and drew the first file's frames stretched
// into the new shape. The picture on screen stopped describing the file that
// would come out.
//
// One copy of the arithmetic, in a header both sides can reach, because "the
// preview matches the export" is only true while the two agree about this.

#include <QImage>
#include <QPainter>
#include <QRect>
#include <QSize>

#include <algorithm>
#include <cmath>

namespace harpia {

// Where a `srcSize` picture sits inside `canvas`: scaled by the smaller of the
// two ratios, centred, never enlarged past the canvas. Empty when either side
// has no area.
inline QRect fitRectInCanvas(QSize srcSize, QSize canvas)
{
	if (srcSize.width() <= 0 || srcSize.height() <= 0 || canvas.width() <= 0 ||
	    canvas.height() <= 0)
		return QRect();
	// The same std::min the exporter uses. Deliberately not QSize::scaled(): the
	// rounding has to match, or a preview would be a pixel off the encode.
	const double sc = std::min(double(canvas.width()) / srcSize.width(),
				   double(canvas.height()) / srcSize.height());
	const int w = std::max(1, int(srcSize.width() * sc));
	const int h = std::max(1, int(srcSize.height() * sc));
	return QRect((canvas.width() - w) / 2, (canvas.height() - h) / 2, w, h);
}

// True when `srcSize` already fills `canvas` -- the common case of every clip
// being the same shape, where fitting is a copy nobody needs.
inline bool fillsCanvas(QSize srcSize, QSize canvas)
{
	const QRect r = fitRectInCanvas(srcSize, canvas);
	return !r.isEmpty() && r.size() == canvas;
}

// `src` letterboxed onto a canvas-sized image. Returns `src` untouched when it
// already fits exactly, so the ordinary single-resolution project pays nothing.
inline QImage fitIntoCanvas(const QImage &src, QSize canvas)
{
	if (src.isNull() || canvas.width() <= 0 || canvas.height() <= 0)
		return src;
	// Compared by ASPECT, not by pixel size: the preview decodes at a reduced
	// resolution, so a same-shaped source is never literally canvas-sized here.
	const double srcAr = double(src.width()) / std::max(1, src.height());
	const double canAr = double(canvas.width()) / std::max(1, canvas.height());
	// Relative, and loose enough for the rounding a reduced decode carries:
	// 854x480 is the usual 480p spelling of 16:9 and is 0.0012 off it, so a
	// tight tolerance would rebuild a full canvas-sized image, with a smooth
	// rescale, for every frame of an ordinary same-shaped project. Nowhere near
	// loose enough to let 4:3 (1.333 vs 1.778) through.
	if (std::abs(srcAr - canAr) <= 0.005 * canAr)
		return src; // same shape: drawing it into the canvas is already right

	QImage out(canvas, QImage::Format_RGBA8888);
	out.fill(Qt::black); // the bars, matching what the encoder writes
	QPainter p(&out);
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);
	p.drawImage(fitRectInCanvas(src.size(), canvas), src);
	return out;
}

// A reduced version of `canvas` for rendering into, no wider than `wantW`, with
// the SHAPE kept exactly.
//
// The preview renders at the size the widget can actually show rather than the
// project size, which on a 4K project saves most of the pixels for no visible
// difference. The subtlety is the floor: clamping width and height against
// separate minimums silently changes the aspect, and a 720x1280 project
// squeezed into a narrow panel came out 160x256 -- 0.625 where the project is
// 0.5625, i.e. the picture stretched. So the floor is applied to the SCALE,
// once, and both sides follow it.
inline QSize reducedRenderSize(QSize canvas, int wantW, QSize minSize = QSize(160, 90))
{
	if (canvas.width() <= 0 || canvas.height() <= 0)
		return canvas;
	if (wantW <= 0 || wantW >= canvas.width())
		return canvas; // never enlarge: there is nothing to gain
	double k = double(wantW) / canvas.width();
	// The smallest scale at which BOTH sides still clear the minimum. Capped at
	// 1 so a canvas already below the minimum is left alone rather than blown up.
	const double kMin = std::min(1.0, std::max(double(minSize.width()) / canvas.width(),
						   double(minSize.height()) / canvas.height()));
	k = std::max(k, kMin);
	return QSize(std::max(2, int(std::lround(canvas.width() * k))),
		     std::max(2, int(std::lround(canvas.height() * k))));
}

} // namespace harpia
