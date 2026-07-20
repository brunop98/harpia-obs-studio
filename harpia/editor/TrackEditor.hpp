#pragma once

#include <QImage>
#include <QSet>
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
// sequence). Output segments can be clicked to select, dragged to reorder,
// trimmed by dragging their left/right edge (with live frame preview of the
// edge position), and removed with Delete or the right-click menu. Cuts are
// intentionally allowed to overlap/repeat source material.
class TrackEditor : public QWidget {
	Q_OBJECT
public:
	explicit TrackEditor(QWidget *parent = nullptr);

	void setDuration(qint64 ms);

	// Filmstrip thumbnails for the source track; entry i covers the time slice
	// [i, i+1) * duration/count.
	void setThumbs(const QVector<QImage> &thumbs);

	const QVector<CutSegment> &segments() const { return segs_; }
	int selectedIndex() const { return selected_; } // primary (last clicked), -1 = none
	// All selected cuts, ascending. Ctrl+click toggles membership, Shift+click
	// selects a range; the speed slider applies to every selected cut.
	QList<int> selectedIndices() const;
	void setSegmentSpeed(int index, double speed); // repaints; no segmentsChanged
	void removeSegment(int index);
	void removeSelected();

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
	void scrubSource(qint64 ms);      // preview the source frame while interacting
	void hoverScrub(qint64 ms);       // preview while merely hovering (no click)

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void wheelEvent(QWheelEvent *) override;
	void keyPressEvent(QKeyEvent *) override;
	void leaveEvent(QEvent *) override;
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

private:
	QRect sourceRect() const;
	QRect outputRect() const;
	int msToX(qint64 ms) const;
	qint64 xToMs(int x) const;
	qint64 visibleMs() const; // source-track window: duration / zoom
	void clampView();
	QVector<QRect> segmentRects() const;
	int segmentAt(const QPoint &p) const; // -1 = none
	int insertSlotAt(int x) const;        // 0..count reorder slot for a drop at x
	void showSegmentMenu(int index, const QPoint &globalPos);

	QVector<CutSegment> segs_;
	qint64 duration_ = 0;
	int selected_ = -1;      // primary selection (drives resize + the slider value)
	QSet<int> multiSel_;     // full selection; selected_ is a member when >= 0
	qint64 playheadOutMs_ = -1;

	// Source-track zoom (Ctrl+scroll; plain scroll pans) + filmstrip.
	double zoom_ = 1.0;
	qint64 viewStart_ = 0;
	QVector<QImage> thumbs_;
	qint64 hoverMs_ = -1; // hover position marker on the source track

	enum class Mode { None, CreatingCut, DraggingSegment, ResizingLeft, ResizingRight };
	Mode mode_ = Mode::None;
	qint64 dragStartMs_ = 0; // CreatingCut anchor (source ms)
	qint64 dragCurMs_ = 0;
	QPoint pressPos_;
	bool dragMoved_ = false;
	int dragInsertSlot_ = -1; // reorder target slot while dragging

	// Edge-trim drag state, captured at press so the px→source-ms scale stays
	// stable while the segment's width changes under the cursor.
	qint64 resizeOrigStart_ = 0;
	qint64 resizeOrigEnd_ = 0;
	double resizeSrcPerPx_ = 0.0;
};

} // namespace harpia
