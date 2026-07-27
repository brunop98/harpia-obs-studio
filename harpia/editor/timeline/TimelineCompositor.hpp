#pragma once

// Renders one output frame of the multi-track timeline.
//
// The SAME compositor drives the live preview (GUI thread) and the exporter
// (worker thread) — QImage/QPainter need no GUI — so what you see in the editor
// is what gets encoded. Video tracks composite bottom-to-top (a later track
// index draws on top); text clips are drawn with QPainter using the same
// transform system, so they zoom/move/animate exactly like video clips.

#include "TimelineModel.hpp"

#include <QImage>
#include <QRectF>
#include <QSize>

class QPainter;

namespace harpia {

class TimelineCompositor {
public:
	// Supplies decoded source frames. The preview implementation seeks the
	// window's FrameSeekers; the exporter uses its own worker-side decoders.
	class FrameProvider {
	public:
		virtual ~FrameProvider() = default;
		// RGB(A) frame for a source at a source-time, or a null QImage.
		virtual QImage frameFor(int sourceId, qint64 srcMs) = 0;
	};

	// Compose every visible track at `outMs` onto a `canvas`-sized RGBA image.
	// `eval` (optional) applies each clip's transform script on top of its base
	// pose / keyframes. A QJSEngine isn't thread-safe, so pass the evaluator
	// belonging to the CALLING thread (the preview's, or the export worker's).
	static QImage compose(const TimelineModel &m, qint64 outMs, QSize canvas, FrameProvider &fp,
			      class TransformEvaluator *eval = nullptr, double fps = 30.0);

	// Where a clip is drawn on the canvas at this pose, in canvas pixels. Used by
	// the preview to hit-test and drag the selected clip. `srcSize` is the clip's
	// natural pixel size (its cropped source size, or the measured text size).
	static QRectF clipRectOnCanvas(const TlTransform &tf, QSize canvas, QSize srcSize);

	// Natural (unscaled) size a text clip occupies on a given canvas.
	static QSize textNaturalSize(const TlText &t, QSize canvas);

	// Draw one clip onto an open painter over `canvas`. Exposed so the preview can
	// re-render a single clip (e.g. selection outlines) with the same math.
	static void drawClip(QPainter &p, const TlClip &c, const TlTransform &tf, QSize canvas,
			     const QImage &sourceFrame);

private:
	static void drawTextClip(QPainter &p, const TlClip &c, const TlTransform &tf, QSize canvas);
};

} // namespace harpia
