// A view gesture must never reposition or trim a narration take. Exercise the
// actual widget with middle-button moves, including release and lost-grab paths.
#include <QtWidgets>

#define private public
#include "editor/VoiceoverTrack.hpp"
#undef private

#include <cmath>
#include <cstdio>
#include <limits>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *message)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}

void press(QWidget &widget, QPoint point, Qt::MouseButton button,
	   Qt::MouseButtons buttons = Qt::NoButton)
{
	QMouseEvent event(QEvent::MouseButtonPress, QPointF(point), widget.mapToGlobal(point), button,
		buttons == Qt::NoButton ? Qt::MouseButtons(button) : buttons, Qt::NoModifier);
	QApplication::sendEvent(&widget, &event);
}

void move(QWidget &widget, QPoint point, Qt::MouseButtons buttons)
{
	QMouseEvent event(QEvent::MouseMove, QPointF(point), widget.mapToGlobal(point), Qt::NoButton,
		buttons, Qt::NoModifier);
	QApplication::sendEvent(&widget, &event);
}

void release(QWidget &widget, QPoint point, Qt::MouseButton button,
	     Qt::MouseButtons buttons = Qt::NoButton)
{
	QMouseEvent event(QEvent::MouseButtonRelease, QPointF(point), widget.mapToGlobal(point), button,
		buttons, Qt::NoModifier);
	QApplication::sendEvent(&widget, &event);
}

void wheel(QWidget &widget, QPoint point, QPoint delta, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
	QWheelEvent event(QPointF(point), widget.mapToGlobal(point), QPoint(), delta, Qt::NoButton,
		modifiers, Qt::NoScrollPhase, false);
	QApplication::sendEvent(&widget, &event);
}

QImage picture(VoiceoverTrack &track)
{
	QImage image(track.size(), QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	track.render(&image);
	return image;
}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	VoiceoverTrack track;
	track.resize(800, 90);
	track.setOutputDuration(60000);
	VoiceoverClip clip;
	clip.outStartMs = 20000;
	clip.durationMs = 10000;
	clip.srcTotalMs = 60000;
	clip.peaks = QVector<float>(64, 0.5f);
	track.setClips({clip});
	track.selected_ = 0;
	track.setPlayhead(25000);
	track.show();
	QApplication::processEvents();
	const auto original = track.clips();
	int edits = 0, selections = 0;
	QObject::connect(&track, &VoiceoverTrack::clipsChanged, [&]() { ++edits; });
	QObject::connect(&track, &VoiceoverTrack::clipSelected, [&](int) { ++selections; });

	press(track, QPoint(500, 45), Qt::MiddleButton);
	move(track, QPoint(300, 45), Qt::MiddleButton);
	release(track, QPoint(300, 45), Qt::MiddleButton);
	check(track.viewStart_ == 0 && track.clips() == original,
	      "a fitted narration view has no off-timeline pan range and keeps its clips");
	const qint64 anchor = track.xToMs(400);
	wheel(track, QPoint(400, 45), QPoint(0, 720));
	check(track.zoom_ > 1.0 && std::abs(track.xToMs(400) - anchor) <= 1,
	      "scroll zooms narration around the time under the pointer");

	track.zoom_ = 6.0;
	track.viewStart_ = 20000;
	track.clampView();
	track.setCursor(Qt::CrossCursor);
	const qint64 grabbed = track.xToMs(600);
	press(track, QPoint(600, 45), Qt::MiddleButton);
	check(track.cursor().shape() == Qt::ClosedHandCursor, "middle press shows the grabbed hand cursor");
	for (int x = 590; x >= 400; x -= 10)
		move(track, QPoint(x, 45), Qt::MiddleButton);
	check(track.viewStart_ > 20000 && std::abs(track.xToMs(400) - grabbed) <= 1,
	      "dragging left brings later narration into view and keeps the grabbed time under the pointer");
	release(track, QPoint(400, 45), Qt::MiddleButton);
	check(!track.panning_ && track.cursor().shape() == Qt::CrossCursor,
	      "middle release ends the gesture and restores the previous cursor");
	const qint64 stopped = track.viewStart_;
	move(track, QPoint(700, 45), Qt::NoButton);
	check(track.viewStart_ == stopped, "buttonless movement cannot continue a completed narration pan");
	press(track, QPoint(400, 45), Qt::MiddleButton);
	move(track, QPoint(600, 45), Qt::MiddleButton);
	release(track, QPoint(600, 45), Qt::MiddleButton);
	check(std::abs(track.viewStart_ - 20000) <= 1, "dragging the same distance right returns to the original view");
	check(track.clips() == original && track.selectedIndex() == 0 && track.playheadMs_ == 25000 &&
	      edits == 0 && selections == 0,
	      "middle panning changes no clip, selection or playhead and emits no edit signals");

	track.zoom_ = 400.0; // sub-millisecond pixels expose per-move rounding errors
	track.viewStart_ = 20000;
	const double msPerPixel = double(track.visibleMs()) / track.trackRect().width();
	press(track, QPoint(700, 45), Qt::MiddleButton);
	for (int x = 699; x >= 400; --x)
		move(track, QPoint(x, 45), Qt::MiddleButton);
	release(track, QPoint(400, 45), Qt::MiddleButton);
	check(track.viewStart_ == 20000 + qint64(std::llround(300.0 * msPerPixel)),
	      "many tiny moves accumulate from the press without losing fractional milliseconds");
	track.zoom_ = 6.0;
	track.viewStart_ = 20000;
	press(track, QPoint(500, 45), Qt::MiddleButton);
	const double heldZoom = track.zoom_;
	wheel(track, QPoint(500, 45), QPoint(0, 480));
	check(track.zoom_ == heldZoom && track.viewStart_ == 20000,
	      "wheel input cannot change the mapping during a middle drag");
	press(track, QPoint(500, 45), Qt::LeftButton, Qt::LeftButton | Qt::MiddleButton);
	release(track, QPoint(500, 45), Qt::LeftButton, Qt::MiddleButton);
	move(track, QPoint(450, 45), Qt::MiddleButton);
	check(track.panning_ && track.viewStart_ > 20000 && track.clips() == original,
	      "extra buttons neither edit a clip nor prematurely end a middle drag");
	release(track, QPoint(450, 45), Qt::MiddleButton);
	press(track, QPoint(500, 45), Qt::MiddleButton);
	move(track, QPoint(400, 45), Qt::NoButton);
	check(!track.panning_, "a missing middle-button release is recovered on the next move");
	for (QEvent::Type type : {QEvent::UngrabMouse, QEvent::Hide, QEvent::WindowDeactivate}) {
		press(track, QPoint(500, 45), Qt::MiddleButton);
		QEvent event(type);
		QApplication::sendEvent(&track, &event);
		check(!track.panning_ && track.cursor().shape() != Qt::ClosedHandCursor,
		      "losing the grab, hiding or deactivating the track clears its pan state");
	}
	press(track, QPoint(500, 45), Qt::MiddleButton);
	move(track, QPoint(100000, 45), Qt::MiddleButton);
	release(track, QPoint(100000, 45), Qt::MiddleButton);
	check(track.viewStart_ == 0, "narration panning clamps at the start of the output");
	press(track, QPoint(500, 45), Qt::MiddleButton);
	move(track, QPoint(-100000, 45), Qt::MiddleButton);
	release(track, QPoint(-100000, 45), Qt::MiddleButton);
	check(track.viewStart_ == track.outputMs_ - track.visibleMs(), "narration panning clamps at the end of the output");
	check(track.clips() == original && edits == 0 && selections == 0,
	      "pan cancellation and clamping still leave the narration untouched");

	track.viewStart_ = 20000;
	const qint64 beforeWheel = track.viewStart_;
	wheel(track, QPoint(400, 45), QPoint(0, -120), Qt::ShiftModifier);
	check(track.viewStart_ > beforeWheel, "Shift-wheel also pans a zoomed narration view");
	track.viewStart_ = 20000;
	press(track, QPoint(400, 45), Qt::LeftButton);
	move(track, QPoint(500, 45), Qt::LeftButton);
	release(track, QPoint(500, 45), Qt::LeftButton);
	check(track.clips().first().outStartMs > original.first().outStartMs && edits == 1,
	      "the existing left-drag clip move still edits normally after panning");
	const auto moved = track.clips();
	const QRect clipRect = track.clipRects().first();
	press(track, QPoint(clipRect.left() + 1, clipRect.center().y()), Qt::LeftButton);
	move(track, QPoint(clipRect.left() + 31, clipRect.center().y()), Qt::LeftButton);
	release(track, QPoint(clipRect.left() + 31, clipRect.center().y()), Qt::LeftButton);
	check(track.clips().first().srcStartMs > moved.first().srcStartMs && edits == 2,
	      "left-dragging a trim edge still trims the source after panning");

	check(track.msToX(std::numeric_limits<qint64>::max()) > 0 &&
	      track.msToX(std::numeric_limits<qint64>::min()) < 0,
	      "far-offscreen narration times remain bounded drawable coordinates");
	track.setClips({});
	const QImage empty = picture(track);
	VoiceoverClip huge = clip;
	huge.outStartMs = -1000000000000LL;
	huge.durationMs = 2000000000000LL;
	track.setClips({huge});
	const QImage large = picture(track);
	bool captionUnchanged = true;
	for (int y = 0; y < track.trackRect().top(); ++y)
		for (int x = 0; x < track.width(); ++x)
			captionUnchanged = captionUnchanged && empty.pixel(x, y) == large.pixel(x, y);
	check(captionUnchanged, "zoomed offscreen clip geometry cannot paint over the narration caption");

	QWidget cursorParent;
	cursorParent.setCursor(Qt::WaitCursor);
	VoiceoverTrack cursorTrack(&cursorParent);
	cursorTrack.resize(800, 90);
	cursorTrack.setOutputDuration(60000);
	press(cursorTrack, QPoint(500, 45), Qt::MiddleButton);
	release(cursorTrack, QPoint(500, 45), Qt::MiddleButton);
	check(!cursorTrack.testAttribute(Qt::WA_SetCursor) && cursorTrack.cursor().shape() == Qt::WaitCursor,
	      "releasing a pan preserves an inherited cursor without creating a local override");
	cursorParent.setCursor(Qt::PointingHandCursor);
	check(cursorTrack.cursor().shape() == Qt::PointingHandCursor,
	      "the narration track continues to inherit later parent cursor changes");
	QPixmap cursorImage(12, 12);
	cursorImage.fill(Qt::green);
	const QCursor customCursor(cursorImage, 3, 4);
	cursorTrack.setCursor(customCursor);
	press(cursorTrack, QPoint(500, 45), Qt::MiddleButton);
	release(cursorTrack, QPoint(500, 45), Qt::MiddleButton);
	check(cursorTrack.testAttribute(Qt::WA_SetCursor) && cursorTrack.cursor().shape() == Qt::BitmapCursor &&
	      cursorTrack.cursor().hotSpot() == customCursor.hotSpot() &&
	      cursorTrack.cursor().pixmap().toImage() == customCursor.pixmap().toImage(),
	      "releasing a pan restores a custom cursor image and hotspot exactly");
	track.setOutputDuration(0);
	check(track.viewStart_ == 0 && track.zoom_ == 1.0, "an empty output resets the view without invalid bounds");
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
