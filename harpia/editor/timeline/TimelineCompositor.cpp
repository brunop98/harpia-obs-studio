#include "TimelineCompositor.hpp"

#include "EffectClip.hpp"
#include "Transitions.hpp"
#include "../component/ComponentRegistry.hpp"
#include "../component/ComponentStack.hpp"
#include "Spotlight.hpp"

#include "../script/TransformScript.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QTransform>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QHash>
#include <QStringList>

#include <algorithm>

namespace harpia {

namespace {

// Text is authored against a 1080-tall canvas so a caption keeps its relative
// size when the output resolution changes.
constexpr double kTextRefHeight = 1080.0;

// Everything about a caption is authored against a 1080-tall canvas and scaled
// by this on the way out — the font, and (so they stay in proportion with it)
// the background's padding and corner radius.
double textScale(QSize canvas)
{
	return canvas.height() > 0 ? double(canvas.height()) / kTextRefHeight : 1.0;
}

QFont buildFont(const TlText &t, QSize canvas)
{
	QFont f;
	if (!t.fontFamily.isEmpty())
		f.setFamily(t.fontFamily);
	f.setPixelSize(std::max(4, int(std::llround(t.fontPx * textScale(canvas)))));
	f.setBold(t.bold);
	f.setItalic(t.italic);
	return f;
}

// Building a text path means shaping every glyph into outlines, and the outline
// pass runs a QPainterPathStroker over the result — easily the most expensive
// thing in a frame containing a caption, and it produced an identical path on
// every frame. Cache both against everything they depend on. A caption's style
// changes at human speed, so a handful of entries covers any real timeline.
struct TextPathKey {
	QString text, family;
	int px = 0, align = 0, canvasH = 0;
	bool bold = false, italic = false;

	bool operator==(const TextPathKey &o) const
	{
		return text == o.text && family == o.family && px == o.px && align == o.align &&
		       canvasH == o.canvasH && bold == o.bold && italic == o.italic;
	}
};

size_t qHash(const TextPathKey &k, size_t seed = 0)
{
	return qHashMulti(seed, k.text, k.family, k.px, k.align, k.canvasH, k.bold, k.italic);
}

struct CachedText {
	QPainterPath path;
	QRectF block;
	double strokeWidth = -1.0; // what `stroke` was built for (<0 = not built)
	QPainterPath stroke;
};

QStringList textLines(const TlText &t)
{
	QStringList lines = t.text.split(QLatin1Char('\n'));
	if (lines.isEmpty())
		lines << QString();
	return lines;
}

// Lay the (possibly multi-line) text out as a path centred on the origin, and
// report the block's bounding box.
QPainterPath buildTextPath(const TlText &t, const QFont &f, QRectF *blockOut)
{
	const QFontMetricsF fm(f);
	const QStringList lines = textLines(t);
	const double lineH = fm.height();
	double maxW = 0.0;
	for (const QString &l : lines)
		maxW = std::max(maxW, fm.horizontalAdvance(l));
	const double blockH = lineH * lines.size();

	QPainterPath path;
	double y = -blockH / 2.0 + fm.ascent();
	for (const QString &l : lines) {
		const double w = fm.horizontalAdvance(l);
		double x = -maxW / 2.0;            // left
		if (t.align == 1)                  // centre
			x = -w / 2.0;
		else if (t.align == 2)             // right
			x = maxW / 2.0 - w;
		path.addText(QPointF(x, y), f, l);
		y += lineH;
	}
	if (blockOut)
		*blockOut = QRectF(-maxW / 2.0, -blockH / 2.0, maxW, blockH);
	return path;
}

// The cache is per-thread: the preview composes on the GUI thread and the
// exporter on its worker, and QPainterPath is not shared safely between them.
CachedText &cachedText(const TlText &t, const QFont &f, QSize canvas)
{
	static thread_local QHash<TextPathKey, CachedText> cache;
	TextPathKey k;
	k.text = t.text;
	k.family = t.fontFamily;
	k.px = f.pixelSize();
	k.align = t.align;
	k.canvasH = canvas.height();
	k.bold = t.bold;
	k.italic = t.italic;

	auto it = cache.find(k);
	if (it == cache.end()) {
		if (cache.size() > 64)
			cache.clear(); // bounded; captions change at human speed
		CachedText ct;
		ct.path = buildTextPath(t, f, &ct.block);
		it = cache.insert(k, ct);
	}
	return it.value();
}

} // namespace

QSize TimelineCompositor::textNaturalSize(const TlText &t, QSize canvas)
{
	const QFont f = buildFont(t, canvas);
	const QRectF block = cachedText(t, f, canvas).block;
	// The natural size INCLUDES the background, so the preview's selection
	// outline and hit-testing wrap the sticker rather than just the glyphs.
	const double k = textScale(canvas);
	const double px = t.boxEnabled ? t.boxPadX * k * 2.0 : 0.0;
	const double py = t.boxEnabled ? t.boxPadY * k * 2.0 : 0.0;
	return QSize(std::max(1, int(std::ceil(block.width() + px))),
		     std::max(1, int(std::ceil(block.height() + py))));
}

QRectF TimelineCompositor::clipRectOnCanvas(const TlTransform &tf, QSize canvas, QSize srcSize)
{
	if (srcSize.width() <= 0 || srcSize.height() <= 0 || canvas.width() <= 0 || canvas.height() <= 0)
		return QRectF();
	// Fit the source inside the canvas (preserving aspect), then apply the pose's
	// zoom and centre it at the normalised position.
	const double fit = std::min(double(canvas.width()) / srcSize.width(),
				    double(canvas.height()) / srcSize.height());
	const double s = fit * std::max(0.001, tf.scale);
	const double w = srcSize.width() * s;
	const double h = srcSize.height() * s;
	const double cx = tf.posX * canvas.width();
	const double cy = tf.posY * canvas.height();
	return QRectF(cx - w / 2.0, cy - h / 2.0, w, h);
}

bool TimelineCompositor::isWholeCanvas(const TlTransform &tf)
{
	// Deliberately exact-ish rather than "close enough": at scale 1.001 the area
	// really is slightly larger than the canvas and clipping to it is a no-op, so
	// treating it as whole costs nothing -- but a clip at 0.999 is being
	// deliberately inset, and rounding that up to "everything" would silently
	// ignore what the user did.
	return tf.scale >= 1.0 && std::abs(tf.posX - 0.5) < 1e-9 && std::abs(tf.posY - 0.5) < 1e-9 &&
	       std::abs(tf.rotation) < 1e-9 && tf.opacity >= 1.0;
}

QPainterPath TimelineCompositor::effectAreaPath(const TlTransform &tf, QSize canvas)
{
	QPainterPath path;
	if (canvas.width() <= 0 || canvas.height() <= 0)
		return path;
	const double s = std::max(0.001, tf.scale);
	const double w = canvas.width() * s;
	const double h = canvas.height() * s;
	const double cx = tf.posX * canvas.width();
	const double cy = tf.posY * canvas.height();

	QTransform t;
	t.translate(cx, cy);
	if (std::abs(tf.rotation) > 0.001)
		t.rotate(tf.rotation);
	path.addRect(QRectF(-w / 2.0, -h / 2.0, w, h));
	return t.map(path);
}

void TimelineCompositor::drawTextClip(QPainter &p, const TlClip &c, const TlTransform &tf, QSize canvas)
{
	const TlText &t = c.text;
	if (t.text.isEmpty())
		return;
	const QFont f = buildFont(t, canvas);
	CachedText &ct = cachedText(t, f, canvas);
	const QPainterPath &path = ct.path;
	const QRectF &block = ct.block;

	p.save();
	p.setOpacity(std::clamp(tf.opacity, 0.0, 1.0));
	// Position, spin and zoom: the text transforms about its own centre, like a
	// video clip (the path below is already built centred on the origin).
	p.translate(tf.posX * canvas.width(), tf.posY * canvas.height());
	if (std::abs(tf.rotation) > 0.001)
		p.rotate(tf.rotation);
	p.scale(std::max(0.001, tf.scale), std::max(0.001, tf.scale));
	p.setRenderHint(QPainter::Antialiasing, true);

	if (t.boxEnabled) {
		// `block` is the union box of every line, so a multi-line caption gets one
		// continuous sticker rather than a bar per line. Padding and radius are
		// scaled by the same factor as the font, so the shape keeps its
		// proportions at any canvas size.
		const double k = textScale(canvas);
		const QRectF box =
			block.adjusted(-t.boxPadX * k, -t.boxPadY * k, t.boxPadX * k, t.boxPadY * k);
		// Past half the shorter side a rounded rect stops making sense; clamping
		// there turns a big radius into a clean pill instead of an artefact.
		const double r = std::min(t.boxRadius * k,
					  std::min(box.width(), box.height()) / 2.0);
		QColor fill = t.boxColor;
		// Opacity is its own property: the colour picker sets the hue, this sets
		// how much of the picture shows through, and neither clobbers the other.
		fill.setAlphaF(float(std::clamp(t.boxOpacity, 0.0, 1.0)));
		p.setPen(Qt::NoPen);
		p.setBrush(fill);
		p.drawRoundedRect(box, r, r);
	}
	if (t.outlineWidth > 0.01) {
		if (ct.strokeWidth != t.outlineWidth) {
			QPainterPathStroker stroker;
			stroker.setWidth(t.outlineWidth * 2.0); // straddles the glyph edge
			stroker.setJoinStyle(Qt::RoundJoin);
			stroker.setCapStyle(Qt::RoundCap);
			ct.stroke = stroker.createStroke(path);
			ct.strokeWidth = t.outlineWidth;
		}
		p.fillPath(ct.stroke, t.outlineColor);
	}
	p.fillPath(path, t.color);
	p.restore();
}

void TimelineCompositor::drawClip(QPainter &p, const TlClip &c, const TlTransform &tf, QSize canvas,
				  const QImage &sourceFrame)
{
	if (c.type == TlClip::Type::Text) {
		drawTextClip(p, c, tf, canvas);
		return;
	}
	if (sourceFrame.isNull())
		return;
	// Per-clip crop first, then fit+zoom+position the remaining region.
	//
	// The crop is a source RECTANGLE handed to drawImage, not a cropped copy of
	// the frame: copying allocated and blitted the whole cropped region on every
	// frame of every cropped clip, and QPainter can read the sub-rectangle
	// directly for nothing.
	QRect srcRect(QPoint(0, 0), sourceFrame.size());
	if (!c.crop.isNull() && c.crop.width() > 1 && c.crop.height() > 1) {
		const QRect r = c.crop.intersected(srcRect);
		if (r.width() > 1 && r.height() > 1)
			srcRect = r;
	}
	const QRectF dst = clipRectOnCanvas(tf, canvas, srcRect.size());
	if (dst.isEmpty())
		return;
	p.save();
	p.setOpacity(std::clamp(tf.opacity, 0.0, 1.0));
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);
	if (std::abs(tf.rotation) > 0.001) { // spin about the clip's own centre
		const QPointF c = dst.center();
		p.translate(c);
		p.rotate(tf.rotation);
		p.translate(-c);
	}
	p.drawImage(dst, sourceFrame, srcRect);
	p.restore();
}

QImage TimelineCompositor::compose(const TimelineModel &m, qint64 outMs, QSize canvas, FrameProvider &fp,
				   TransformEvaluator *eval, double fps, QSize logicalCanvas)
{
	if (canvas.width() <= 0 || canvas.height() <= 0)
		return QImage();
	if (!logicalCanvas.isValid() || logicalCanvas.isEmpty())
		logicalCanvas = canvas;
	QImage out(canvas, QImage::Format_RGBA8888);
	out.fill(Qt::black); // gaps and letterbox areas read as black

	QPainter p(&out);
	p.setRenderHint(QPainter::Antialiasing, true);
	// Index order is display order (0 = top lane) and the HIGHER lane renders in
	// FRONT, so walk the tracks back-to-front: the last index is drawn first and
	// index 0 lands on top. `hidden` skips a video track without touching its
	// audio; `muted` only affects the mix.
	for (int ti = m.tracks.size() - 1; ti >= 0; --ti) {
		const TlTrack &t = m.tracks[ti];

		// An effect track grades everything composited SO FAR -- which, walking
		// back-to-front, is exactly the tracks below it. Tracks above are drawn
		// after this and are untouched, and two overlapping effect tracks stack
		// in track order (top applied last). That order comes from the track
		// list, so repeated exports are identical by construction.
		if (t.kind == TlTrack::Kind::Effect) {
			if (t.hidden)
				continue;
			const int ei = t.clipAt(outMs);
			if (ei < 0)
				continue;
			const TlClip &ec = t.clips[ei];
			if (ec.type != TlClip::Type::Effect)
				continue;
			const bool hasComponents = !ec.components.isEmpty();
			if (!hasComponents && !ec.fx.enabled)
				continue;
			// An effect clip has a transform like any other clip, and it
			// bounds WHERE the grade lands: move it, shrink it, spin it, and
			// only that part of the picture is affected. At the default pose
			// (centred, scale 1, no rotation) the area is the whole canvas, so
			// an effect clip nobody has moved behaves exactly as it did.
			const TlTransform exf = ec.transformAt(outMs);
			const QPainterPath area = effectAreaPath(exf, out.size());
			const bool whole = isWholeCanvas(exf);

			// The painter has to be closed before the pixels are touched
			// directly, and reopened for whatever is drawn on top.
			p.end();

			// Grading a cut-out is only safe when nothing in the stack reads
			// its NEIGHBOURS: a blur handed a sub-rect samples the cut edge
			// instead of the pixels really there and leaves a seam at the
			// border. So a point-op stack -- a curve, a matrix, a hue rotation
			// -- grades just the area's bounding box, and everything else
			// grades the whole frame and is painted back through the clip path,
			// exactly as before.
			//
			// Worth the branch: at 1080p a quarter-area effect costs 0.48 ms
			// against 2.24 ms, and the copy 0.20 ms against 0.75 ms.
			// An effect clip's Pixel components run over the composite so far,
			// which IS everything below this track -- the same reach
			// Effects::apply had. On an ordinary clip the identical components
			// see only that clip's own frame; what they grade is decided by
			// where the clip sits, not by the component.
			std::unique_ptr<ComponentStack> stack;
			EvalContext ectx;
			if (hasComponents) {
				stack.reset(new ComponentStack(ec.components,
							       ComponentRegistry::instance()));
				ectx.tMs = outMs - ec.outStartMs;
				ectx.outMs = outMs;
				ectx.durMs = std::max<qint64>(1, ec.outDurationMs());
				ectx.fps = fps;
				ectx.canvas = logicalCanvas;
			}

			QRect sub;
			if (!whole && stack && stack->pixelStageIsPointOp())
				sub = area.boundingRect().toAlignedRect().intersected(out.rect());

			QImage graded;   // the whole frame, when a sub-rect will not do
			QImage patch;    // just the area's box, when it will
			if (!sub.isEmpty()) {
				patch = out.copy(sub);
				stack->evaluatePixels(ectx, patch);
			} else {
				if (!whole)
					graded = out.copy();
				QImage &target = whole ? out : graded;
				if (stack)
					stack->evaluatePixels(ectx, target);
				else // not yet migrated: a project still carrying the old fx
					Effects::apply(target, ec.fx, outMs - ec.outStartMs);
			}
			p.begin(&out);
			p.setRenderHint(QPainter::Antialiasing, true);
			// One paint-back for both routes: the sub-rect goes at its own
			// offset, the whole-frame copy at the origin. `whole` graded in
			// place and has nothing to paint.
			if (!whole) {
				p.save();
				p.setClipPath(area);
				// Opacity fades the grade in and out without touching what is
				// under it -- the natural reading of an effect clip's own
				// opacity, and keyframeable like every other pose channel.
				p.setOpacity(std::clamp(exf.opacity, 0.0, 1.0));
				if (!sub.isEmpty())
					p.drawImage(sub.topLeft(), patch);
				else
					p.drawImage(0, 0, graded);
				p.restore();
			}
			continue;
		}

		if (t.kind != TlTrack::Kind::Video || t.hidden)
			continue;

		// Draw one of this track's clips onto an arbitrary painter. Pulled out
		// so an overlap can render its two clips into their own layers and blend
		// them, using exactly the same decode/script/transform path a lone clip
		// takes -- a transition must not be a second, subtly different renderer.
		auto renderClip = [&](QPainter &into, int ci) {
			const TlClip &c = t.clips[ci];

			// The clip's components, in resolved order. Built once per clip per
			// frame; an empty list costs one branch, which is what almost every
			// clip will be until the port finishes.
			const bool hasComponents = !c.components.isEmpty();
			std::unique_ptr<ComponentStack> stack;
			EvalContext ectx;
			if (hasComponents) {
				stack.reset(new ComponentStack(c.components,
							       ComponentRegistry::instance()));
				ectx.tMs = outMs - c.outStartMs;
				ectx.outMs = outMs;
				ectx.durMs = std::max<qint64>(1, c.outDurationMs());
				ectx.fps = fps;
				// The project's size, not the render size: a half-resolution
				// preview must not change what a component computes.
				ectx.canvas = logicalCanvas;
			}

			// Time stage FIRST, because which source instant to decode is its
			// answer to give. Everything after this point is working on the
			// frame the Time components chose.
			qint64 srcMs = c.srcAtOutput(outMs);
			ClipState cstate;
			if (hasComponents) {
				cstate = stack->evaluatePose(ectx, c.transformAt(outMs));
				if (std::abs(cstate.timeScale - 1.0) > 1e-9) {
					const qint64 off = std::clamp<qint64>(
						outMs - c.outStartMs, 0, c.outDurationMs());
					srcMs = c.srcStartMs +
						qint64(std::llround(double(off) *
								    (c.speed > 0.01 ? c.speed : 1.0) *
								    cstate.timeScale));
				}
			}

			// Decode before scripting, not after: a script needs the clip's pixel
			// size to work out where a point in the picture lands on the canvas,
			// and that is only known once the frame (or the caption) exists.
			QImage frame;
			if (c.type == TlClip::Type::Video || c.type == TlClip::Type::Image) {
				// Which track is asking, so a sequential provider can keep
				// one decoder per stack rather than one per source.
				fp.setTrack(ti);
				frame = fp.frameFor(c.sourceId, srcMs);
			}

			// Base pose / keyframes, then the Transform components, then the
			// clip's scripts on top of both. Scripts stay last of the three
			// because that is where they already were, and moving them would
			// change what existing projects render.
			TlTransform tf = hasComponents ? cstate.xf : c.transformAt(outMs);
			if (eval && !c.scripts.isEmpty()) {
				ScriptContext sctx;
				// The project's size, not the render size: a preview at half
				// resolution must not change what a script computes.
				sctx.canvasW = logicalCanvas.width();
				sctx.canvasH = logicalCanvas.height();
				QSize natural = frame.size();
				if (c.type == TlClip::Type::Text)
					natural = textNaturalSize(c.text, canvas);
				if (!c.crop.isNull() && c.crop.width() > 1 && c.crop.height() > 1 &&
				    !frame.isNull())
					natural = c.crop.intersected(QRect(QPoint(0, 0), frame.size()))
							  .size();
				const double k =
					double(logicalCanvas.width()) / std::max(1, canvas.width());
				sctx.clipW = int(std::lround(natural.width() * k));
				sctx.clipH = int(std::lround(natural.height() * k));
				sctx.fps = fps;
				sctx.index = ci;
				sctx.globalTime = double(outMs) / 1000.0;
				// Stacked: each script starts from what the previous one
				// produced, so scripts driving different channels compose and a
				// later one wins on a channel they share.
				for (const TlScript &s : c.scripts) {
					if (s.name.isEmpty())
						continue;
					tf = eval->apply(ClipScript{s.name, s.params}, tf, c, outMs, sctx);
				}
			}
			// Pixel stage, on the clip's own frame before it is placed on the
			// canvas — so a component blurs the clip, not everything under it.
			// Text clips have no frame to hand over; they are drawn straight to
			// the canvas, and are the next thing the Source stage will fix.
			if (hasComponents && !frame.isNull())
				stack->evaluatePixels(ectx, frame);

			drawClip(into, c, tf, canvas, frame);
		};

		// Two clips covering the same instant on one track IS a transition --
		// there is no separate object to create or delete. Pull them apart and
		// it stops happening on its own.
		int oi = -1, ii = -1;
		if (t.overlapAt(outMs, &oi, &ii) && t.clips[ii].transition.enabled) {
			const TlClip &inc = t.clips[ii];
			const qint64 span = t.overlapBefore(ii);
			if (span > 0) {
				QImage layerA(canvas, QImage::Format_RGBA8888);
				QImage layerB(canvas, QImage::Format_RGBA8888);
				layerA.fill(Qt::transparent);
				layerB.fill(Qt::transparent);
				{
					QPainter pa(&layerA);
					pa.setRenderHint(QPainter::Antialiasing, true);
					renderClip(pa, oi);
				}
				{
					QPainter pb(&layerB);
					pb.setRenderHint(QPainter::Antialiasing, true);
					renderClip(pb, ii);
				}
				const double u = std::clamp(
					double(outMs - inc.outStartMs) / double(span), 0.0, 1.0);
				p.drawImage(0, 0, Transitions::blend(layerA, layerB, u, inc.transition));
				continue;
			}
		}

		const int ci = t.clipAt(outMs);
		if (ci < 0)
			continue;
		renderClip(p, ci);
	}
	p.end();

	// An Inverse Selection is an effect clip, applied by the effect-track branch
	// above at the point in the stack where its track sits -- so it dims exactly
	// the tracks below it, and what is above stays clear. There is no
	// project-wide pass here any more.
	return out;
}

} // namespace harpia
