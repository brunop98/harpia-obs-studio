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
//   * READ-ONLY. It never takes the mouse (WA_TransparentForMouseEvents), so it
//     can float over the preview without ever swallowing a click meant for the
//     picture underneath. It reports; it does not navigate.
//   * Only while you are working the timeline. It appears on a zoom, a scroll
//     or a pan and fades out shortly after you stop, so it costs no permanent
//     layout space and is not there to be read when it has nothing to say.
//   * Plain. The duration, the viewport, the playhead. No clips, no thumbnails:
//     one glance, one fact.
//
// The two mapping functions are static and take a rect, so the arithmetic that
// decides where the red box lands can be tested without a window.

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

protected:
	void paintEvent(QPaintEvent *) override;

private:
	void startFade(double to);

	qint64 totalMs_ = 0;
	qint64 startMs_ = 0;
	qint64 visibleMs_ = 0;
	qint64 playheadMs_ = -1;

	double opacity_ = 0.0;
	QVariantAnimation *fade_ = nullptr;
	QTimer *idle_ = nullptr;
};

} // namespace harpia
