#include "TimelineCompositor.hpp"

#include "../script/TransformScript.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
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

QFont buildFont(const TlText &t, QSize canvas)
{
	QFont f;
	if (!t.fontFamily.isEmpty())
		f.setFamily(t.fontFamily);
	const double k = canvas.height() > 0 ? double(canvas.height()) / kTextRefHeight : 1.0;
	f.setPixelSize(std::max(4, int(std::llround(t.fontPx * k))));
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
	const double pad = t.boxEnabled ? t.boxPadding * 2.0 : 0.0;
	return QSize(std::max(1, int(std::ceil(block.width() + pad))),
		     std::max(1, int(std::ceil(block.height() + pad))));
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
		const QRectF box = block.adjusted(-t.boxPadding, -t.boxPadding, t.boxPadding, t.boxPadding);
		p.setPen(Qt::NoPen);
		p.setBrush(t.boxColor);
		p.drawRoundedRect(box, t.boxRadius, t.boxRadius);
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
	QImage img = sourceFrame;
	if (!c.crop.isNull() && c.crop.width() > 1 && c.crop.height() > 1) {
		const QRect r = c.crop.intersected(QRect(QPoint(0, 0), img.size()));
		if (r.width() > 1 && r.height() > 1)
			img = img.copy(r);
	}
	const QRectF dst = clipRectOnCanvas(tf, canvas, img.size());
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
	p.drawImage(dst, img);
	p.restore();
}

QImage TimelineCompositor::compose(const TimelineModel &m, qint64 outMs, QSize canvas, FrameProvider &fp,
				   TransformEvaluator *eval, double fps)
{
	if (canvas.width() <= 0 || canvas.height() <= 0)
		return QImage();
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
		if (t.kind != TlTrack::Kind::Video || t.hidden)
			continue;
		const int ci = t.clipAt(outMs);
		if (ci < 0)
			continue;
		const TlClip &c = t.clips[ci];

		// Decode before scripting, not after: a script needs the clip's pixel size
		// to work out where a point in the picture lands on the canvas, and that
		// is only known once the frame (or the measured caption) exists.
		QImage frame;
		if (c.type == TlClip::Type::Video || c.type == TlClip::Type::Image)
			frame = fp.frameFor(c.sourceId, c.srcAtOutput(outMs));

		// Base pose / keyframes first, then the clip's script overrides whichever
		// channels it defines.
		TlTransform tf = c.transformAt(outMs);
		if (eval && !c.scripts.isEmpty()) {
			ScriptContext sctx;
			sctx.canvasW = canvas.width();
			sctx.canvasH = canvas.height();
			QSize natural = frame.size();
			if (c.type == TlClip::Type::Text)
				natural = textNaturalSize(c.text, canvas);
			if (!c.crop.isNull() && c.crop.width() > 1 && c.crop.height() > 1 &&
			    !frame.isNull())
				natural = c.crop.intersected(QRect(QPoint(0, 0), frame.size())).size();
			sctx.clipW = natural.width();
			sctx.clipH = natural.height();
			sctx.fps = fps;
			sctx.index = ci;
			sctx.globalTime = double(outMs) / 1000.0;
			// Stacked: each script starts from what the previous one produced, so
			// scripts driving different channels compose and a later one wins on a
			// channel they share.
			for (const TlScript &s : c.scripts) {
				if (s.name.isEmpty())
					continue;
				tf = eval->apply(ClipScript{s.name, s.params}, tf, c, outMs, sctx);
			}
		}
		drawClip(p, c, tf, canvas, frame);
	}
	p.end();
	return out;
}

} // namespace harpia
