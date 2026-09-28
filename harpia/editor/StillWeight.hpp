#pragma once

// How heavy a still is for the editor, and what size of it the preview needs.
//
// A phone photo is 12 MP; a camera RAW export or a stitched panorama can be
// 50-200 MP. Kept whole, one of those is hundreds of MB of pixels, and the
// preview used to hand the whole thing to the compositor on every frame --
// scaled down to a few hundred pixels wide every time, and run through every
// effect on the clip at full size first. That is the lag.
//
// So the window keeps a WORKING copy capped at kStillWorkingMax on its long
// side (still far more than any preview shows, and enough for a 4K canvas),
// and the preview gets a copy sized to what it is drawing. The export reads
// the original file again at full resolution (StillImage.cpp), so nothing
// here changes what gets written -- only how much the editor carries around.
//
// Pure: no widgets, no files. The window decides what to show; this decides
// what counts as large and what size to scale to.

#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace harpia {

// The long side of the copy the editor keeps in memory.
inline constexpr int kStillWorkingMax = 4096;

// Past either of these the user is told (once) that the image is heavy.
inline constexpr double kStillWarnMegapixels = 40.0; // e.g. 8000 x 5000
inline constexpr qint64 kStillWarnFileBytes = 40ll * 1024 * 1024;

struct StillWeight {
	bool heavy = false;
	double megapixels = 0.0;
	qint64 memoryBytes = 0; // decoded, 4 bytes a pixel
	QString why;            // one line: what makes it heavy, empty when it is not
};

inline StillWeight stillWeight(QSize px, qint64 fileBytes)
{
	StillWeight w;
	if (px.isEmpty())
		return w;
	w.megapixels = double(px.width()) * px.height() / 1e6;
	w.memoryBytes = qint64(px.width()) * px.height() * 4;
	QStringList reasons;
	if (w.megapixels > kStillWarnMegapixels)
		reasons << QStringLiteral("%1 x %2 px (%3 megapixels, about %4 MB once decoded)")
				   .arg(px.width())
				   .arg(px.height())
				   .arg(w.megapixels, 0, 'f', 0)
				   .arg(w.memoryBytes / (1024 * 1024));
	if (fileBytes > kStillWarnFileBytes)
		reasons << QStringLiteral("a %1 MB file").arg(fileBytes / (1024 * 1024));
	w.heavy = !reasons.isEmpty();
	w.why = reasons.join(QStringLiteral(", "));
	return w;
}

// `full` scaled to fit inside `bound`, never enlarged. Aspect is kept; the
// result is at least 1 x 1.
inline QSize stillFitSize(QSize full, QSize bound)
{
	if (full.isEmpty() || bound.isEmpty())
		return full;
	if (full.width() <= bound.width() && full.height() <= bound.height())
		return full;
	return full.scaled(bound, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

// The working copy's size: capped at kStillWorkingMax on the long side.
inline QSize stillWorkingSize(QSize full)
{
	return stillFitSize(full, QSize(kStillWorkingMax, kStillWorkingMax));
}

// The preview copy's size for a preview rendering at `render`. Half as much
// again as the render size, so a clip zoomed in a little stays sharp; a clip
// zoomed further is the rare case, and a softer picture while framing it is a
// far better trade than lag on every frame of every still.
inline QSize stillPreviewSize(QSize full, QSize render)
{
	if (render.isEmpty())
		return full;
	const QSize bound(std::max(64, render.width() * 3 / 2), std::max(64, render.height() * 3 / 2));
	return stillFitSize(full, bound);
}

// A crop rectangle in the ORIGINAL's pixels, moved onto a copy of `copy`
// size. Frames reach the compositor smaller than their source (a still's
// preview copy, a video decoded at preview size), and a crop drawn from the
// full-size numbers would cut the wrong part of the smaller picture.
inline QRect cropForFrame(const QRect &crop, QSize original, QSize copy)
{
	if (crop.isNull() || original.isEmpty() || copy.isEmpty() || original == copy)
		return crop;
	const double sx = double(copy.width()) / original.width();
	const double sy = double(copy.height()) / original.height();
	const int x = int(std::floor(crop.x() * sx));
	const int y = int(std::floor(crop.y() * sy));
	const int r = int(std::ceil((crop.x() + crop.width()) * sx));
	const int b = int(std::ceil((crop.y() + crop.height()) * sy));
	return QRect(x, y, std::max(1, r - x), std::max(1, b - y));
}

} // namespace harpia
