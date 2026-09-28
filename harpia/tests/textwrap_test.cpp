// A caption wider than the picture wraps: lines break at spaces so the block
// stays inside the canvas width, typed line breaks are kept, a single long
// word is left whole, and a narrower canvas wraps sooner.
#include "editor/timeline/TimelineCompositor.hpp"
#include "editor/timeline/TimelineModel.hpp"

#include <QApplication>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace harpia;

static int failures = 0;
static void ok(bool cond, const char *what)
{
	std::printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		++failures;
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	const QSize canvas(1920, 1080);

	TlText t;
	t.fontPx = 64;
	t.text = QStringLiteral("Hi");
	const QSize one = TimelineCompositor::textNaturalSize(t, canvas);
	ok(one.height() > 0 && one.width() > 0, "a short caption has a size");
	const int lineH = one.height();

	t.text = QStringLiteral("No hashtags, my game found your secret and it is a very long caption indeed");
	const QSize wrapped = TimelineCompositor::textNaturalSize(t, canvas);
	ok(wrapped.width() <= canvas.width(), "a long caption never gets wider than the canvas");
	ok(wrapped.height() >= 2 * lineH - 2, "it wraps onto more lines instead");
	std::printf("     %d x %d, line %d\n", wrapped.width(), wrapped.height(), lineH);

	const QSize narrow = TimelineCompositor::textNaturalSize(t, QSize(1080, 1080));
	ok(narrow.width() <= 1080 && narrow.height() > wrapped.height(),
	   "on a narrower canvas it wraps sooner and grows taller");

	t.text = QStringLiteral("one\ntwo");
	ok(std::abs(TimelineCompositor::textNaturalSize(t, canvas).height() - 2 * lineH) <= 2, "typed line breaks are kept");

	t.text = QStringLiteral("Supercalifragilisticexpialidocious_and_then_some_more_letters_without_a_space_anywhere");
	const QSize word = TimelineCompositor::textNaturalSize(t, canvas);
	ok(std::abs(word.height() - lineH) <= 1, "a single word with no spaces stays on one line, however long");

	// The exported frame agrees: drawing the long caption at scale 1 leaves
	// its ends on screen (the leftmost and rightmost columns stay empty).
	t.text = QStringLiteral("No hashtags, my game found your secret and it is a very long caption indeed");
	TlClip c;
	c.type = TlClip::Type::Text;
	c.text = t;
	c.text.outlineWidth = 0;
	QImage img(canvas, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::black);
	{
		QPainter p(&img);
		TimelineCompositor::drawClip(p, c, c.baseTransform(), canvas, QImage());
	}
	bool edgeInk = false;
	for (int y = 0; y < img.height(); ++y)
		if (qGray(img.pixel(0, y)) > 40 || qGray(img.pixel(img.width() - 1, y)) > 40)
			edgeInk = true;
	ok(!edgeInk, "and the drawn caption touches neither side of the frame");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
