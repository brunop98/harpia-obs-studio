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

inline unsigned char clamp8(float v)
{
	return (unsigned char)std::clamp(v, 0.0f, 255.0f);
}

// Walk every pixel with a per-channel function. Everything colour-ish goes
// through here so the loop, the format check and the alpha handling exist once.
//
// float, not double: the destination is eight bits per channel, so single
// precision has three digits to spare, and it halves the arithmetic width of
// the hottest loop in the effect stack.
template <typename F> void perPixel(QImage &img, F fn)
{
	if (img.format() != QImage::Format_RGBA8888)
		img = img.convertToFormat(QImage::Format_RGBA8888);
	const int w = img.width(), h = img.height();
	for (int y = 0; y < h; ++y) {
		unsigned char *row = img.scanLine(y);
		for (int x = 0; x < w; ++x) {
			unsigned char *px = row + x * 4;
			float r = px[0], g = px[1], b = px[2];
			fn(r, g, b, x, y);
			px[0] = clamp8(r);
			px[1] = clamp8(g);
			px[2] = clamp8(b);
		}
	}
}

inline float lumaOf(float r, float g, float b)
{
	return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// A point operation — one whose output channel depends only on the SAME input
// channel — has 256 possible answers per channel. Working them out once and
// looking them up turns the per-pixel float conversion (which measurement
// showed was the whole cost of these effects) into a table read.
//
// `fn(channel, value)` is called 768 times, whatever the frame size.
template <typename F> void channelLut(QImage &img, F fn)
{
	unsigned char lut[3][256];
	for (int c = 0; c < 3; ++c)
		for (int v = 0; v < 256; ++v)
			lut[c][v] = clamp8(fn(c, float(v)));
	if (img.format() != QImage::Format_RGBA8888)
		img = img.convertToFormat(QImage::Format_RGBA8888);
	const int w = img.width(), h = img.height();
	for (int y = 0; y < h; ++y) {
		unsigned char *row = img.scanLine(y);
		for (int x = 0; x < w * 4; x += 4) {
			row[x + 0] = lut[0][row[x + 0]];
			row[x + 1] = lut[1][row[x + 1]];
			row[x + 2] = lut[2][row[x + 2]];
		}
	}
}

// The same idea for a 3x3 colour matrix, where an output channel mixes all
// three inputs: nine tables of pre-multiplied contributions, summed as integers
// in 16.16 so the pixel loop still never touches floating point.
void colourMatrix(QImage &img, const float m[9])
{
	int lut[9][256];
	for (int i = 0; i < 9; ++i)
		for (int v = 0; v < 256; ++v)
			lut[i][v] = int(std::lround(double(m[i]) * v * 65536.0));
	if (img.format() != QImage::Format_RGBA8888)
		img = img.convertToFormat(QImage::Format_RGBA8888);
	const int w = img.width(), h = img.height();
	for (int y = 0; y < h; ++y) {
		unsigned char *row = img.scanLine(y);
		for (int x = 0; x < w * 4; x += 4) {
			const int r = row[x], g = row[x + 1], b = row[x + 2];
			const int nr = lut[0][r] + lut[1][g] + lut[2][b];
			const int ng = lut[3][r] + lut[4][g] + lut[5][b];
			const int nb = lut[6][r] + lut[7][g] + lut[8][b];
			row[x + 0] = (unsigned char)std::clamp((nr + 32768) >> 16, 0, 255);
			row[x + 1] = (unsigned char)std::clamp((ng + 32768) >> 16, 0, 255);
			row[x + 2] = (unsigned char)std::clamp((nb + 32768) >> 16, 0, 255);
		}
	}
}

// Rotate a pixel's hue without building a QColor. The old path constructed
// three QColor objects per pixel (fromRgb, toHsv, toRgb), which cost more than
// every other effect in the stack put together; this is the same HSV rotation
// written out, with saturation and value preserved exactly because they come
// straight back out of the max/min the conversion already computed.
// Hue is carried in sextants (0..6) rather than degrees: the conversion needs
// the sextant and the fraction within it anyway, so working in degrees only
// added two divisions by 60 and two fmod calls per pixel. `deg6` is the shift
// already reduced to [0, 6), and `recip` is 1/chroma for every chroma there is,
// which removes the last division from the loop.
inline void rotateHue(unsigned char *px, float deg6, const float *recip)
{
	const int r = px[0], g = px[1], b = px[2];
	const int mx = std::max(r, std::max(g, b));
	const int mn = std::min(r, std::min(g, b));
	const int d = mx - mn;
	if (d == 0)
		return; // grey has no hue to turn

	const float dInv = recip[d];
	float h6;
	if (mx == r)
		h6 = float(g - b) * dInv; // (-1, 1)
	else if (mx == g)
		h6 = 2.0f + float(b - r) * dInv;
	else
		h6 = 4.0f + float(r - g) * dInv;
	// h6 is in (-1, 5] and deg6 in [0, 6), so one fix-up each way is enough.
	h6 += deg6;
	if (h6 < 0.0f)
		h6 += 6.0f;
	if (h6 >= 6.0f)
		h6 -= 6.0f;

	// Back to RGB. `mx` is the value and `d` the chroma, so the two extremes
	// land on exactly the bytes they came from and only the middle channel
	// moves — a rotation of 0 is bit-for-bit the identity.
	const int sector = int(h6);
	const float f = h6 - float(sector);
	const auto mid = [&](float t) { return (unsigned char)(float(mn) + float(d) * t + 0.5f); };
	switch (sector) {
	case 0: px[0] = (unsigned char)mx; px[1] = mid(f);        px[2] = (unsigned char)mn; break;
	case 1: px[0] = mid(1.0f - f);     px[1] = (unsigned char)mx; px[2] = (unsigned char)mn; break;
	case 2: px[0] = (unsigned char)mn; px[1] = (unsigned char)mx; px[2] = mid(f); break;
	case 3: px[0] = (unsigned char)mn; px[1] = mid(1.0f - f);  px[2] = (unsigned char)mx; break;
	case 4: px[0] = mid(f);            px[1] = (unsigned char)mn; px[2] = (unsigned char)mx; break;
	default: px[0] = (unsigned char)mx; px[1] = (unsigned char)mn; px[2] = mid(1.0f - f); break;
	}
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
// Unsigned throughout: the multiplies are meant to wrap, and doing that in
// signed int is undefined behaviour the compiler is entitled to exploit.
inline unsigned hashNoise16(int x, int y, int salt)
{
	unsigned int n = unsigned(x) * 374761393u + unsigned(y) * 668265263u +
			 unsigned(salt) * 1274126177u;
	n = (n ^ (n >> 13)) * 1274126177u;
	return (n ^ (n >> 16)) & 0xffff;
}

void applyVignette(QImage &img, double amount, double size, double softness)
{
	const double cx = img.width() / 2.0, cy = img.height() / 2.0;
	const double maxR = std::sqrt(cx * cx + cy * cy);
	const double inner = std::max(0.001, size) * maxR;
	const double outer = inner + std::max(0.01, softness) * maxR;
	if (img.format() != QImage::Format_RGBA8888)
		img = img.convertToFormat(QImage::Format_RGBA8888);
	const int w = img.width(), h = img.height();

	// The falloff is a function of the frame size and the three parameters
	// only — nothing about the picture — so playing back a clip recomputes the
	// identical field for every frame. Build it once as 8.8 fixed point and keep
	// it; the per-frame work is then just the multiply. (Per-thread, because the
	// preview and the exporter render at different sizes at the same time.)
	struct Field {
		int w = 0, h = 0;
		double amount = 0, size = 0, softness = 0;
		std::vector<unsigned short> fac;
	};
	static thread_local Field f;
	if (f.w != w || f.h != h || f.amount != amount || f.size != size ||
	    f.softness != softness) {
		f.w = w;
		f.h = h;
		f.amount = amount;
		f.size = size;
		f.softness = softness;
		f.fac.assign(size_t(w) * size_t(h), 256);
		// dx² only depends on the column, so it is computed once for the frame.
		std::vector<float> dx2(size_t(std::max(1, w)), 0.0f);
		for (int x = 0; x < w; ++x)
			dx2[size_t(x)] = float((x - cx) * (x - cx));
		const float innerF = float(inner);
		const float invSpan = 1.0f / float(std::max(1.0, outer - inner));
		const float amountF = float(amount);
		for (int y = 0; y < h; ++y) {
			const float dy2 = float((y - cy) * (y - cy));
			unsigned short *fr = f.fac.data() + size_t(y) * size_t(w);
			for (int x = 0; x < w; ++x) {
				const float d = std::sqrt(dx2[size_t(x)] + dy2);
				float k = std::clamp((d - innerF) * invSpan, 0.0f, 1.0f);
				k = k * k * (3.0f - 2.0f * k); // smooth: no visible ring
				fr[x] = (unsigned short)((1.0f - amountF * k) * 256.0f + 0.5f);
			}
		}
	}

	for (int y = 0; y < h; ++y) {
		unsigned char *row = img.scanLine(y);
		const unsigned short *fr = f.fac.data() + size_t(y) * size_t(w);
		for (int x = 0; x < w; ++x) {
			unsigned char *px = row + x * 4;
			const int k = fr[x];
			px[0] = (unsigned char)((px[0] * k) >> 8);
			px[1] = (unsigned char)((px[1] * k) >> 8);
			px[2] = (unsigned char)((px[2] * k) >> 8);
		}
	}
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
	//
	// Convert FIRST, then copy: converting afterwards used to leave two separate
	// full-frame allocations (the convert, then the blur's detach) where one
	// will do.
	if (img.format() != QImage::Format_RGBA8888)
		img = img.convertToFormat(QImage::Format_RGBA8888);
	QImage soft = img.copy();
	Spotlight::blurInPlace(soft, std::max(1, radiusPx(0.06, img)));
	const int w = img.width(), h = img.height();
	// 8.8 fixed point: the difference is at most ±255, so amount*diff stays well
	// inside an int and the whole loop avoids floating point entirely.
	const int amt = int(std::lround(std::clamp(amount, 0.0, 8.0) * 256.0));
	for (int y = 0; y < h; ++y) {
		unsigned char *a = img.scanLine(y);
		const unsigned char *s = soft.scanLine(y);
		for (int x = 0; x < w * 4; x += 4)
			for (int c = 0; c < 3; ++c)
				a[x + c] = (unsigned char)std::clamp(
					a[x + c] + ((amt * (a[x + c] - s[x + c])) >> 8), 0, 255);
	}
}

void applyGlow(QImage &img, double amount, double radius, double threshold)
{
	if (img.format() != QImage::Format_RGBA8888)
		img = img.convertToFormat(QImage::Format_RGBA8888);
	// Keep only what is brighter than the threshold, blur that, add it back.
	// Both passes stay in integers: the threshold test is a luma weighted by
	// 1024ths, and the add-back is 8.8 fixed point.
	QImage bright = img.copy();
	const int w = img.width(), h = img.height();
	const int cut = int(std::lround(std::clamp(threshold, 0.0, 1.0) * 255.0)) * 1024;
	for (int y = 0; y < h; ++y) {
		unsigned char *px = bright.scanLine(y);
		for (int x = 0; x < w * 4; x += 4) {
			const int l = 218 * px[x] + 732 * px[x + 1] + 74 * px[x + 2];
			if (l <= cut)
				px[x] = px[x + 1] = px[x + 2] = 0;
		}
	}
	Spotlight::blurInPlace(bright, std::max(1, radiusPx(radius, img)));
	const int amt = int(std::lround(std::clamp(amount, 0.0, 4.0) * 256.0));
	for (int y = 0; y < h; ++y) {
		unsigned char *a = img.scanLine(y);
		const unsigned char *s = bright.scanLine(y);
		for (int x = 0; x < w * 4; x += 4)
			for (int c = 0; c < 3; ++c)
				a[x + c] = (unsigned char)std::clamp(
					a[x + c] + ((amt * s[x + c]) >> 8), 0, 255);
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
	// The displacement is separable: the source column depends only on x and the
	// source row only on y. Four small tables replace four roundings, two
	// divisions and four clamps per pixel with four array reads.
	std::vector<int> rxT(size_t(w), 0), bxT(size_t(w), 0), ryT(size_t(h), 0), byT(size_t(h), 0);
	for (int x = 0; x < w; ++x) {
		const double d = (x - cx) / cx * maxShift;
		rxT[size_t(x)] = std::clamp(int(std::lround(x + d)), 0, w - 1) * 4;
		bxT[size_t(x)] = std::clamp(int(std::lround(x - d)), 0, w - 1) * 4;
	}
	for (int y = 0; y < h; ++y) {
		const double d = (y - cy) / cy * maxShift;
		ryT[size_t(y)] = std::clamp(int(std::lround(y + d)), 0, h - 1);
		byT[size_t(y)] = std::clamp(int(std::lround(y - d)), 0, h - 1);
	}
	for (int y = 0; y < h; ++y) {
		unsigned char *dst = img.scanLine(y);
		const unsigned char *rrow = src.constScanLine(ryT[size_t(y)]);
		const unsigned char *brow = src.constScanLine(byT[size_t(y)]);
		for (int x = 0; x < w; ++x) {
			dst[x * 4 + 0] = rrow[rxT[size_t(x)] + 0];
			dst[x * 4 + 2] = brow[bxT[size_t(x)] + 2];
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

void FxSpec::setKeyAt(qint64 tMs, const QMap<QString, double> &values)
{
	for (FxKey &k : keys)
		if (k.tMs == tMs) {
			k.params = values;
			return;
		}
	FxKey k;
	k.tMs = tMs;
	k.params = values;
	keys.append(k);
	std::sort(keys.begin(), keys.end(),
		  [](const FxKey &a, const FxKey &b) { return a.tMs < b.tMs; });
}

void FxSpec::removeKeyAt(qint64 tMs)
{
	for (int i = 0; i < keys.size(); ++i) {
		if (keys[i].tMs != tMs)
			continue;
		keys.remove(i);
		// One key left is a static setting written in an awkward place: fold it
		// back into params so the Inspector's spin boxes drive it again.
		if (keys.size() == 1) {
			for (auto it = keys.front().params.constBegin();
			     it != keys.front().params.constEnd(); ++it)
				params[it.key()] = it.value();
			keys.clear();
		}
		return;
	}
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
		const float add = float(v("amount") * 255.0);
		channelLut(img, [add](int, float x) { return x + add; });
		break;
	}
	case FxType::Contrast: {
		// Pivot on mid grey so raising contrast doesn't also brighten.
		const float k = float(1.0 + v("amount"));
		channelLut(img, [k](int, float x) { return 128.0f + (x - 128.0f) * k; });
		break;
	}
	case FxType::Saturation: {
		// Pulling every channel towards or away from the luma is linear in all
		// three inputs, so saturation is exactly a colour matrix.
		const float k = float(1.0 + v("amount"));
		const float lr = 0.2126f * (1.0f - k), lg = 0.7152f * (1.0f - k),
			    lb = 0.0722f * (1.0f - k);
		const float m[9] = {lr + k, lg,     lb,
				    lr,     lg + k, lb,
				    lr,     lg,     lb + k};
		colourMatrix(img, m);
		break;
	}
	case FxType::Exposure: {
		const float k = float(std::pow(2.0, v("stops")));
		channelLut(img, [k](int, float x) { return x * k; });
		break;
	}
	case FxType::HueShift: {
		float deg6 = std::fmod(float(v("degrees")) / 60.0f, 6.0f);
		if (deg6 < 0.0f)
			deg6 += 6.0f;
		float recip[256];
		recip[0] = 0.0f;
		for (int i = 1; i < 256; ++i)
			recip[i] = 1.0f / float(i);
		if (img.format() != QImage::Format_RGBA8888)
			img = img.convertToFormat(QImage::Format_RGBA8888);
		const int w = img.width(), h = img.height();
		for (int y = 0; y < h; ++y) {
			unsigned char *row = img.scanLine(y);
			for (int x = 0; x < w * 4; x += 4)
				rotateHue(row + x, deg6, recip);
		}
		break;
	}
	case FxType::ColorBalance: {
		const float d[3] = {float(v("r") * 255.0), float(v("g") * 255.0),
				    float(v("b") * 255.0)};
		channelLut(img, [&d](int c, float x) { return x + d[c]; });
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
		// The grain is a 16-bit hash, so scaling it is an integer shift rather
		// than a conversion to double and back for every channel.
		const int amt = int(std::lround(std::clamp(v("amount"), 0.0, 1.0) * 128.0));
		const bool mono = v("mono", 1.0) >= 0.5;
		if (img.format() != QImage::Format_RGBA8888)
			img = img.convertToFormat(QImage::Format_RGBA8888);
		const int w = img.width(), h = img.height();
		const auto grain = [amt](int x, int y, int salt) {
			// hash is 0..65535; centre it, then scale by amt/65536.
			return ((int(hashNoise16(x, y, salt)) - 32768) * amt) >> 16;
		};
		for (int y = 0; y < h; ++y) {
			unsigned char *row = img.scanLine(y);
			for (int x = 0; x < w; ++x) {
				unsigned char *px = row + x * 4;
				if (mono) {
					const int n = grain(x, y, 1);
					for (int c = 0; c < 3; ++c)
						px[c] = (unsigned char)std::clamp(px[c] + n, 0,
										  255);
				} else {
					for (int c = 0; c < 3; ++c)
						px[c] = (unsigned char)std::clamp(
							px[c] + grain(x, y, c + 1), 0, 255);
				}
			}
		}
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
