#pragma once

#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QSet>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <cmath>

class QPainter;
class QTimer;

namespace harpia {

// One kept section of the source video, played at its own speed in the output.
struct CutSegment {
	qint64 srcStartMs = 0;
	qint64 srcEndMs = 0;
	double speed = 1.0;
	int sourceId = 0; // which editor source this cut comes from (multi-source mix)

	qint64 outDurationMs() const
	{
		const double sp = speed > 0.01 ? speed : 1.0;
		return std::max<qint64>(1, qint64(std::llround(double(srcEndMs - srcStartMs) / sp)));
	}

	bool operator==(const CutSegment &o) const
	{
		return srcStartMs == o.srcStartMs && srcEndMs == o.srcEndMs && speed == o.speed &&
		       sourceId == o.sourceId;
	}
};

// Runtime-tweakable layout parameters for the multi-cut track editor (edited
// live from the editor's Developer Panel to find the best UI configuration).
struct TrackLayoutParams {
	int margin = 8;
	int captionH = 16;
	int srcH = 34; // source-track height (tall enough for the filmstrip)
	int trackGap = 8;
	int outH = 40;       // output-track height
	int segGap = 4;      // gap between output segments
	int minSegW = 48;    // "reasonably wide" — easy to click and drag
	int hardMinSegW = 24; // absolute floor when many segments compete
	int tileGap = 2;     // gap between filmstrip tiles
	int captionFontPx = 11; // track captions
	int segFontPx = 10;     // per-segment labels
	double maxZoom = 32.0;
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

	// Which editor source the Source track currently shows. New cuts created on
	// the Source track are stamped with this id (multi-source mixing). Repaints so
	// the used-region overlay always reflects the shown source.
	void setActiveSource(int id)
	{
		activeSourceId_ = id;
		update();
	}
	// The editor source id relevant to the most recent scrubSource()/hoverScrub()
	// emission (the active source for the Source track, the hovered/clicked
	// segment's source for the Output track). Read synchronously in the slot.
	int scrubSourceId() const { return lastScrubSourceId_; }

	// Filmstrip thumbnails for the source track; entry i covers the time slice
	// [i, i+1) * duration/count.
	void setThumbs(const QVector<QImage> &thumbs);

	// Per-source filmstrip so Output cuts render from their OWN source (a mix can
	// hold cuts from several videos). Fed for every source as its strip decodes.
	void setSourceThumbs(int sourceId, const QVector<QImage> &thumbs, qint64 durationMs);

	const QVector<CutSegment> &segments() const { return segs_; }
	// Replace the whole cut list (undo/redo restore). Clears selection; does NOT
	// emit segmentsChanged (the caller refreshes derived UI).
	void setSegments(const QVector<CutSegment> &segs);
	int selectedIndex() const { return selected_; } // primary (last clicked), -1 = none
	// All selected cuts, ascending. Ctrl+click toggles membership, Shift+click
	// selects a range; the speed slider applies to every selected cut.
	QList<int> selectedIndices() const;
	void setSegmentSpeed(int index, double speed); // repaints; no segmentsChanged
	void addSegments(const QVector<CutSegment> &segs); // append (auto-cut populate)
	void removeSegment(int index);
	void removeSelected();

	qint64 totalOutputMs() const;
	qint64 outputStartOf(int index) const; // output-time where segment #index begins
	// Map an output-time position to (segment index, source ms). Returns -1 when
	// there are no segments; outMs past the end clamps into the last segment.
	int sourceForOutput(qint64 outMs, qint64 *srcMs) const;

	void setPlayhead(qint64 outMs); // playhead on the output track (output-time)
	void clearPlayhead();
	qint64 playhead() const { return playheadOutMs_; } // -1 when none

	// Developer Panel: tweak the layout live (invalidates the strip cache).
	const TrackLayoutParams &layoutParams() const { return lp_; }
	void setLayoutParams(const TrackLayoutParams &p);

signals:
	void segmentsChanged();          // added / removed / reordered
	void selectionChanged(int index); // -1 = nothing selected
	void scrubSource(qint64 ms);      // preview the source frame while interacting
	void hoverScrub(qint64 ms);       // preview while merely hovering (no click)
	void autoCutRequested();          // "Auto-cut on scene changes" from the source menu

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
	// Output track has its own zoom/pan over OUTPUT-time (0..totalOutputMs).
	qint64 outVisibleMs() const;
	int outMsToX(qint64 ms) const;
	qint64 outXToMs(int x) const;
	void clampOutView();
	// (Re)render the source bar background + filmstrip into stripCache_ when
	// the view changed — repaints then blit it instead of rescaling tiles.
	void ensureStripCache(const QRect &src);
	// Tick marks + time labels along a track (source or output), spacing adapts
	// to the zoom level. Maps [viewStart, viewStart+visible] across `bar`.
	void drawTimeRuler(QPainter &p, const QRect &bar, qint64 viewStart, qint64 visible) const;
	QVector<QRect> segmentRects() const;
	int segmentAt(const QPoint &p) const; // -1 = none
	int insertSlotAt(int x) const;        // 0..count reorder slot for a drop at x
	void showSegmentMenu(int index, const QPoint &globalPos, const QPoint &localPos);
	// Split a cut into two at a source-time position (the click maps to it).
	void splitSegment(int index, qint64 splitSrcMs);

	// Emit a scrub/hover preview, remembering which source the ms belongs to so
	// the window can decode from the right file (see scrubSourceId()).
	void emitScrub(qint64 ms, int srcId)
	{
		lastScrubSourceId_ = srcId;
		emit scrubSource(ms);
	}
	void emitHover(qint64 ms, int srcId)
	{
		lastScrubSourceId_ = srcId;
		emit hoverScrub(ms);
	}

	TrackLayoutParams lp_;
	QVector<CutSegment> segs_;
	qint64 duration_ = 0;
	int activeSourceId_ = 0;    // stamped onto new cuts from the Source track
	int lastScrubSourceId_ = 0; // source of the most recent scrub/hover emit
	// Per-source filmstrips for rendering Output cuts (keyed by source id).
	QHash<int, QVector<QImage>> srcThumbs_;
	QHash<int, qint64> srcThumbDur_;
	int selected_ = -1;      // primary selection (drives resize + the slider value)
	QSet<int> multiSel_;     // full selection; selected_ is a member when >= 0
	qint64 playheadOutMs_ = -1;

	// Source-track zoom (Ctrl+scroll; plain scroll pans) + filmstrip.
	double zoom_ = 1.0;
	qint64 viewStart_ = 0;
	double outZoom_ = 1.0;    // output-track zoom (1 = whole output visible)
	qint64 outViewStart_ = 0; // first visible output-ms
	// Smooth-scroll: panning sets a target and a timer eases the view toward it,
	// so scrolling glides instead of jumping (easier to follow the position).
	qint64 viewTarget_ = 0;
	qint64 outViewTarget_ = 0;
	QTimer *scrollAnim_ = nullptr;
	void animateScrollStep(); // ease viewStart_/outViewStart_ toward the targets
	QVector<QImage> thumbs_;

	// Filmstrip render cache (keyed on size/zoom/view/thumbs revision/dpr).
	QPixmap stripCache_;
	QSize stripCacheSize_;
	double stripCacheZoom_ = -1.0;
	qint64 stripCacheView_ = -1;
	int thumbsRev_ = 0;
	int stripCacheRev_ = -1;
	qreal stripCacheDpr_ = 0.0;
	qint64 hoverMs_ = -1;  // hover position marker on the source track
	int hoverOutSeg_ = -1; // hovered output segment (marker there too)
	int hoverOutX_ = -1;   // marker x within that segment

	enum class Mode { None, CreatingCut, DraggingSegment, ResizingLeft, ResizingRight };
	Mode mode_ = Mode::None;
	qint64 dragStartMs_ = 0; // CreatingCut anchor (source ms)
	qint64 dragCurMs_ = 0;
	QPoint pressPos_;
	bool dragMoved_ = false;
	int dragInsertSlot_ = -1; // reorder target slot while dragging
	int dragGhostX_ = -1;     // mouse x while reordering (drives the drag ghost)

	// Edge-trim drag state, captured at press so the px→source-ms scale stays
	// stable while the segment's width changes under the cursor.
	qint64 resizeOrigStart_ = 0;
	qint64 resizeOrigEnd_ = 0;
	double resizeSrcPerPx_ = 0.0;
	// For Shift+trim (re-time): the cut's output duration at press and the
	// output-ms-per-pixel scale, so dragging the edge changes speed to fit the
	// same source content into the new width.
	qint64 resizeOrigOutDur_ = 0;
	double resizeOutPerPx_ = 0.0;
};

} // namespace harpia
