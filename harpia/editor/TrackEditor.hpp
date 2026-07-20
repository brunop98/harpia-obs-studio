#pragma once

#include <QVector>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace harpia {

// One kept section of the source video, played at its own speed in the output.
struct CutSegment {
	qint64 srcStartMs = 0;
	qint64 srcEndMs = 0;
	double speed = 1.0;

	qint64 outDurationMs() const
	{
		const double sp = speed > 0.01 ? speed : 1.0;
		return std::max<qint64>(1, qint64(std::llround(double(srcEndMs - srcStartMs) / sp)));
	}
};

// The multi-cut track editor: a Source track (the whole video — press and drag
// to select a section to keep) above an Output track (the selected cuts in
// sequence). Output segments can be clicked to select, dragged to reorder, and
// removed with Delete or the right-click menu. Cuts are intentionally allowed
// to overlap/repeat source material.
class TrackEditor : public QWidget {
	Q_OBJECT
public:
	explicit TrackEditor(QWidget *parent = nullptr);

	void setDuration(qint64 ms);

	const QVector<CutSegment> &segments() const { return segs_; }
	int selectedIndex() const { return selected_; }
	void setSegmentSpeed(int index, double speed); // repaints; no segmentsChanged
	void removeSegment(int index);

	qint64 totalOutputMs() const;
	qint64 outputStartOf(int index) const; // output-time where segment #index begins
	// Map an output-time position to (segment index, source ms). Returns -1 when
	// there are no segments; outMs past the end clamps into the last segment.
	int sourceForOutput(qint64 outMs, qint64 *srcMs) const;

	void setPlayhead(qint64 outMs); // playhead on the output track (output-time)
	void clearPlayhead();

signals:
	void segmentsChanged();          // added / removed / reordered
	void selectionChanged(int index); // -1 = nothing selected
	void scrubSource(qint64 ms);      // preview the source frame under the mouse

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void keyPressEvent(QKeyEvent *) override;
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

private:
	QRect sourceRect() const;
	QRect outputRect() const;
	int msToX(qint64 ms) const;
	qint64 xToMs(int x) const;
	QVector<QRect> segmentRects() const;
	int segmentAt(const QPoint &p) const; // -1 = none
	int insertSlotAt(int x) const;        // 0..count reorder slot for a drop at x
	void showSegmentMenu(int index, const QPoint &globalPos);

	QVector<CutSegment> segs_;
	qint64 duration_ = 0;
	int selected_ = -1;
	qint64 playheadOutMs_ = -1;

	enum class Mode { None, CreatingCut, DraggingSegment };
	Mode mode_ = Mode::None;
	qint64 dragStartMs_ = 0; // CreatingCut anchor (source ms)
	qint64 dragCurMs_ = 0;
	QPoint pressPos_;
	bool dragMoved_ = false;
	int dragInsertSlot_ = -1; // reorder target slot while dragging
};

} // namespace harpia
