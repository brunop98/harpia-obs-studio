#include "Spotlight.hpp"

#include <QPainter>
#include <QStringList>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <vector>

namespace harpia {

QPainterPath Spotlight::maskPath(const SpotMask &m, const SpotPose &pose, QSize canvas)
{
	const double cw = std::max(1, canvas.width());
	const double ch = std::max(1, canvas.height());
	// A circle is round on SCREEN, so its height comes from the width in pixels
	// rather than from the normalised height (which would make it an ellipse on
	// any non-square canvas).
	double wpx = std::max(1.0, pose.w * cw);
	double hpx = (m.shape == SpotShape::Circle) ? wpx : std::max(1.0, pose.h * ch);
	const QRectF r(-wpx / 2.0, -hpx / 2.0, wpx, hpx);

	QPainterPath local;
	switch (m.shape) {
	case SpotShape::Rect:
		local.addRect(r);
		break;
	case SpotShape::RoundRect: {
		const double rad = std::clamp(pose.radius, 0.0, 0.5) * std::min(wpx, hpx);
		local.addRoundedRect(r, rad, rad);
		break;
	}
	case SpotShape::Circle:
	case SpotShape::Ellipse:
		local.addEllipse(r);
		break;
	}

	QTransform tf;
	tf.translate(pose.cx * cw, pose.cy * ch);
	tf.rotate(pose.rotation);
	return tf.map(local);
}

void Spotlight::blurInPlace(QImage &img, int radius)
{
	if (radius < 1 || img.isNull())
		return;
	if (img.format() != QImage::Format_RGBA8888 && img.format() != QImage::Format_ARGB32 &&
	    img.format() != QImage::Format_RGB32)
		img = img.convertToFormat(QImage::Format_RGBA8888);

	const int w = img.width(), h = img.height();
	const int ch = 4;
	const int stride = img.bytesPerLine();
	std::vector<unsigned char> tmp(size_t(w) * size_t(h) * ch);

	// Three box passes ≈ a Gaussian, and each pass is a running sum: cost is
	// O(pixels) regardless of the radius, which matters because the radius here
	// scales with the frame.
	auto boxH = [&](unsigned char *src, int srcStride, unsigned char *dst, int dstStride) {
		const double inv = 1.0 / (2.0 * radius + 1.0);
		for (int y = 0; y < h; ++y) {
			unsigned char *sr = src + size_t(y) * srcStride;
			unsigned char *dr = dst + size_t(y) * dstStride;
			int acc[4] = {0, 0, 0, 0};
			for (int c = 0; c < ch; ++c)
				acc[c] = sr[c] * (radius + 1);
			for (int x = 1; x <= radius; ++x) {
				const int xi = std::min(x, w - 1);
				for (int c = 0; c < ch; ++c)
					acc[c] += sr[xi * ch + c];
			}
			for (int x = 0; x < w; ++x) {
				const int xa = std::clamp(x + radius + 1, 0, w - 1);
				const int xb = std::clamp(x - radius, 0, w - 1);
				for (int c = 0; c < ch; ++c) {
					dr[x * ch + c] = (unsigned char)(acc[c] * inv);
					acc[c] += sr[xa * ch + c] - sr[xb * ch + c];
				}
			}
		}
	};
	auto boxV = [&](unsigned char *src, int srcStride, unsigned char *dst, int dstStride) {
		const double inv = 1.0 / (2.0 * radius + 1.0);
		for (int x = 0; x < w; ++x) {
			int acc[4] = {0, 0, 0, 0};
			for (int c = 0; c < ch; ++c)
				acc[c] = src[x * ch + c] * (radius + 1);
			for (int y = 1; y <= radius; ++y) {
				const int yi = std::min(y, h - 1);
				for (int c = 0; c < ch; ++c)
					acc[c] += src[size_t(yi) * srcStride + x * ch + c];
			}
			for (int y = 0; y < h; ++y) {
				const int ya = std::clamp(y + radius + 1, 0, h - 1);
				const int yb = std::clamp(y - radius, 0, h - 1);
				for (int c = 0; c < ch; ++c) {
					dst[size_t(y) * dstStride + x * ch + c] =
						(unsigned char)(acc[c] * inv);
					acc[c] += src[size_t(ya) * srcStride + x * ch + c] -
						  src[size_t(yb) * srcStride + x * ch + c];
				}
			}
		}
	};

	unsigned char *bits = img.bits();
	for (int pass = 0; pass < 3; ++pass) {
		boxH(bits, stride, tmp.data(), w * ch);
		boxV(tmp.data(), w * ch, bits, stride);
	}
}

void Spotlight::apply(QImage &frame, const SpotlightSpec &spec, qint64 outMs)
{
	if (!spec.active() || frame.isNull())
		return;

	// The union of every enabled, visible area at this instant. `visible` is a
	// keyframable 0..1 so a mask can be faded out of existence; below the halfway
	// point it simply isn't cut.
	QPainterPath cut;
	for (const SpotMask &m : spec.masks) {
		if (!m.enabled)
			continue;
		const SpotPose pose = m.poseAt(outMs);
		if (pose.visible < 0.5)
			continue;
		cut = cut.united(maskPath(m, pose, frame.size()));
	}
	if (cut.isEmpty())
		return;

	// The region that gets dimmed: everything outside the areas, or (inverted)
	// the areas themselves.
	QPainterPath dimmed;
	if (spec.invert) {
		dimmed = cut;
	} else {
		dimmed.addRect(QRectF(0, 0, frame.width(), frame.height()));
		dimmed = dimmed.subtracted(cut);
	}

	const double amount = std::clamp(spec.dimOpacity, 0.0, 1.0);
	// Radius scales with the frame, so a project blurs the same at any render
	// size — including the reduced-size preview.
	const int radius = int(std::lround(std::clamp(spec.blur, 0.0, 1.0) * 0.06 *
					   std::min(frame.width(), frame.height())));

	QPainter p(&frame);
	p.setRenderHint(QPainter::Antialiasing, false); // hard edges, by design

	if (radius > 0) {
		// Blur a copy of the whole frame once, then paint only the dimmed part
		// of it back. Blurring the clipped region instead would pull in the
		// black outside it and leave a dark halo along the mask's edge.
		QImage blurred = frame.copy();
		blurInPlace(blurred, radius);
		p.save();
		p.setClipPath(dimmed);
		p.drawImage(0, 0, blurred);
		p.restore();
	}

	if (amount > 0.0) {
		QColor c = spec.dimColor;
		c.setAlphaF(float(amount));
		p.setClipPath(dimmed);
		p.fillRect(frame.rect(), c);
	}
}

QStringList Spotlight::presetNames()
{
	return {QStringLiteral("Spotlight"), QStringLiteral("Rectangle"),
		QStringLiteral("Rounded rectangle"), QStringLiteral("Circle"),
		QStringLiteral("Ellipse")};
}

SpotMask Spotlight::preset(const QString &name)
{
	SpotMask m;
	m.name = name;
	if (name == QStringLiteral("Rectangle")) {
		m.shape = SpotShape::Rect;
		m.pose.w = 0.5;
		m.pose.h = 0.32;
	} else if (name == QStringLiteral("Rounded rectangle")) {
		m.shape = SpotShape::RoundRect;
		m.pose.w = 0.5;
		m.pose.h = 0.32;
		m.pose.radius = 0.2;
	} else if (name == QStringLiteral("Circle")) {
		m.shape = SpotShape::Circle;
		m.pose.w = 0.3;
		m.pose.h = 0.3;
	} else if (name == QStringLiteral("Ellipse")) {
		m.shape = SpotShape::Ellipse;
		m.pose.w = 0.45;
		m.pose.h = 0.28;
	} else { // "Spotlight" — the tutorial default: a soft-cornered highlight
		m.shape = SpotShape::RoundRect;
		m.pose.w = 0.4;
		m.pose.h = 0.4;
		m.pose.radius = 0.5; // fully rounded = a stadium/circle
	}
	return m;
}

QVector<SpotMask> Spotlight::presets()
{
	QVector<SpotMask> out;
	for (const QString &n : presetNames())
		out.append(preset(n));
	return out;
}

} // namespace harpia
