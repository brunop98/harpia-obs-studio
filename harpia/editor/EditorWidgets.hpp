#pragma once

#include "timeline/Spotlight.hpp"

#include <QImage>
#include <QPointF>
#include <QPixmap>
#include <QRect>
#include <QVector>
#include <QWidget>

namespace harpia {

// Runtime-tweakable minimum size of the video preview canvas (shared by both
// editor modes), edited live from the Developer Panel.
struct PreviewLayoutParams {
	int minW = 480;
	int minH = 270;
};

// Shows the current video frame scaled-to-fit, and (when crop is enabled) an
// interactive crop rectangle with 8 resize handles that maps to source-video
// pixels. Emits cropChanged() in source pixels.
class PreviewCanvas : public QWidget {
	Q_OBJECT
public:
	explicit PreviewCanvas(QWidget *parent = nullptr);

	void setVideoSize(int w, int h); // source dimensions (px)
	void setFrame(const QImage &img);
	// What is on screen right now (empty before the first frame).
	const QImage &currentFrame() const { return frame_; }

	void setCropEnabled(bool on);
	bool cropEnabled() const { return cropEnabled_; }
	QRect cropRectVideo() const { return cropVideo_; }
	void setCropRectVideo(const QRect &r); // undo/redo restore; no signal
	void resetCrop(); // full frame

	// ---- Direct clip manipulation (Full editing) ------------------------
	// When armed, dragging moves the selected timeline clip and the wheel zooms
	// it about the cursor. Deltas are emitted normalised to the output canvas, so
	// the window can apply them to the clip's pose (or a keyframe) directly.
	void setTransformMode(bool on);
	bool transformMode() const { return transformMode_; }
	// Outline of the clip being manipulated, in output-canvas pixels (empty
	// hides it), plus how far it is turned. The rect is the UNROTATED box and
	// the angle is applied about its centre -- the same decomposition the
	// compositor uses -- so the grips sit on the corners of the shape as drawn
	// rather than on a bounding box that is wrong the moment anything is turned.
	void setTransformBox(const QRectF &canvasRect, double rotationDeg);

	// Developer Panel: tweak the preview's minimum size live.
	const PreviewLayoutParams &layoutParams() const { return lp_; }
	void setLayoutParams(const PreviewLayoutParams &p);

	// ---- Spotlight masks (Inverse Selection) ----------------------------
	// The masks are handed over whole rather than by reference: the canvas draws
	// and drags them, the window owns them. Poses are already resolved for the
	// playhead, so a keyframed mask's handles sit where the mask actually is.
	struct SpotDraw {
		SpotShape shape = SpotShape::RoundRect;
		SpotPose pose;
		bool enabled = true;
	};
	void setSpotlightMode(bool on);
	bool spotlightMode() const { return spotMode_; }
	void setSpotlightMasks(const QVector<SpotDraw> &masks, int selected);

	// The transform grips in widget coordinates, for tests: pressing exactly on
	// one is the whole point, and a test that computed the geometry itself would
	// be checking its own arithmetic rather than the widget's.
	QVector<QPointF> handlePointsForTest() const { return transformHandlePoints(); }

signals:
	void cropChanged(const QRect &videoRect);
	// Incremental move, as a fraction of the canvas (add to posX/posY).
	void transformDragged(double dxNorm, double dyNorm);
	// Zoom by `factor` while holding the point under the cursor fixed; the
	// cursor is given in normalised canvas coordinates.
	void transformZoomed(double factor, double cursorXNorm, double cursorYNorm);
	// A grip was dragged. Absolute rather than incremental: a resize moves the
	// centre as well as the size, and sending those as two deltas invites them
	// to be applied against different poses. `scaleFactor` multiplies whatever
	// the clip's scale is; `rotationDeg` replaces its rotation.
	void transformScaled(double scaleFactor, double posXNorm, double posYNorm);
	void transformRotated(double rotationDeg);
	// The gesture ended -- one undo step per drag, not one per pixel. Matches
	// what spotlightEditFinished does for masks.
	void transformEditFinished();
	// A mask was clicked (-1 = the click missed every mask).
	void spotlightSelected(int index);
	// Live, once per mouse-move: the mask's new pose in normalised canvas terms.
	void spotlightPoseChanged(int index, const SpotPose &pose);
	// The drag ended — one undo step per gesture, not one per pixel.
	void spotlightEditFinished();

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void wheelEvent(QWheelEvent *) override;

private:
	enum class Zone { None, Move, L, R, T, B, TL, TR, BL, BR };
	QRect displayRect() const;                  // where the frame is painted
	QRect videoToWidget(const QRect &r) const;  // source px -> widget px
	QRect widgetCropRect() const;               // crop in widget px
	Zone zoneAt(const QPoint &p) const;
	void applyWidgetCrop(const QRect &widgetRect); // clamp + map back to video px

	PreviewLayoutParams lp_;
	QImage frame_;
	int vw_ = 0, vh_ = 0;
	bool cropEnabled_ = false;
	QRect cropVideo_; // source pixels

	Zone drag_ = Zone::None;
	QPoint dragStart_;
	QRect dragStartCrop_; // widget px at press

	// Full-editing clip manipulation.
	//
	// The grips are the same eight-plus-rotate arrangement the spotlight masks
	// use, and for the same reason: everything is computed in the clip's OWN
	// unrotated space and then turned, so dragging a corner of a tilted clip
	// resizes along its axes rather than the screen's.
	//
	// Only uniform scale, because that is all a clip's pose has. Every grip
	// therefore does the same kind of thing -- an edge grip scales by how far it
	// is dragged along its own axis -- rather than four of them silently doing
	// nothing.
	enum class XfZone { None, Move, TL, T, TR, L, R, BL, B, BR, Rotate };
	QVector<QPointF> transformHandlePoints() const; // widget px, already turned
	XfZone transformZoneAt(const QPoint &p) const;
	QRectF transformWidgetRect() const; // the unrotated box, in widget px

	bool transformMode_ = false;
	bool transformDragging_ = false;
	QPoint transformLast_;
	QRectF transformRect_;        // canvas px, UNROTATED; empty = no outline
	double transformRotation_ = 0.0;
	XfZone xfDrag_ = XfZone::None;
	QPointF xfPressLocal_;    // press point in the clip's own axes
	QRectF xfPressRect_;      // the widget-space box when the drag started
	double xfPressRotation_ = 0.0;
	double xfPressAngle_ = 0.0; // pointer angle at press, for the rotate grip

	// ---- Spotlight editing ----------------------------------------------
	// A mask's own frame: everything is computed in UNROTATED mask space and
	// then turned by the pose's rotation, so a corner drag on a turned mask
	// resizes along the mask's axes rather than the screen's.
	enum class SpotZone { None, Move, L, R, T, B, TL, TR, BL, BR, Rotate };
	QPointF canvasToWidgetF(double nx, double ny) const;
	QPointF widgetToCanvasF(const QPointF &p) const; // normalised, unclamped
	QPointF maskLocal(int i, const QPointF &widgetPt) const; // widget -> mask axes
	QVector<QPointF> spotHandlePoints(int i) const;          // 8 grips + rotate
	SpotZone spotZoneAt(const QPoint &p, int *maskOut) const;
	void drawSpotlight(QPainter &p) const;
	// Snap an edge or a centre to the canvas's own landmarks (edges, middle,
	// thirds). Returns the value unchanged when nothing is within reach.
	double snapNorm(double v, bool horizontal) const;

	bool spotMode_ = false;
	QVector<SpotDraw> spotMasks_;
	int spotSel_ = -1;
	SpotZone spotDrag_ = SpotZone::None;
	int spotDragMask_ = -1;
	SpotPose spotStartPose_;   // pose at mouse-down, so a drag is absolute
	QPointF spotStartLocal_;   // press point in mask axes
	QPointF spotStartCanvas_;  // press point in normalised canvas terms
	bool spotSnap_ = true;     // Shift bypasses it, as everywhere else
	QPointF spotGuideH_, spotGuideV_; // where a snap landed, for the guide lines
	bool spotGuideHOn_ = false, spotGuideVOn_ = false;
};

// Runtime-tweakable layout parameters for the trim Timeline (edited live from
// the editor's Developer Panel to find the best UI configuration).
struct TimelineLayoutParams {
	int pad = 12;      // left/right margin
	int barTop = 14;   // bar y
	int barH = 36;     // bar height (tall enough for the filmstrip)
	int handleW = 8;   // handle grab width
	int tileGap = 2;   // gap between filmstrip tiles
	int fontPx = 12;   // Start/End time-label font size
	double maxZoom = 32.0;
};

// A trim timeline with start/end handles and a playhead. Dragging a handle
// emits scrub() with the ms under it (for live preview) plus startChanged()/
// endChanged(); clicking the bar moves the playhead and scrubs. The bar shows
// a filmstrip of thumbnails (setThumbs) so each part of the video is easy to
// find, and supports zooming: Ctrl+scroll zooms around the cursor, plain
// scroll pans, and a thin indicator under the bar shows the visible window.
class Timeline : public QWidget {
	Q_OBJECT
public:
	explicit Timeline(QWidget *parent = nullptr);

	void setDuration(qint64 ms);
	void setStart(qint64 ms);
	void setEnd(qint64 ms);
	void setPlayhead(qint64 ms); // move the playhead (during playback) without emitting
	qint64 start() const { return start_; }
	qint64 end() const { return end_; }
	qint64 playhead() const { return playhead_; }

	// Filmstrip thumbnails; entry i covers time slice [i, i+1) * duration/count.
	void setThumbs(const QVector<QImage> &thumbs);

	// Developer Panel: tweak the layout live (invalidates the strip cache).
	const TimelineLayoutParams &layoutParams() const { return lp_; }
	void setLayoutParams(const TimelineLayoutParams &p);

signals:
	void startChanged(qint64 ms);
	void endChanged(qint64 ms);
	void scrub(qint64 ms);      // frame to preview while interacting
	void hoverScrub(qint64 ms); // preview while merely hovering (no click)

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void wheelEvent(QWheelEvent *) override;
	void leaveEvent(QEvent *) override;

private:
	int msToX(qint64 ms) const;
	qint64 xToMs(int x) const;
	qint64 visibleMs() const; // duration / zoom
	void clampView();
	// (Re)render the bar background + filmstrip into stripCache_ when the view
	// changed — repaints then blit it instead of rescaling every tile.
	void ensureStripCache(const QRect &bar);

	enum class Grab { None, Start, End, Playhead };
	Grab grab_ = Grab::None;

	TimelineLayoutParams lp_;

	qint64 duration_ = 0;
	qint64 start_ = 0;
	qint64 end_ = 0;
	qint64 playhead_ = 0;

	double zoom_ = 1.0;      // 1x = whole clip visible
	qint64 viewStart_ = 0;   // first visible ms
	qint64 hoverMs_ = -1;    // hover position marker
	QVector<QImage> thumbs_;

	// Filmstrip render cache (keyed on size/zoom/view/thumbs revision/dpr).
	QPixmap stripCache_;
	QSize stripCacheSize_;
	double stripCacheZoom_ = -1.0;
	qint64 stripCacheView_ = -1;
	int thumbsRev_ = 0;
	int stripCacheRev_ = -1;
	qreal stripCacheDpr_ = 0.0;
};

} // namespace harpia
