// The point-op pixel passes are split across cores. Splitting must not change
// a single pixel.
//
// Band boundaries are where this kind of change goes wrong: an off-by-one in
// the row range leaves a seam or a duplicated line, and both are easy to miss
// by eye on a photograph. So the check is not "it looks right" but "the frame
// is byte-identical to the one-band result", over every band count from 1 to 8
// — including counts that do not divide the height evenly, which is exactly
// where a rounding mistake in `h * b / bands` would show.
//
// This test exists because the pixel-equality test that already covered the
// effects runs at 160x120, far below the size threshold that decides whether to
// thread at all. It therefore exercised only the SERIAL path and would have
// passed however wrong the banded one was. Effects::setPixelBandsForTest is
// what makes the threaded path reachable from a test.
#include "editor/timeline/EffectClip.hpp"

#include <QGuiApplication>
#include <QImage>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// Content that varies in both directions, so a row taken from the wrong band is
// a different colour rather than an identical one.
static QImage gradient(int w, int h)
{
	QImage img(w, h, QImage::Format_RGBA8888);
	for (int y = 0; y < h; ++y) {
		auto *row = reinterpret_cast<QRgb *>(img.scanLine(y));
		for (int x = 0; x < w; ++x)
			row[x] = qRgba((x * 7 + y * 3) & 0xff, (y * 5) & 0xff,
				       (x * 11 + y * 13) & 0xff, 255);
	}
	return img;
}

static bool identical(const QImage &a, const QImage &b, int *firstBadRow)
{
	if (a.size() != b.size() || a.format() != b.format())
		return false;
	for (int y = 0; y < a.height(); ++y)
		if (std::memcmp(a.constScanLine(y), b.constScanLine(y), size_t(a.width()) * 4) != 0) {
			if (firstBadRow)
				*firstBadRow = y;
			return false;
		}
	return true;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);

	// A height that divides badly by most band counts (241 is prime), so the
	// row-range arithmetic is under real pressure rather than landing evenly.
	const int W = 337, H = 241;

	// Both kinds of pass: a per-channel LUT and a 3x3 colour matrix. They have
	// separate loops and separate opportunities to get the bands wrong.
	struct Case {
		FxType type;
		const char *key;
		double value;
		const char *name;
	};
	const Case cases[] = {
		{FxType::Brightness, "amount", 0.35, "Brightness (channel LUT)"},
		{FxType::Contrast, "amount", 0.60, "Contrast (channel LUT)"},
		{FxType::Saturation, "amount", 0.80, "Saturation (colour matrix)"},
		{FxType::HueShift, "degrees", 95.0, "Hue shift (colour matrix)"},
		{FxType::ColorBalance, "temperature", 0.5, "Colour balance (colour matrix)"},
	};

	for (const Case &c : cases) {
		FxSpec fx;
		fx.type = c.type;
		fx.enabled = true;
		fx.params = fxDefaults(c.type);
		fx.params[QString::fromLatin1(c.key)] = c.value;

		// The reference: one band, i.e. the serial loop.
		Effects::setPixelBandsForTest(1);
		QImage ref = gradient(W, H);
		Effects::apply(ref, fx, 0);

		bool allSame = true;
		int badBands = 0, badRow = -1;
		for (int bands = 2; bands <= 8; ++bands) {
			Effects::setPixelBandsForTest(bands);
			QImage got = gradient(W, H);
			Effects::apply(got, fx, 0);
			int row = -1;
			if (!identical(ref, got, &row)) {
				allSame = false;
				badBands = bands;
				badRow = row;
				break;
			}
		}
		if (allSame)
			std::printf("  PASS %s — identical at 2..8 bands\n", c.name);
		else {
			std::printf("  FAIL %s — differs at %d bands, first bad row %d\n", c.name,
				    badBands, badRow);
			++failures;
		}
	}
	Effects::setPixelBandsForTest(0); // back to normal for anything after

	std::printf("\n-- and the effect actually did something --\n");
	{
		// Guards the whole file: if apply() were a no-op every comparison above
		// would pass trivially.
		FxSpec fx;
		fx.type = FxType::Brightness;
		fx.enabled = true;
		fx.params = fxDefaults(FxType::Brightness);
		fx.params[QStringLiteral("amount")] = 0.35;
		QImage before = gradient(W, H);
		QImage after = before;
		Effects::apply(after, fx, 0);
		int row = -1;
		ok(!identical(before, after, &row), "the frame changed, so the comparisons mean something");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
