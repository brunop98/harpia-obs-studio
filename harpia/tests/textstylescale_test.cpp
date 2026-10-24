// Caption outlines use the same 1080-height reference as their font and box.
// A lower-resolution preview must not turn a fine edge into a thick border;
// larger exports must retain the authored outline-to-letter proportion too.
#include "editor/timeline/TimelineCompositor.hpp"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

static QImage captionAt(int canvasHeight, double outline, bool direct)
{
	const QSize canvas(canvasHeight, canvasHeight);
	QImage image(canvas, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	TlClip clip;
	clip.type = TlClip::Type::Text;
	clip.text.text = QStringLiteral("H");
	clip.text.fontPx = 144; // exact integer font sizes at every tested scale
	clip.text.bold = true;
	clip.text.outlineWidth = outline;
	clip.text.color = QColor(Qt::white);
	clip.text.outlineColor = QColor(Qt::black);
	TlTransform pose = clip.baseTransform();
	if (direct)
		pose.opacity = 0.75; // fading uses the direct path instead of a sprite
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);
	TimelineCompositor::drawClip(painter, clip, pose, canvas, QImage());
	return image;
}

static QRect inkBounds(const QImage &image)
{
	int left = image.width(), top = image.height(), right = -1, bottom = -1;
	for (int y = 0; y < image.height(); ++y) {
		const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
		for (int x = 0; x < image.width(); ++x)
			if (qAlpha(line[x]) >= 16) {
				left = std::min(left, x);
				top = std::min(top, y);
				right = std::max(right, x);
				bottom = std::max(bottom, y);
			}
	}
	return right >= left ? QRect(QPoint(left, top), QPoint(right, bottom)) : QRect();
}

int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);
	for (bool direct : {false, true}) {
		std::printf("\n-- %s caption rendering --\n", direct ? "direct/fading" : "cached sprite");
		for (int height : {360, 540, 1080, 2160}) {
			const QRect plain = inkBounds(captionAt(height, 0, direct));
			const QRect outlined = inkBounds(captionAt(height, 12, direct));
			const double expectedGrowth = 24.0 * height / 1080.0;
			const int widthGrowth = outlined.width() - plain.width();
			const int heightGrowth = outlined.height() - plain.height();
			std::printf("    %dpx canvas: outline adds %d x %dpx, expected %.1fpx per dimension\n",
				    height, widthGrowth, heightGrowth, expectedGrowth);
			check(!plain.isEmpty() && !outlined.isEmpty(), "both plain and outlined captions render");
			check(std::abs(widthGrowth - expectedGrowth) <= 2.0 &&
			      std::abs(heightGrowth - expectedGrowth) <= 2.0,
			      "outline growth scales with the caption font and canvas height");
			check(inkBounds(captionAt(height, 12, direct)) == outlined,
			      "reusing a cached path or sprite preserves the scaled outline bounds");
		}
	}
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
