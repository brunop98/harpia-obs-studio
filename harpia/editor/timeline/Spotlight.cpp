#include "Spotlight.hpp"

#include "../shader/SpotlightGl.hpp"

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
	const int stride = img.bytesPerLine();
	const size_t plane = size_t(w) * size_t(h);

	// This is the single hottest thing in the renderer — it backs Blur, Gaussian
	// blur, Sharpen, Glow and the Blur transition — so it is worth doing
	// properly.
	//
	// The frame arrives interleaved RGBA, and a stride-4 inner loop cannot be
	// vectorised: the compiler has to touch one byte in every four. Splitting
	// the three colour channels into their own contiguous planes costs two extra
	// linear passes and buys six vectorisable ones. Alpha never leaves the image
	// at all, so it is neither read nor written.
	//
	// The scratch is kept between calls rather than allocated per frame, and is
	// thread-local because the preview blurs on the GUI thread while the
	// exporter blurs on its worker.
	static thread_local std::vector<unsigned char> scratch;
	static thread_local std::vector<int> accBuf;
	if (scratch.size() < plane * 6)
		scratch.resize(plane * 6);
	if (accBuf.size() < size_t(w))
		accBuf.resize(size_t(w));
	unsigned char *src[3] = {scratch.data(), scratch.data() + plane, scratch.data() + plane * 2};
	unsigned char *dst[3] = {scratch.data() + plane * 3, scratch.data() + plane * 4,
				 scratch.data() + plane * 5};
	int *acc = accBuf.data();

	unsigned char *bits = img.bits();
	for (int y = 0; y < h; ++y) {
		const unsigned char *row = bits + size_t(y) * stride;
		const size_t o = size_t(y) * size_t(w);
		for (int x = 0; x < w; ++x) {
			src[0][o + x] = row[x * 4 + 0];
			src[1][o + x] = row[x * 4 + 1];
			src[2][o + x] = row[x * 4 + 2];
		}
	}

	// Divide by the window width as an integer reciprocal multiply.
	//
	// Not a float multiply: the box sum is an int, and converting it to float
	// and back for every element put a cvtsi2ss/cvttss2si pair on the inner
	// loop's critical path. `mul` is ceil(2^32 / win), so the 64-bit product's
	// top half is exactly floor(sum / win) for every sum a box of bytes can
	// produce; adding half a window first makes it round to nearest.
	const unsigned win = unsigned(2 * radius + 1);
	const unsigned long long mul = (0x100000000ull + win - 1) / win;
	const unsigned half = win / 2;
	const auto avg = [mul, half](unsigned a) {
		return (unsigned char)((((unsigned long long)(a + half)) * mul) >> 32);
	};

	// Three box passes ≈ a Gaussian, and each pass is a running sum: cost is
	// O(pixels) regardless of the radius, which matters because the radius here
	// scales with the frame.
	auto boxH = [&](const unsigned char *s, unsigned char *d) {
		for (int y = 0; y < h; ++y) {
			const unsigned char *sr = s + size_t(y) * w;
			unsigned char *dr = d + size_t(y) * w;
			int a = sr[0] * (radius + 1);
			for (int x = 1; x <= radius; ++x)
				a += sr[std::min(x, w - 1)];
			for (int x = 0; x < w; ++x) {
				dr[x] = avg(unsigned(a));
				a += sr[std::min(x + radius + 1, w - 1)] -
				     sr[std::max(x - radius, 0)];
			}
		}
	};
	// One accumulator PER COLUMN, iterated rows-outer: every read and write is
	// sequential, and the inner loop carries no dependency across x, so it
	// vectorises. Walking actual columns (the obvious way to write this) reads
	// one cache line per pixel and was most of the old cost.
	auto boxV = [&](const unsigned char *s, unsigned char *d) {
		for (int x = 0; x < w; ++x)
			acc[x] = s[x] * (radius + 1);
		for (int y = 1; y <= radius; ++y) {
			const unsigned char *sr = s + size_t(std::min(y, h - 1)) * w;
			for (int x = 0; x < w; ++x)
				acc[x] += sr[x];
		}
		for (int y = 0; y < h; ++y) {
			const unsigned char *sa = s + size_t(std::min(y + radius + 1, h - 1)) * w;
			const unsigned char *sb = s + size_t(std::max(y - radius, 0)) * w;
			unsigned char *dr = d + size_t(y) * w;
			for (int x = 0; x < w; ++x) {
				dr[x] = avg(unsigned(acc[x]));
				acc[x] += sa[x] - sb[x];
			}
		}
	};

	for (int c = 0; c < 3; ++c) {
		for (int pass = 0; pass < 3; ++pass) {
			boxH(src[c], dst[c]);
			boxV(dst[c], src[c]);
		}
	}

	for (int y = 0; y < h; ++y) {
		unsigned char *row = bits + size_t(y) * stride;
		const size_t o = size_t(y) * size_t(w);
		for (int x = 0; x < w; ++x) {
			row[x * 4 + 0] = src[0][o + x];
			row[x * 4 + 1] = src[1][o + x];
			row[x * 4 + 2] = src[2][o + x];
		}
	}
}

void Spotlight::apply(QImage &frame, const SpotlightSpec &spec, qint64 outMs)
{
	if (!spec.active() || frame.isNull())
		return;

	// The GPU does the whole thing when it can. Dispatching HERE rather than at
	// the call sites is deliberate: the compositor and the effect-clip path both
	// arrive through this function, so neither can end up on a different path
	// from the other, and preview and export cannot drift apart. A false return
	// means nothing was touched and the CPU code below is the answer.
	if (SpotlightGl::tryApply(frame, spec, outMs))
		return;
	applyCpu(frame, spec, outMs);
}

void Spotlight::applyCpu(QImage &frame, const SpotlightSpec &spec, qint64 outMs)
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
