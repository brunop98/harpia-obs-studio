// A fitted caption background follows the laid-out lines, and is composited
// once: padding shared by two lines must not make a translucent sticker dark.
#include "editor/timeline/TimelineCompositor.hpp"

#include <QDir>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}

QImage render(const TlText &text, QSize canvas = QSize(1024, 1080), bool direct = false,
	      TlTransform transform = TlTransform())
{
	QImage image(canvas, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);
	if (direct) {
		// Force the non-sprite route while retaining the exact same final
		// geometry. 1/1024 is exact, so this introduces no sub-pixel error.
		painter.translate(1.0, 0.0);
		transform.posX -= 1.0 / canvas.width();
	}
	TlClip clip;
	clip.type = TlClip::Type::Text;
	clip.text = text;
	TimelineCompositor::drawClip(painter, clip, transform, canvas, QImage());
	return image;
}

int alphaAt(const QImage &image, int x, int y)
{
	return image.rect().contains(x, y) ? qAlpha(image.pixel(x, y)) : 0;
}

int filledPixels(const QImage &image)
{
	int count = 0;
	for (int y = 0; y < image.height(); ++y)
		for (int x = 0; x < image.width(); ++x)
			count += alphaAt(image, x, y) > 96;
	return count;
}

QPair<int, int> rowBounds(const QImage &image, int y)
{
	int left = image.width(), right = -1;
	for (int x = 0; x < image.width(); ++x)
		if (alphaAt(image, x, y) > 96) {
			left = std::min(left, x);
			right = x;
		}
	return {left, right};
}

bool coveredByNaturalSize(const TlText &text, const QImage &image)
{
	const QSize natural = TimelineCompositor::textNaturalSize(text, image.size());
	const QRectF selection(image.width() / 2.0 - natural.width() / 2.0 - 1.0,
		image.height() / 2.0 - natural.height() / 2.0 - 1.0,
		natural.width() + 2.0, natural.height() + 2.0);
	for (int y = 0; y < image.height(); ++y)
		for (int x = 0; x < image.width(); ++x)
			if (alphaAt(image, x, y) > 0 && !selection.contains(QPointF(x + 0.5, y + 0.5)))
				return false;
	return true;
}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	TlText text;
	text.fontFamily = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
	text.fontPx = 64;
	text.text = QStringLiteral("MMMMMMMM\nHi");
	text.color = QColor(0, 0, 0, 0); // isolate the background's pixels
	text.outlineWidth = 0.0;
	text.boxEnabled = true;
	text.boxColor = QColor(Qt::red);
	text.boxOpacity = 0.5;
	text.boxPadX = 20;
	text.boxPadY = 4;
	text.boxRadius = 12;
	QFont font(text.fontFamily);
	font.setPixelSize(text.fontPx);
	const QFontMetricsF metrics(font);
	const double height = metrics.height();
	const double wide = metrics.horizontalAdvance(QStringLiteral("MMMMMMMM"));
	const double narrow = metrics.horizontalAdvance(QStringLiteral("Hi"));
	const int secondRow = int(std::lround(540.0 + height / 2.0));
	const int shoulderX = int(std::lround(512.0 + (wide + narrow) / 4.0));
	const QImage rectangle = render(text);
	text.boxShape = TlTextBoxFitText;
	const QImage fitted = render(text);
	check(alphaAt(rectangle, shoulderX, secondRow) > 120 && alphaAt(fitted, shoulderX, secondRow) == 0,
	      "the short line's background narrows instead of retaining the longest line's rectangle");
	check(alphaAt(fitted, 512, secondRow) > 120 && filledPixels(fitted) > 1000,
	      "the fitted shape still covers the actual short line");
	text.boxShape = TlTextBoxRectangle;
	check(render(text) == rectangle, "switching shapes cannot return another shape from the sprite cache");
	text.boxShape = TlTextBoxFitText;
	check(render(text) == fitted, "a repeated fitted caption uses identical cached pixels");
	check(render(text, QSize(1024, 1080), true) == fitted,
	      "cached and direct rendering agree for the same fitted caption");

	for (int align = 0; align <= 2; ++align) {
		text.align = align;
		const QImage aligned = render(text);
		const QPair<int, int> actual = rowBounds(aligned, secondRow);
		const double left = align == 0 ? 512.0 - wide / 2.0
			: align == 1 ? 512.0 - narrow / 2.0 : 512.0 + wide / 2.0 - narrow;
		check(actual.second > actual.first && std::abs(actual.first - (left - text.boxPadX)) <= 2.0 &&
		      std::abs(actual.second - (left + narrow + text.boxPadX)) <= 2.0,
		      "each line's fitted background respects its left, centre or right alignment");
	}
	text.align = 1;
	text.boxPadY = 40; // adjacent bands overlap over most of a line
	const QImage overlapping = render(text);
	int maximum = 0;
	for (int y = 0; y < overlapping.height(); ++y)
		for (int x = 0; x < overlapping.width(); ++x)
			maximum = std::max(maximum, alphaAt(overlapping, x, y));
	check(maximum >= 127 && maximum <= 128,
	      "overlapping padding is painted once, without a darker translucent seam");
	text.boxPadY = 4;
	text.boxRadius = 0;
	const QImage square = render(text);
	text.boxRadius = 20;
	check(filledPixels(render(text)) < filledPixels(square),
	      "corner radius rounds the joined silhouette rather than changing its line widths");
	text.boxRadius = 200;
	check(coveredByNaturalSize(text, render(text)), "large fitted corner radii remain inside the caption's selectable bounds");

	text.boxRadius = 0;
	text.boxPadY = 0;
	text.text = QStringLiteral("MMMMMMMM\n  \nHi");
	check(rowBounds(render(text), 540).second < 0,
	      "a blank line retains its vertical spacing without painting a full-width background");
	text.text = QStringLiteral("MM MM MM MM MM Hi");
	const QSize narrowCanvas(360, 1080);
	const QImage wrapped = render(text, narrowCanvas);
	text.boxShape = TlTextBoxRectangle;
	const QImage wrappedRectangle = render(text, narrowCanvas);
	text.boxShape = TlTextBoxFitText;
	check(TimelineCompositor::textNaturalSize(text, narrowCanvas).height() >= height * 2.0 &&
	      filledPixels(wrapped) < filledPixels(wrappedRectangle),
	      "automatic wrapping fits the resulting rendered lines as well as typed line breaks");

	text.text = QStringLiteral("<size=96>MMMMMM</size>\n<size=48>Hi</size>");
	text.boxPadY = 4;
	text.boxRadius = 12;
	font.setPixelSize(96);
	const double bigHeight = QFontMetricsF(font).height();
	font.setPixelSize(48);
	const double smallWidth = QFontMetricsF(font).horizontalAdvance(QStringLiteral("Hi"));
	const QImage rich = render(text);
	const QPair<int, int> richLastLine = rowBounds(rich, int(std::lround(540.0 + bigHeight / 2.0)));
	check(richLastLine.second > richLastLine.first &&
	      std::abs((richLastLine.second - richLastLine.first + 1) - (smallWidth + 2 * text.boxPadX)) <= 3.0,
	      "rich-text lines use their own font sizes and ignore markup when fitting the background");
	check(render(text, QSize(1024, 1080), true) == rich,
	      "rich-text fitted backgrounds match between cached and direct rendering");
	text.text = QStringLiteral("<i>ffffj</i>\n<size=120>j</size>");
	text.boxPadX = text.boxPadY = 0;
	check(coveredByNaturalSize(text, render(text)), "selection bounds include rich glyph overhang with no background padding");
	text.text = QString::fromUtf8("MMMM 😀 MMMM\n😀");
	check(coveredByNaturalSize(text, render(text)), "selection bounds include the fitted emoji line bands");

	text.text = QStringLiteral("MMMMMMMM\nHi");
	text.boxPadX = 20;
	text.boxPadY = 12;
	TlTransform faded;
	faded.opacity = 0.5;
	const QImage fade = render(text, QSize(1024, 1080), false, faded);
	check(alphaAt(fade, 512, secondRow) >= 63 && alphaAt(fade, 512, secondRow) <= 65,
	      "animated clip opacity still composes the fitted background exactly once");
	TlTransform rotated;
	rotated.rotation = 90;
	const QImage turn = render(text, QSize(1024, 1080), false, rotated);
	check(alphaAt(turn, int(std::lround(512.0 - height / 2.0)), 540) > 120,
	      "the joined background follows the caption's rotation");

	if (argc > 1) {
		QDir output(QString::fromLocal8Bit(argv[1]));
		QDir().mkpath(output.absolutePath());
		TlText example = text;
		example.text = QStringLiteral("creating a dark\nfantasy game day\n145");
		example.fontPx = 96;
		example.bold = true;
		example.color = Qt::white;
		example.boxColor = Qt::black;
		example.boxOpacity = 0.75;
		example.boxPadX = 22;
		example.boxPadY = 12;
		example.boxRadius = 18;
		example.boxShape = TlTextBoxRectangle;
		check(render(example).save(output.filePath(QStringLiteral("rectangle.png"))), "the rectangular comparison artifact is saved");
		example.boxShape = TlTextBoxFitText;
		check(render(example).save(output.filePath(QStringLiteral("fitted.png"))), "the fitted social-caption example is saved");
	}
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
