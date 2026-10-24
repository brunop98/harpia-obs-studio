// Social guides are widget overlays, positioned against the whole output
// canvas. They must not enter a clean frame or change preview hit testing.
#include "editor/EditorWidgets.hpp"
#include "editor/SocialSafeAreas.hpp"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>

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

static QImage painted(PreviewCanvas &canvas)
{
	QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	canvas.render(&image);
	return image;
}

static QPoint at(const QRectF &rect, double x, double y)
{
	return QPoint(int(rect.x() + x * rect.width()), int(rect.y() + y * rect.height()));
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	check(socialSafeAreaLayout(SocialSafeArea::Off).content.isEmpty(), "Off has no guide geometry");
	QVector<QRectF> safeRects;
	for (const auto mode : {SocialSafeArea::TikTok, SocialSafeArea::InstagramReels, SocialSafeArea::YouTubeShorts}) {
		const auto layout = socialSafeAreaLayout(mode);
		safeRects.append(layout.content);
		check(!layout.label.isEmpty() && QRectF(0, 0, 1, 1).contains(layout.content),
		      "a platform names its guide and keeps its safe area inside the canvas");
		check(layout.content.left() > 0 && layout.content.top() > 0 &&
		      layout.content.right() < 0.9 && layout.content.bottom() < 0.85,
		      "platform controls leave a useful inset for text and faces");
		double covered = layout.content.width() * layout.content.height();
		bool disjoint = true;
		for (int i = 0; i < layout.interfaceAreas.size(); ++i) {
			const QRectF area = layout.interfaceAreas[i];
			covered += area.width() * area.height();
			disjoint &= area.intersected(layout.content).isEmpty();
			for (int j = 0; j < i; ++j)
				disjoint &= area.intersected(layout.interfaceAreas[j]).isEmpty();
		}
		check(disjoint && std::abs(covered - 1.0) < 1e-9,
		      "interface shading covers only the unsafe margins, without overlapping corners");
	}
	check(safeRects[0] != safeRects[1] && safeRects[1] != safeRects[2] && safeRects[0] != safeRects[2],
	      "TikTok, Reels and Shorts have separate guide layouts");
	check(mapSocialSafeRect(QRectF(0.1, 0.2, 0.7, 0.6), QRectF(50, 30, 200, 400)) == QRectF(70, 110, 140, 240),
	      "normalised guides map through canvas offsets and scale");
	check(mapSocialSafeRect({}, QRectF(0, 0, 100, 100)).isEmpty(), "empty guide geometry stays empty");

	PreviewCanvas canvas;
	canvas.setLayoutParams({1, 1});
	canvas.resize(800, 640);
	canvas.setVideoSize(1080, 1920);
	QImage frame(360, 640, QImage::Format_RGBA8888);
	frame.fill(QColor(150, 170, 190));
	canvas.setFrame(frame);
	const qint64 originalKey = canvas.currentFrame().cacheKey();
	const QImage off = painted(canvas);
	// 9:16 at640 high is360 wide, centred in an800-wide widget.
	const QRectF output(220, 0, 360, 640);
	check(canvas.socialSafeArea() == SocialSafeArea::Off, "guides start disabled");
	for (const auto mode : {SocialSafeArea::TikTok, SocialSafeArea::InstagramReels, SocialSafeArea::YouTubeShorts}) {
		canvas.setSocialSafeArea(mode);
		const QImage on = painted(canvas);
		check(on != off, "turning on a social guide changes the visible preview");
		check(on.pixelColor(at(output, 0.5, 0.5)) == off.pixelColor(at(output, 0.5, 0.5)),
		      "the safe content interior retains the original video pixels");
		check(on.pixelColor(at(output, 0.94, 0.5)).red() < off.pixelColor(at(output, 0.94, 0.5)).red(),
		      "the right-side interface region is visibly shaded");
		check(on.pixelColor(10, 320) == off.pixelColor(10, 320) && on.pixelColor(790, 320) == off.pixelColor(790, 320),
		      "the guide never shades letterbox bars outside the logical canvas");
		check(canvas.currentFrame() == frame && canvas.currentFrame().cacheKey() == originalKey,
		      "preview guides leave clean frames and their cache identity unchanged");
	}
	canvas.setSocialSafeArea(SocialSafeArea::Off);
	check(painted(canvas) == off, "switching Off restores the exact original preview");
	canvas.setSocialSafeArea(static_cast<SocialSafeArea>(99));
	check(canvas.socialSafeArea() == SocialSafeArea::Off && painted(canvas) == off,
	      "an invalid remembered guide falls back to Off");

	canvas.setVideoSize(1920, 1080);
	QImage landscape(640, 360, QImage::Format_RGBA8888);
	landscape.fill(QColor(150, 170, 190));
	canvas.setFrame(landscape);
	const QImage landscapeOff = painted(canvas);
	canvas.setSocialSafeArea(SocialSafeArea::TikTok);
	const QImage landscapeOn = painted(canvas);
	// 16:9 in800x640 is800x450 with95-pixel bars above and below.
	check(landscapeOn.pixelColor(400, 15) == landscapeOff.pixelColor(400, 15) &&
	      landscapeOn.pixelColor(400, 625) == landscapeOff.pixelColor(400, 625),
	      "landscape guides respect top and bottom letterbox bars too");
	check(landscapeOn.pixelColor(760, 320).red() < landscapeOff.pixelColor(760, 320).red(),
	      "guides follow the current canvas after its aspect ratio changes");

	int moves = 0, finished = 0;
	QObject::connect(&canvas, &PreviewCanvas::transformDragged, [&](double, double) { ++moves; });
	QObject::connect(&canvas, &PreviewCanvas::transformEditFinished, [&]() { ++finished; });
	canvas.setTransformMode(true);
	canvas.setTransformBox(QRectF(0, 0, 1920, 1080), 0);
	const QPointF from(740, 320), to(720, 330); // inside the shaded right margin
	QMouseEvent press(QEvent::MouseButtonPress, from, canvas.mapToGlobal(from), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent move(QEvent::MouseMove, to, canvas.mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent release(QEvent::MouseButtonRelease, to, canvas.mapToGlobal(to), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(&canvas, &press);
	QApplication::sendEvent(&canvas, &move);
	QApplication::sendEvent(&canvas, &release);
	check(moves > 0 && finished == 1, "the shaded guide does not intercept clip dragging or its undo boundary");
	check(canvas.currentFrame() == landscape, "editing under a guide still keeps the underlying frame clean");

	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
