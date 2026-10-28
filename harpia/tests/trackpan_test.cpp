// Middle-button navigation on the two Multi-Cut timelines changes the view,
// never the cuts, selection, playhead or source preview. Both lanes keep their
// own zoom, while each grab uses a fixed press-to-time mapping.
#include <QtWidgets>
#include <QSignalSpy>
#include <algorithm>
#include <cmath>
#define private public
#include "editor/TrackEditor.hpp"
#undef private

#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}
void press(QWidget &widget, QPoint at, Qt::MouseButton button, Qt::MouseButtons held = Qt::NoButton)
{
	QMouseEvent event(QEvent::MouseButtonPress, QPointF(at), widget.mapToGlobal(at), button,
			 held | button, Qt::NoModifier);
	QApplication::sendEvent(&widget, &event);
}
void move(QWidget &widget, QPoint at, Qt::MouseButtons held)
{
	QMouseEvent event(QEvent::MouseMove, QPointF(at), widget.mapToGlobal(at), Qt::NoButton,
			 held, Qt::NoModifier);
	QApplication::sendEvent(&widget, &event);
}
void release(QWidget &widget, QPoint at, Qt::MouseButton button, Qt::MouseButtons held = Qt::NoButton)
{
	QMouseEvent event(QEvent::MouseButtonRelease, QPointF(at), widget.mapToGlobal(at), button,
			 held, Qt::NoModifier);
	QApplication::sendEvent(&widget, &event);
}
void setup(TrackEditor &track)
{
	track.resize(800, 220);
	track.setDuration(60000);
	track.setActiveSource(7);
	track.setSegments({{0, 10000, 1.0, 1}, {2000, 12000, 1.0, 2}, {0, 10000, 1.0, 3}});
	track.selectForTest({1});
	track.setPlayhead(5000);
	track.zoom_ = track.zoomTarget_ = 4.0;
	track.outZoom_ = track.outZoomTarget_ = 4.0;
	track.viewStart_ = track.viewTarget_ = 15000;
	track.outViewStart_ = track.outViewTarget_ = 8000;
	track.show();
	QApplication::processEvents();
}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	TrackEditor track;
	setup(track);
	const QVector<CutSegment> cuts = track.segments();
	const QList<int> selection = track.selectedIndices();
	QSignalSpy changed(&track, &TrackEditor::segmentsChanged);
	QSignalSpy selected(&track, &TrackEditor::selectionChanged);
	QSignalSpy scrubbed(&track, &TrackEditor::scrubSource);
	QSignalSpy hovered(&track, &TrackEditor::hoverScrub);
	QSignalSpy hoverEnded(&track, &TrackEditor::hoverScrubEnded);
	QSignalSpy views(&track, &TrackEditor::viewChanged);
	const QPoint source(600, track.sourceRect().center().y());
	const QPoint output(400, track.outputRect().center().y());

	// Start in a caption, not just an empty patch of the source filmstrip.
	const QPoint sourceCaption(source.x(), track.sourceRect().top() - track.lp_.captionH / 2);
	press(track, sourceCaption, Qt::MiddleButton);
	check(track.cursor().shape() == Qt::ClosedHandCursor, "a middle grab exposes the closed-hand cursor");
	move(track, sourceCaption - QPoint(200, 0), Qt::MiddleButton);
	check(track.viewStart_ == 18750 && track.outViewStart_ == 8000,
	      "dragging Source left pans forward by the exact pixel-to-time distance without moving Output");
	check(!views.isEmpty() && views.last()[0].toLongLong() == 60000 &&
	      views.last()[1].toLongLong() == track.viewStart_ && views.last()[2].toLongLong() == 15000,
	      "the overview receives the Source lane's actual visible window");
	release(track, sourceCaption - QPoint(200, 0), Qt::MiddleButton);
	check(track.cursor().shape() != Qt::ClosedHandCursor, "releasing the middle button clears the grab cursor");
	press(track, source, Qt::MiddleButton);
	move(track, source + QPoint(200, 0), Qt::MiddleButton);
	release(track, source + QPoint(200, 0), Qt::MiddleButton);
	check(track.viewStart_ == 15000, "an equal drag right returns Source to the original view");

	const qint64 outputBefore = track.outViewStart_;
	press(track, output, Qt::MiddleButton);
	move(track, output - QPoint(100, 0), Qt::MiddleButton);
	release(track, output - QPoint(100, 0), Qt::MiddleButton);
	check(track.outViewStart_ == outputBefore + 938 && track.viewStart_ == 15000,
	      "grabbing an Output clip pans only Output using its own zoom");
	check(views.last()[0].toLongLong() == 30000 && views.last()[1].toLongLong() == track.outViewStart_ &&
	      views.last()[3].toLongLong() == 5000,
	      "the overview switches to Output without moving its playhead");
	const QPoint outputCaption(output.x(), track.outputRect().top() - track.lp_.captionH / 2);
	press(track, outputCaption, Qt::MiddleButton);
	move(track, outputCaption + QPoint(100, 0), Qt::MiddleButton);
	release(track, outputCaption + QPoint(100, 0), Qt::MiddleButton);
	check(std::abs(track.outViewStart_ - outputBefore) <= 1, "the Output caption can also be grabbed to pan back");

	// Hundreds of fractional-pixel-time steps must accumulate from the press,
	// rather than being rounded to zero individually at very high zoom.
	track.zoom_ = track.zoomTarget_ = 400.0;
	const qint64 preciseBefore = track.viewStart_;
	press(track, source, Qt::MiddleButton);
	for (int x = 599; x >= 300; --x)
		move(track, QPoint(x, source.y()), Qt::MiddleButton);
	release(track, QPoint(300, source.y()), Qt::MiddleButton);
	check(track.viewStart_ - preciseBefore == 56, "many one-pixel moves retain their full sub-millisecond pan distance");
	track.zoom_ = track.zoomTarget_ = 4.0;
	press(track, source, Qt::MiddleButton);
	move(track, QPoint(100000, source.y()), Qt::MiddleButton);
	check(track.viewStart_ == 0, "Source panning clamps at the start of the video");
	move(track, QPoint(-100000, source.y()), Qt::MiddleButton);
	release(track, source, Qt::MiddleButton);
	check(track.viewStart_ == 45000, "Source panning clamps at its final visible window");
	press(track, output, Qt::MiddleButton);
	move(track, QPoint(-100000, output.y()), Qt::MiddleButton);
	check(track.outViewStart_ == 22500, "Output panning also clamps at its final visible window");
	move(track, QPoint(100000, output.y()), Qt::MiddleButton);
	release(track, output, Qt::MiddleButton);
	check(track.outViewStart_ == 0, "Output panning cannot go before zero");

	track.viewStart_ = track.viewTarget_ = 15000;
	track.zoomTarget_ = 8.0;
	track.outZoomTarget_ = 8.0;
	track.viewTarget_ = 30000;
	track.outViewTarget_ = 12000;
	track.scrollAnim_->start();
	press(track, source, Qt::MiddleButton);
	track.animateScrollStep(); // queued wheel easing must not fight the grab
	check(!track.scrollAnim_->isActive() && track.zoomTarget_ == track.zoom_ &&
	      track.outZoomTarget_ == track.outZoom_ && track.viewTarget_ == track.viewStart_ &&
	      track.outViewTarget_ == track.outViewStart_, "a middle grab cancels unfinished easing in both lanes");
	QWheelEvent wheel(QPointF(source), track.mapToGlobal(source), QPoint(), QPoint(0, 120),
		Qt::MiddleButton, Qt::NoModifier, Qt::NoScrollPhase, false);
	QApplication::sendEvent(&track, &wheel);
	check(track.zoom_ == 4.0 && track.zoomTarget_ == 4.0 && !track.scrollAnim_->isActive(),
	      "wheel events cannot change the time scale halfway through a middle drag");
	press(track, source, Qt::LeftButton, Qt::MiddleButton);
	release(track, source, Qt::LeftButton, Qt::MiddleButton);
	press(track, source, Qt::RightButton, Qt::MiddleButton);
	release(track, source, Qt::RightButton, Qt::MiddleButton);
	check(track.mode_ == TrackEditor::Mode::Panning, "another button's press or release cannot turn a pan into an edit");
	move(track, source - QPoint(40, 0), Qt::MiddleButton);
	const qint64 beforeMissingRelease = track.viewStart_;
	move(track, source - QPoint(60, 0), Qt::NoButton);
	check(track.mode_ == TrackEditor::Mode::None && track.cursor().shape() != Qt::ClosedHandCursor &&
	      track.viewStart_ == beforeMissingRelease, "a missing middle release is recovered without another pan or preview");

	// Preserve an explicit cursor exactly, including custom artwork/hotspot,
	// and recover even when the widget cannot receive the button release.
	QPixmap cursorPixmap(12, 12);
	cursorPixmap.fill(Qt::red);
	const QCursor originalCursor(cursorPixmap, 2, 3);
	track.setCursor(originalCursor);
	press(track, source, Qt::MiddleButton);
	release(track, source, Qt::MiddleButton);
	check(track.testAttribute(Qt::WA_SetCursor) && track.cursor().hotSpot() == QPoint(2, 3) &&
	      track.cursor().pixmap().toImage() == cursorPixmap.toImage(),
	      "releasing a pan restores the original explicit cursor and its hotspot");
	for (QEvent::Type type : {QEvent::UngrabMouse, QEvent::Hide, QEvent::WindowDeactivate}) {
		track.setCursor(Qt::SizeHorCursor);
		press(track, source, Qt::MiddleButton);
		const qint64 beforeCancel = track.viewStart_;
		QEvent cancelled(type);
		QApplication::sendEvent(&track, &cancelled);
		const bool cursorRestored = track.testAttribute(Qt::WA_SetCursor) &&
			track.cursor().shape() == Qt::SizeHorCursor;
		move(track, source - QPoint(100, 0), Qt::MiddleButton);
		check(track.mode_ == TrackEditor::Mode::None && cursorRestored && track.viewStart_ == beforeCancel,
		      type == QEvent::UngrabMouse ? "losing the mouse grab cancels panning and restores the cursor" :
		      type == QEvent::Hide ? "hiding the timeline cancels panning and restores the cursor" :
		      "deactivating the window cancels panning and restores the cursor");
	}
	track.unsetCursor();
	check(track.segments() == cuts && track.selectedIndices() == selection && track.playhead() == 5000 &&
	      changed.isEmpty() && selected.isEmpty() && scrubbed.isEmpty() && hovered.isEmpty() && hoverEnded.isEmpty(),
	      "navigation preserves every cut, selection and playhead and emits no edit or preview requests");

	track.zoom_ = track.zoomTarget_ = 1.0;
	track.viewStart_ = track.viewTarget_ = 0;
	press(track, source, Qt::MiddleButton);
	move(track, source - QPoint(200, 0), Qt::MiddleButton);
	release(track, source, Qt::MiddleButton);
	check(track.viewStart_ == 0, "a fully fitted video stays put because the entire source is already visible");
	press(track, QPoint(100, source.y()), Qt::LeftButton);
	move(track, QPoint(300, source.y()), Qt::LeftButton);
	release(track, QPoint(300, source.y()), Qt::LeftButton);
	check(track.segments().size() == cuts.size() + 1 && track.segments().last().sourceId == 7 &&
	      changed.size() == 1 && !scrubbed.isEmpty(), "ordinary left dragging still creates a source cut after panning");

	{
		QWidget parent;
		parent.setCursor(Qt::CrossCursor);
		TrackEditor child(&parent);
		setup(child);
		child.unsetCursor();
		const QPoint childSource(600, child.sourceRect().center().y());
		press(child, childSource, Qt::MiddleButton);
		parent.setCursor(Qt::PointingHandCursor);
		release(child, childSource, Qt::MiddleButton);
		check(!child.testAttribute(Qt::WA_SetCursor) && child.cursor().shape() == Qt::PointingHandCursor,
		      "finishing a pan restores cursor inheritance rather than forcing an explicit cursor");
	}

	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
