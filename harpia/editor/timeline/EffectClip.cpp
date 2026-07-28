#include "EffectClip.hpp"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <vector>

namespace harpia {

namespace {

// The parameter table. Everything the Inspector shows, everything "Reset"
// restores and everything persistence writes comes from here, so a new
// parameter is one line and cannot be forgotten in three other places.
struct FxDef {
	const char *name;
	QVector<FxParamDef> params;
};

const FxDef &defOf(FxType t)
{
	static const FxDef table[kFxTypeCount] = {
		{"Brightness", {{"amount", "Amount", -1.0, 1.0, 0.2}}},
		{"Contrast", {{"amount", "Amount", -1.0, 1.0, 0.2}}},
		{"Saturation", {{"amount", "Amount", -1.0, 2.0, 0.3}}},
		{"Exposure", {{"stops", "Stops", -4.0, 4.0, 0.5}}},
		{"Hue shift", {{"degrees", "Degrees", -180.0, 180.0, 30.0}}},
		{"Colour balance",
		 {{"r", "Red", -1.0, 1.0, 0.0}, {"g", "Green", -1.0, 1.0, 0.0},
		  {"b", "Blue", -1.0, 1.0, 0.1}}},
		{"Blur", {{"radius", "Radius", 0.0, 1.0, 0.15}}},
		{"Gaussian blur", {{"radius", "Radius", 0.0, 1.0, 0.15}}},
		{"Sharpen", {{"amount", "Amount", 0.0, 2.0, 0.6}}},
		{"Vignette",
		 {{"amount", "Amount", 0.0, 1.0, 0.5}, {"size", "Size", 0.1, 1.5, 0.75},
		  {"softness", "Softness", 0.01, 1.0, 0.45}}},
		{"Glow",
		 {{"amount", "Amount", 0.0, 1.0, 0.4}, {"radius", "Radius", 0.0, 1.0, 0.25},
		  {"threshold", "Threshold", 0.0, 1.0, 0.6}}},
		{"Pixelate", {{"size", "Block size", 0.002, 0.2, 0.02}}},
		{"Noise", {{"amount", "Amount", 0.0, 1.0, 0.15},
			   {"mono", "Monochrome", 0.0, 1.0, 1.0}}},
		{"Chromatic aberration", {{"amount", "Amount", 0.0, 1.0, 0.3}}},
		{"Inverse selection", {}}, // its own spec, edited in the Spotlight panel
	};
	const int i = std::clamp(int(t), 0, kFxTypeCount - 1);
	return table[i];
}

inline unsigned char clamp8(double v)
{
	return (unsigned char)std::clamp(v, 0.0, 255.0);
}

// Walk every pixel with a per-channel function. Everything colour-ish goes
// through here so the loop, the format check and the alpha handling exist once.
template <typename F> void perPixel(QImage &img, F fn)
{
	if (img.format() != QImage::Format_RGBA8888)
		img = img.convertToFormat(QImage::Format_RGBA8888);
	const int w = img.width(), h = img.height();
	for (int y = 0; y < h; ++y) {
		unsigned char *row = img.scanLine(y);
		for (int x = 0; x < w; ++x) {
			unsigned char *px = row + x * 4;
			double r = px[0], g = px[1], b = px[2];
			fn(r, g, b, x, y);
			px[0] = clamp8(r);
			px[1] = clamp8(g);
			px[2] = clamp8(b);
		}
	}
}

double lumaOf(double r, double g, double b)
{
	return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

// Radius parameters are 0..1 of the frame's shorter side, so an effect looks the
// same at any render size — including a reduced-size preview.
int radiusPx(double norm, const QImage &img)
{
	return int(std::lround(std::clamp(norm, 0.0, 1.0) * 0.08 *
			       std::min(img.width(), img.height())));
}

// Deterministic value noise: the same frame always gets the same grain, so two
// exports of the same project are byte-identical.
inline double hashNoise(int x, int y, int salt)
{
	unsigned int n = (unsigned int)(x * 374761393 + y * 668265263 + salt * 1274126177);
	n = (n ^ (n >> 13)) * 1274126177u;
	return double((n ^ (n >> 16)) & 0xffff) / 65535.0;
}

void applyVignette(QImage &img, double amount, double size, double softness)
{
	const double cx = img.width() / 2.0, cy = img.height() / 2.0;
	const double maxR = std::sqrt(cx * cx + cy * cy);
	const double inner = std::max(0.001, size) * maxR;
	const double outer = inner + std::max(0.01, softness) * maxR;
	perPixel(img, [&](double &r, double &g, double &b, int x, int y) {
		const double dx = x - cx, dy = y - cy;
		const double d = std::sqrt(dx * dx + dy * dy);
		double k = (d - inner) / std::max(1.0, outer - inner);
		k = std::clamp(k, 0.0, 1.0);
		k = k * k * (3.0 - 2.0 * k); // smooth, so the falloff has no visible ring
		const double f = 1.0 - amount * k;
		r *= f;
		g *= f;
		b *= f;
	});
}

void applyPixelate(QImage &img, double sizeNorm)
{
	const int block = std::max(2, int(std::lround(std::clamp(sizeNorm, 0.002, 0.5) *
						      std::min(img.width(), img.height()))));
	if (img.format() != QImage::Format_RGBA8888)
		img = img.convertToFormat(QImage::Format_RGBA8888);
	const int w = img.width(), h = img.height();
	for (int by = 0; by < h; by += block) {
		for (int bx = 0; bx < w; bx += block) {
			const int x1 = std::min(bx + block, w), y1 = std::min(by + block, h);
			long long sr = 0, sg = 0, sb = 0, n = 0;
			for (int y = by; y < y1; ++y) {
				const unsigned char *row = img.scanLine(y);
				for (int x = bx; x < x1; ++x) {
					const unsigned char *px = row + x * 4;
					sr += px[0];
					sg += px[1];
					sb += px[2];
					++n;
				}
			}
			if (n <= 0)
				continue;
			const unsigned char r = (unsigned char)(sr / n), g = (unsigned char)(sg / n),
					    b = (unsigned char)(sb / n);
			for (int y = by; y < y1; ++y) {
				unsigned char *row = img.scanLine(y);
				for (int x = bx; x < x1; ++x) {
					unsigned char *px = row + x * 4;
					px[0] = r;
					px[1] = g;
					px[2] = b;
				}
			}
		}
	}
}

void applySharpen(QImage &img, double amount)
{
	// Unsharp mask: the original minus a blurred copy, added back. Reuses the
	// same blur as everything else rather than a second convolution.
	QImage soft = img.convertToFormat(QImage::Format_RGBA8888);
	Spotlight::blurInPlace(soft, std::max(1, radiusPx(0.06, img)));
	if (img.format() != QImage::Format_RGBA8888)
		img = img.convertToFormat(QImage::Format_RGBA8888);
	const int w = img.width(), h = img.height();
	for (int y = 0; y < h; ++y) {
		unsigned char *a = img.scanLine(y);
		const unsigned char *s = soft.scanLine(y);
		for (int x = 0; x < w * 4; x += 4)
			for (int c = 0; c < 3; ++c)
				a[x + c] = clamp8(a[x + c] + amount * (a[x + c] - s[x + c]));
	}
}

void applyGlow(QImage &img, double amount, double radius, double threshold)
{
	if (img.format() != QImage::Format_RGBA8888)
		img = img.convertToFormat(QImage::Format_RGBA8888);
	// Keep only what is brighter than the threshold, blur that, add it back.
	QImage bright = img.copy();
	const double cut = threshold * 255.0;
	perPixel(bright, [&](double &r, double &g, double &b, int, int) {
		const double l = lumaOf(r, g, b);
		if (l <= cut) {
			r = g = b = 0.0;
		}
	});
	Spotlight::blurInPlace(bright, std::max(1, radiusPx(radius, img)));
	const int w = img.width(), h = img.height();
	for (int y = 0; y < h; ++y) {
		unsigned char *a = img.scanLine(y);
		const unsigned char *s = bright.scanLine(y);
		for (int x = 0; x < w * 4; x += 4)
			for (int c = 0; c < 3; ++c)
				a[x + c] = clamp8(a[x + c] + amount * s[x + c]);
	}
}

void applyChromatic(QImage &img, double amount)
{
	if (img.format() != QImage::Format_RGBA8888)
		img = img.convertToFormat(QImage::Format_RGBA8888);
	const int w = img.width(), h = img.height();
	// Red and blue are pushed apart radially from the centre, which is what a
	// real lens does — a flat sideways offset reads as a mistake.
	const double maxShift = amount * 0.012 * std::min(w, h);
	if (maxShift < 0.5)
		return;
	const QImage src = img.copy();
	const double cx = w / 2.0, cy = h / 2.0;
	for (int y = 0; y < h; ++y) {
		unsigned char *dst = img.scanLine(y);
		for (int x = 0; x < w; ++x) {
			const double dx = (x - cx) / cx, dy = (y - cy) / cy;
			const int rx = std::clamp(int(std::lround(x + dx * maxShift)), 0, w - 1);
			const int ry = std::clamp(int(std::lround(y + dy * maxShift)), 0, h - 1);
			const int bx = std::clamp(int(std::lround(x - dx * maxShift)), 0, w - 1);
			const int by = std::clamp(int(std::lround(y - dy * maxShift)), 0, h - 1);
			dst[x * 4 + 0] = src.scanLine(ry)[rx * 4 + 0];
			dst[x * 4 + 2] = src.scanLine(by)[bx * 4 + 2];
		}
	}
}

} // namespace

const char *fxTypeName(FxType t)
{
	return defOf(t).name;
}

FxType fxTypeFromInt(int v)
{
	return (v >= 0 && v < kFxTypeCount) ? FxType(v) : FxType::Brightness;
}

QVector<FxParamDef> fxParams(FxType t)
{
	return defOf(t).params;
}

QMap<QString, double> fxDefaults(FxType t)
{
	QMap<QString, double> m;
	for (const FxParamDef &p : defOf(t).params)
		m.insert(QString::fromLatin1(p.key), p.def);
	return m;
}

QMap<QString, double> FxSpec::paramsAt(qint64 tMs) const
{
	if (keys.isEmpty())
		return params;
	// Start from the static values so a parameter no key mentions still has one.
	QMap<QString, double> out = params;
	auto pick = [&](const FxKey &k) {
		for (auto it = k.params.constBegin(); it != k.params.constEnd(); ++it)
			out[it.key()] = it.value();
	};
	if (keys.size() == 1 || tMs <= keys.front().tMs) {
		pick(keys.front());
		return out;
	}
	if (tMs >= keys.back().tMs) {
		pick(keys.back());
		return out;
	}
	int i = 0;
	while (i + 1 < keys.size() && keys[i + 1].tMs <= tMs)
		++i;
	const FxKey &a = keys[i];
	const FxKey &b = keys[i + 1];
	const qint64 span = std::max<qint64>(1, b.tMs - a.tMs);
	const double u = tlEaseAt(a.ease, double(tMs - a.tMs) / double(span), a.bez1, a.bez2);
	pick(a);
	for (auto it = b.params.constBegin(); it != b.params.constEnd(); ++it) {
		const double from = a.params.value(it.key(), params.value(it.key(), it.value()));
		out[it.key()] = from + (it.value() - from) * u;
	}
	return out;
}

bool Effects::isNoOp(const FxSpec &fx, const QMap<QString, double> &p)
{
	if (!fx.enabled)
		return true;
	auto v = [&](const char *k, double d = 0.0) { return p.value(QString::fromLatin1(k), d); };
	switch (fx.type) {
	case FxType::Brightness:
	case FxType::Contrast:
	case FxType::Saturation:
	case FxType::Sharpen:
	case FxType::ChromaticAberration:
		return std::abs(v("amount")) < 1e-6;
	case FxType::Exposure:
		return std::abs(v("stops")) < 1e-6;
	case FxType::HueShift:
		return std::abs(v("degrees")) < 1e-6;
	case FxType::ColorBalance:
		return std::abs(v("r")) < 1e-6 && std::abs(v("g")) < 1e-6 && std::abs(v("b")) < 1e-6;
	case FxType::Blur:
	case FxType::GaussianBlur:
		return v("radius") < 1e-6;
	case FxType::Vignette:
	case FxType::Glow:
	case FxType::Noise:
		return v("amount") < 1e-6;
	case FxType::Pixelate:
		return v("size") < 1e-6;
	case FxType::InverseSelection:
		return !fx.spot.active();
	case FxType::Count:
		break;
	}
	return false;
}

void Effects::apply(QImage &img, const FxSpec &fx, qint64 tMs, qint64 outMs)
{
	if (img.isNull())
		return;
	const QMap<QString, double> p = fx.paramsAt(tMs);
	if (isNoOp(fx, p))
		return; // an effect that would change nothing costs nothing
	auto v = [&](const char *k, double d = 0.0) { return p.value(QString::fromLatin1(k), d); };

	switch (fx.type) {
	case FxType::Brightness: {
		const double add = v("amount") * 255.0;
		perPixel(img, [add](double &r, double &g, double &b, int, int) {
			r += add;
			g += add;
			b += add;
		});
		break;
	}
	case FxType::Contrast: {
		// Pivot on mid grey so raising contrast doesn't also brighten.
		const double k = 1.0 + v("amount");
		perPixel(img, [k](double &r, double &g, double &b, int, int) {
			r = 128.0 + (r - 128.0) * k;
			g = 128.0 + (g - 128.0) * k;
			b = 128.0 + (b - 128.0) * k;
		});
		break;
	}
	case FxType::Saturation: {
		const double k = 1.0 + v("amount");
		perPixel(img, [k](double &r, double &g, double &b, int, int) {
			const double l = lumaOf(r, g, b);
			r = l + (r - l) * k;
			g = l + (g - l) * k;
			b = l + (b - l) * k;
		});
		break;
	}
	case FxType::Exposure: {
		const double k = std::pow(2.0, v("stops"));
		perPixel(img, [k](double &r, double &g, double &b, int, int) {
			r *= k;
			g *= k;
			b *= k;
		});
		break;
	}
	case FxType::HueShift: {
		const double deg = v("degrees");
		perPixel(img, [deg](double &r, double &g, double &b, int, int) {
			QColor c = QColor::fromRgb(clamp8(r), clamp8(g), clamp8(b)).toHsv();
			int h = c.hue();
			if (h < 0)
				h = 0; // grey has no hue; leave it grey
			c.setHsv((int(h + deg) % 360 + 360) % 360, c.saturation(), c.value());
			const QColor rgb = c.toRgb();
			r = rgb.red();
			g = rgb.green();
			b = rgb.blue();
		});
		break;
	}
	case FxType::ColorBalance: {
		const double dr = v("r") * 255.0, dg = v("g") * 255.0, db = v("b") * 255.0;
		perPixel(img, [dr, dg, db](double &r, double &g, double &b, int, int) {
			r += dr;
			g += dg;
			b += db;
		});
		break;
	}
	case FxType::Blur:
	case FxType::GaussianBlur:
		// The box blur is run three times, which IS a Gaussian approximation, so
		// the two differ only in how hard they hit: "Blur" is the cheap single
		// radius, "Gaussian" uses a wider one for a softer falloff.
		Spotlight::blurInPlace(img, std::max(1, radiusPx(v("radius") *
								(fx.type == FxType::GaussianBlur ? 1.6
												 : 1.0),
							  img)));
		break;
	case FxType::Sharpen:
		applySharpen(img, v("amount"));
		break;
	case FxType::Vignette:
		applyVignette(img, v("amount"), v("size", 0.75), v("softness", 0.45));
		break;
	case FxType::Glow:
		applyGlow(img, v("amount"), v("radius", 0.25), v("threshold", 0.6));
		break;
	case FxType::Pixelate:
		applyPixelate(img, v("size", 0.02));
		break;
	case FxType::Noise: {
		const double amt = v("amount") * 128.0;
		const bool mono = v("mono", 1.0) >= 0.5;
		perPixel(img, [amt, mono](double &r, double &g, double &b, int x, int y) {
			if (mono) {
				const double n = (hashNoise(x, y, 1) - 0.5) * amt;
				r += n;
				g += n;
				b += n;
			} else {
				r += (hashNoise(x, y, 1) - 0.5) * amt;
				g += (hashNoise(x, y, 2) - 0.5) * amt;
				b += (hashNoise(x, y, 3) - 0.5) * amt;
			}
		});
		break;
	}
	case FxType::ChromaticAberration:
		applyChromatic(img, v("amount"));
		break;
	case FxType::InverseSelection:
		Spotlight::apply(img, fx.spot, outMs);
		break;
	case FxType::Count:
		break;
	}
}

} // namespace harpia
