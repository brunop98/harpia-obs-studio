#include "Transitions.hpp"

#include "Spotlight.hpp" // blurInPlace

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cstring>
#include <cmath>

namespace harpia {

namespace {

const char *kNames[kTransitionTypeCount] = {
	"Crossfade", "Dissolve",  "Fade through black", "Fade through white",
	"Wipe left", "Wipe right", "Wipe up",           "Wipe down",
	"Push",      "Slide",      "Zoom",              "Blur",
	"Iris",      "Circle",     "Pixelate",
};

// Deterministic per-pixel threshold for the noise dissolve, so two exports of
// the same project match.
inline double hash01(int x, int y)
{
	unsigned int n = (unsigned int)(x * 374761393 + y * 668265263);
	n = (n ^ (n >> 13)) * 1274126177u;
	return double((n ^ (n >> 16)) & 0xffff) / 65535.0;
}

QImage toRGBA(const QImage &i)
{
	return i.format() == QImage::Format_RGBA8888 ? i : i.convertToFormat(QImage::Format_RGBA8888);
}

// Straight per-pixel lerp, alpha included, so a transition between two clips
// that do not fill the frame stays correct at the edges.
QImage lerpImages(const QImage &a, const QImage &b, double u)
{
	QImage out = toRGBA(a);
	const QImage bb = toRGBA(b);
	const int w = std::min(out.width(), bb.width()), h = std::min(out.height(), bb.height());
	const double k = std::clamp(u, 0.0, 1.0);
	for (int y = 0; y < h; ++y) {
		unsigned char *o = out.scanLine(y);
		const unsigned char *s = bb.scanLine(y);
		for (int x = 0; x < w * 4; ++x)
			o[x] = (unsigned char)std::lround(o[x] + (s[x] - o[x]) * k);
	}
	return out;
}

// Fade both to a flat colour and back out of it — the "dip to black" cut.
QImage fadeThrough(const QImage &a, const QImage &b, double u, const QColor &via)
{
	QImage out(a.size(), QImage::Format_RGBA8888);
	out.fill(Qt::transparent);
	QPainter p(&out);
	if (u < 0.5) {
		p.setOpacity(1.0);
		p.drawImage(0, 0, a);
		p.fillRect(out.rect(), QColor(via.red(), via.green(), via.blue(),
					      int(std::lround(u * 2.0 * 255.0))));
	} else {
		p.setOpacity(1.0);
		p.drawImage(0, 0, b);
		p.fillRect(out.rect(), QColor(via.red(), via.green(), via.blue(),
					      int(std::lround((1.0 - u) * 2.0 * 255.0))));
	}
	return out;
}

// A wipe/iris/circle: the incoming is revealed through a growing shape. The
// soft edge is a gradient band along the moving boundary.
QImage revealThrough(const QImage &a, const QImage &b, const QPainterPath &shape)
{
	QImage out = toRGBA(a);
	QPainter p(&out);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setClipPath(shape);
	p.drawImage(0, 0, b);
	return out;
}

} // namespace

const char *transitionName(TransitionType t)
{
	const int i = std::clamp(int(t), 0, kTransitionTypeCount - 1);
	return kNames[i];
}

TransitionType transitionFromInt(int v)
{
	return (v >= 0 && v < kTransitionTypeCount) ? TransitionType(v) : TransitionType::Crossfade;
}

bool Transitions::hasSoftEdge(TransitionType t)
{
	switch (t) {
	case TransitionType::WipeLeft:
	case TransitionType::WipeRight:
	case TransitionType::WipeUp:
	case TransitionType::WipeDown:
	case TransitionType::Iris:
	case TransitionType::Circle:
		return true;
	default:
		return false;
	}
}

QImage Transitions::blend(const QImage &outgoingIn, const QImage &incomingIn, double u,
			  const TlTransition &tr)
{
	if (outgoingIn.isNull())
		return incomingIn;
	if (incomingIn.isNull())
		return outgoingIn;
	u = std::clamp(u, 0.0, 1.0);

	// Reverse means "play it as though the two clips were the other way round":
	// swap the layers AND run the clock backwards, which lands on the same
	// frames but with the effect's geometry mirrored. A left wipe becomes a
	// right one; a symmetric type like a crossfade is unchanged, correctly.
	//
	// Flipping only the clock does NOT do this -- the boundary still sweeps the
	// same way, it just reaches each point at a different moment.
	const QImage &outgoing = tr.reverse ? incomingIn : outgoingIn;
	const QImage &incoming = tr.reverse ? outgoingIn : incomingIn;
	if (tr.reverse)
		u = 1.0 - u;

	// The two curves shape the two halves independently: `uIn` drives how fast
	// the incoming arrives, `uOut` how fast the outgoing leaves. A single-image
	// effect (a wipe) has only one boundary, so it follows the incoming.
	const double uIn = tlEaseAt(tr.easeIn, u);
	const double uOut = tlEaseAt(tr.easeOut, u);

	const int w = outgoing.width(), h = outgoing.height();
	const double soft = std::clamp(tr.softness, 0.0, 1.0);

	switch (tr.type) {
	case TransitionType::Crossfade:
		// Blend by the mean of the two curves: with both linear this is the
		// plain dissolve, and bending one side alone still favours it.
		return lerpImages(outgoing, incoming, (uIn + uOut) / 2.0);

	case TransitionType::Dissolve: {
		// Pixel-noise: each pixel flips at its own threshold, so the change
		// scatters rather than fading.
		QImage out = toRGBA(outgoing);
		const QImage bb = toRGBA(incoming);
		for (int y = 0; y < h && y < bb.height(); ++y) {
			unsigned char *o = out.scanLine(y);
			const unsigned char *s = bb.scanLine(y);
			for (int x = 0; x < w && x < bb.width(); ++x)
				if (hash01(x, y) < uIn)
					std::memcpy(o + x * 4, s + x * 4, 4);
		}
		return out;
	}

	case TransitionType::FadeThroughBlack:
		return fadeThrough(outgoing, incoming, (uIn + uOut) / 2.0, QColor(0, 0, 0));
	case TransitionType::FadeThroughWhite:
		return fadeThrough(outgoing, incoming, (uIn + uOut) / 2.0, QColor(255, 255, 255));

	case TransitionType::WipeLeft:
	case TransitionType::WipeRight:
	case TransitionType::WipeUp:
	case TransitionType::WipeDown: {
		// A soft edge is a band of intermediate blend along the boundary, so
		// the whole frame is a lerp whose weight varies with position.
		QImage out = toRGBA(outgoing);
		const QImage bb = toRGBA(incoming);
		const double band = std::max(1e-4, soft * 0.35);
		for (int y = 0; y < h && y < bb.height(); ++y) {
			unsigned char *o = out.scanLine(y);
			const unsigned char *s = bb.scanLine(y);
			// How far along the wipe's axis this pixel sits, 0..1.
			const double fy = h > 1 ? double(y) / (h - 1) : 0.0;
			for (int x = 0; x < w && x < bb.width(); ++x) {
				const double fx = w > 1 ? double(x) / (w - 1) : 0.0;
				double pos = fx;
				if (tr.type == TransitionType::WipeRight)
					pos = 1.0 - fx;
				else if (tr.type == TransitionType::WipeUp)
					pos = 1.0 - fy;
				else if (tr.type == TransitionType::WipeDown)
					pos = fy;
				// The edge sweeps from -band to 1; a pixel is fully
				// incoming once the edge has passed it by `band`.
				const double k = std::clamp(
					(uIn * (1.0 + band) - pos) / band, 0.0, 1.0);
				if (k <= 0.0)
					continue;
				for (int c = 0; c < 4; ++c)
					o[x * 4 + c] = (unsigned char)std::lround(
						o[x * 4 + c] +
						(s[x * 4 + c] - o[x * 4 + c]) * k);
			}
		}
		return out;
	}

	case TransitionType::Push: {
		// Both move: the incoming shoves the outgoing out of frame.
		QImage out(outgoing.size(), QImage::Format_RGBA8888);
		out.fill(Qt::transparent);
		QPainter p(&out);
		const int dx = int(std::lround(uIn * w));
		p.drawImage(-dx, 0, outgoing);
		p.drawImage(w - dx, 0, incoming);
		return out;
	}

	case TransitionType::Slide: {
		// Only the incoming moves, over a stationary outgoing.
		QImage out = toRGBA(outgoing);
		QPainter p(&out);
		p.drawImage(int(std::lround((1.0 - uIn) * w)), 0, incoming);
		return out;
	}

	case TransitionType::Zoom: {
		// The incoming grows into place while the outgoing fades under it.
		QImage out = lerpImages(outgoing, incoming, uOut * 0.35); // a hint of the mix
		QPainter p(&out);
		p.setRenderHint(QPainter::SmoothPixmapTransform, true);
		const double s = 0.2 + 0.8 * uIn;
		const QRectF dst(w * (1.0 - s) / 2.0, h * (1.0 - s) / 2.0, w * s, h * s);
		p.setOpacity(uIn);
		p.drawImage(dst, incoming);
		return out;
	}

	case TransitionType::Blur: {
		// Both go soft in the middle of the move and sharpen again at the ends.
		QImage a = toRGBA(outgoing), b = toRGBA(incoming);
		const double bell = 1.0 - std::abs(u * 2.0 - 1.0); // 0 at the ends, 1 mid
		const int r = int(std::lround(bell * 0.05 * std::min(w, h)));
		if (r > 0) {
			Spotlight::blurInPlace(a, r);
			Spotlight::blurInPlace(b, r);
		}
		return lerpImages(a, b, (uIn + uOut) / 2.0);
	}

	case TransitionType::Iris: {
		QPainterPath shape;
		const double s = uIn;
		shape.addRect(QRectF(w * (1.0 - s) / 2.0, h * (1.0 - s) / 2.0, w * s, h * s));
		return revealThrough(outgoing, incoming, shape);
	}

	case TransitionType::Circle: {
		QPainterPath shape;
		// Radius reaches the corners at u = 1, so the reveal completes exactly.
		const double maxR = std::sqrt(double(w) * w + double(h) * h) / 2.0;
		const double r = uIn * maxR;
		shape.addEllipse(QPointF(w / 2.0, h / 2.0), r, r);
		return revealThrough(outgoing, incoming, shape);
	}

	case TransitionType::Pixelate: {
		// Blocky in the middle, sharp at both ends.
		QImage mix = lerpImages(outgoing, incoming, (uIn + uOut) / 2.0);
		const double bell = 1.0 - std::abs(u * 2.0 - 1.0);
		const int block = int(std::lround(bell * 0.06 * std::min(w, h)));
		if (block >= 2) {
			for (int by = 0; by < h; by += block)
				for (int bx = 0; bx < w; bx += block) {
					const int x1 = std::min(bx + block, w);
					const int y1 = std::min(by + block, h);
					long long s[4] = {0, 0, 0, 0};
					long long n = 0;
					for (int y = by; y < y1; ++y) {
						const unsigned char *row = mix.scanLine(y);
						for (int x = bx; x < x1; ++x, ++n)
							for (int c = 0; c < 4; ++c)
								s[c] += row[x * 4 + c];
					}
					if (n <= 0)
						continue;
					for (int y = by; y < y1; ++y) {
						unsigned char *row = mix.scanLine(y);
						for (int x = bx; x < x1; ++x)
							for (int c = 0; c < 4; ++c)
								row[x * 4 + c] =
									(unsigned char)(s[c] / n);
					}
				}
		}
		return mix;
	}

	case TransitionType::Count:
		break;
	}
	return lerpImages(outgoing, incoming, u);
}

} // namespace harpia
