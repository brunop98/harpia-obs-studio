#include "ZoomKeyframes.hpp"

#include <algorithm>
#include <cmath>

namespace harpia {

TlTransform zoomPoseFor(QSize canvas, QSize srcSize, QPointF pointInClip, double scale)
{
	TlTransform tf; // 0.5/0.5, scale 1
	if (canvas.width() <= 0 || canvas.height() <= 0 || srcSize.width() <= 0 ||
	    srcSize.height() <= 0)
		return tf;

	const double s = std::max(0.001, scale);
	tf.scale = s;

	// Same fit the compositor does, so the arithmetic here describes the picture
	// that actually gets drawn rather than a second opinion about it.
	const double fit = std::min(double(canvas.width()) / srcSize.width(),
				    double(canvas.height()) / srcSize.height());
	const double w = srcSize.width() * fit * s;  // clip width on the canvas
	const double h = srcSize.height() * fit * s;

	const double u = std::clamp(pointInClip.x(), 0.0, 1.0);
	const double v = std::clamp(pointInClip.y(), 0.0, 1.0);

	// The clip is drawn centred at (posX*W, posY*H). For the point (u,v) inside
	// it to land at the canvas centre, the centre has to move the other way by
	// however far that point is from the middle of the clip.
	double cx = canvas.width() / 2.0 + w * (0.5 - u);
	double cy = canvas.height() / 2.0 + h * (0.5 - v);

	// Keep covering the canvas, on each axis that already did. cx <= w/2 keeps
	// the left edge off-screen; cx >= W - w/2 keeps the right edge off.
	if (w >= canvas.width())
		cx = std::clamp(cx, canvas.width() - w / 2.0, w / 2.0);
	if (h >= canvas.height())
		cy = std::clamp(cy, canvas.height() - h / 2.0, h / 2.0);

	tf.posX = cx / canvas.width();
	tf.posY = cy / canvas.height();
	return tf;
}

QPointF clipPointFromCanvas(const TlTransform &current, QSize canvas, QSize srcSize,
			    QPointF canvasNorm)
{
	if (canvas.width() <= 0 || canvas.height() <= 0 || srcSize.width() <= 0 ||
	    srcSize.height() <= 0)
		return QPointF(0.5, 0.5);

	// The same rect the compositor draws the clip into, so this inverts the
	// real layout rather than a second opinion about it.
	const double fit = std::min(double(canvas.width()) / srcSize.width(),
				    double(canvas.height()) / srcSize.height());
	const double s = fit * std::max(0.001, current.scale);
	const double w = srcSize.width() * s;
	const double h = srcSize.height() * s;
	if (w <= 0.0 || h <= 0.0)
		return QPointF(0.5, 0.5);
	const double left = current.posX * canvas.width() - w / 2.0;
	const double top = current.posY * canvas.height() - h / 2.0;

	const double px = canvasNorm.x() * canvas.width();
	const double py = canvasNorm.y() * canvas.height();
	// Clamped: a click on the letterbox bars is outside the picture, and the
	// nearest edge of it is the only sensible reading of what was meant.
	return QPointF(std::clamp((px - left) / w, 0.0, 1.0),
		       std::clamp((py - top) / h, 0.0, 1.0));
}

bool addZoomAt(TlClip &clip, qint64 atOutMs, QPointF pointInClip, QSize canvas, QSize srcSize,
	       const ZoomSettings &settings)
{
	const qint64 dur = clip.outDurationMs();
	const qint64 lead = std::max<qint64>(1, settings.leadInMs);
	const qint64 hold = std::max<qint64>(0, settings.holdMs);
	const qint64 out = std::max<qint64>(1, settings.outMs);
	if (dur < lead + out)
		return false; // no room to arrive and leave

	// Clip-relative, because that is what the four keys are measured in.
	qint64 peak = std::clamp<qint64>(atOutMs - clip.outStartMs, 0, dur);

	// The envelope has to fit. Slide it inwards rather than truncating it: a
	// zoom clipped by the end of the clip ends mid-push, which reads as the
	// picture jumping back rather than settling.
	qint64 start = peak - lead;
	qint64 end = peak + hold + out;
	if (start < 0) {
		peak -= start; // shift right
		start = 0;
		end = peak + hold + out;
	}
	if (end > dur) {
		const qint64 over = end - dur;
		peak -= over;
		start = peak - lead;
		end = dur;
	}
	if (start < 0 || peak <= start || end <= peak)
		return false; // cannot be made to fit

	// The pose it comes FROM and returns TO: whatever the clip is already doing
	// there, so a zoom dropped onto an already-animated clip joins on rather
	// than yanking it back to the base pose.
	const TlTransform before = clip.transformAt(clip.outStartMs + start);
	const TlTransform after = clip.transformAt(clip.outStartMs + end);
	const TlTransform zoomed = zoomPoseFor(canvas, srcSize, pointInClip, settings.scale);

	// Position and scale only. Rotation and opacity are deliberately left
	// unpinned: keying them here would freeze whatever they happened to be and
	// quietly override a fade or a spin the clip already had.
	const int lanes = (1 << TlLanePos) | (1 << TlLaneScale);
	clip.setKeyframeAt(clip.outStartMs + start, before, settings.ease, lanes);
	clip.setKeyframeAt(clip.outStartMs + peak, zoomed, settings.ease, lanes);
	if (hold > 0)
		clip.setKeyframeAt(clip.outStartMs + peak + hold, zoomed, settings.ease, lanes);
	clip.setKeyframeAt(clip.outStartMs + end, after, settings.ease, lanes);
	clip.sortKeys();
	return true;
}

} // namespace harpia
