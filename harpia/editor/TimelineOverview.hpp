#pragma once

// "Where am I?" for a zoomed-in timeline.
//
// Zooming into a long project is the only way to work precisely, and it is also
// how you lose the thread: the lane in front of you could be anywhere in the
// video and nothing on screen says which part. This is the answer that costs
// nothing to read -- the whole project as one bar, with the slice you are
// looking at drawn as a red rectangle inside it.
//
// Deliberately narrow:
//   * Only while you are working the timeline. It appears on a zoom, a scroll
//     or a pan and fades out shortly after you stop, so it costs no permanent
//     layout space and is not there to be read when it has nothing to say.
//     Hidden, it takes no mouse events at all, so the preview underneath keeps
//     every click except during the seconds this is actually on screen.
//   * While it IS on screen you can drag the red box to move the view, or click
//     anywhere in the bar to jump there. It stays up as long as the pointer is
//     over it, so it cannot fade out from under the hand reaching for it.
//   * Plain. The duration, the viewport, the playhead. No clips, no thumbnails:
//     one glance, one fact.
//
// The mapping functions are static and take a rect, so the arithmetic that
// decides where the red box lands -- and where a drag takes it -- can be tested
// without a window.

#include <QColor>
#include <QRect>
#include <QWidget>

class QVariantAnimation;
class QTimer;

namespace harpia {

class TimelineOverview : public QWidget {
	Q_OBJECT
public:
	explicit TimelineOverview(QWidget *parent = nullptr);
	~TimelineOverview() override;

	// Show (or refresh) the overview and restart the idle countdown.
	// `visibleMs` is how much of `totalMs` the timeline currently shows;
	// `playheadMs` < 0 draws no playhead.
	void showFor(qint64 totalMs, qint64 startMs, qint64 visibleMs, qint64 playheadMs);
	// Straight to invisible, no fade -- for leaving the mode or closing.
	void hideNow();

	// Where the viewport rectangle goes inside `bar`. Empty when there is
	// nothing to show. Clamped to the bar, and never thinner than a few pixels:
	// at 60x zoom on an hour-long project the true width rounds to zero, and a
	// marker you cannot see is worse than a slightly generous one.
	static QRect viewportRect(qint64 totalMs, qint64 startMs, qint64 visibleMs, const QRect &bar);
	// Where a moment in the project sits across `bar`.
	static int xForMs(qint64 totalMs, qint64 ms, const QRect &bar);
	// The inverse: which moment a pixel is.
	static qint64 msForX(qint64 totalMs, int x, const QRect &bar);
	// The view start that puts the viewport under the pointer, given where
	// inside the box it was grabbed (0 = its left edge, 0.5 = its middle).
	// Clamped so a drag can never scroll past either end.
	static qint64 startForDrag(qint64 totalMs, qint64 visibleMs, double grabFrac, int x,
				   const QRect &bar);

	// The bar the two functions above map into: this widget's rect, inset.
	QRect barRect() const;

	// How long after the last movement it starts to fade.
	static constexpr int kIdleHideMs = 1500;
	static constexpr int kFadeMs = 220;
	// A sensible fixed height for the strip.
	static constexpr int kHeight = 26;

	// Test hooks. The fade is time-based and a test should not have to sleep
	// through it to check the geometry.
	double opacityForTest() const { return opacity_; }
	void finishFadeForTest();
	bool draggingForTest() const { return dragging_; }

signals:
	// A drag or a click in the bar: move the visible window to start here.
	void viewStartRequested(qint64 startMs);

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void enterEvent(QEnterEvent *) override;
	void leaveEvent(QEvent *) override;

private:
	void startFade(double to);

	qint64 totalMs_ = 0;
	qint64 startMs_ = 0;
	qint64 visibleMs_ = 0;
	qint64 playheadMs_ = -1;

	// Drag state. grabFrac_ is where in the box the press landed, so the box
	// keeps its grip on the pointer instead of jumping its centre there.
	bool dragging_ = false;
	double grabFrac_ = 0.5;

	double opacity_ = 0.0;
	QVariantAnimation *fade_ = nullptr;
	QTimer *idle_ = nullptr;
};

} // namespace harpia
