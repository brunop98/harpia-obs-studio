#pragma once

#include "TimelineModel.hpp"

#include <QFont>

#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QVector>
#include <QWidget>

class QPainter;
class QTimer;

namespace harpia {

// Runtime-tweakable geometry for the multi-track timeline (Dev-panel tunable).
struct TimelineViewParams {
	int gutterW = 124;    // left column: track headers (name + lock/hide/mute)
	int rulerH = 18;      // top time ruler
	int videoLaneH = 48;  // per video-track height
	int audioLaneH = 44;  // per audio-track height
	int laneGap = 3;
	int margin = 6;
	int minClipW = 6;     // don't let a clip shrink below this on screen
	int snapPx = 8;       // snap threshold in pixels
	int segFontPx = 10;
	int dropBandPx = 7;   // edge band that means "make a new track here"
	double maxZoom = 64.0;
};

// The "Full editing" timeline: a vertically stacked set of video + audio tracks
// over a single shared output-time axis (ruler + playhead + smooth zoom/pan).
// Clips carry explicit output positions, so they can sit anywhere with gaps and
// move between same-kind tracks. Reuses the ms<->px / ease / filmstrip patterns
// from TrackEditor and the waveform idea from VoiceoverTrack.
class TimelineView : public QWidget {
	Q_OBJECT
public:
	explicit TimelineView(QWidget *parent = nullptr);

	const TimelineModel &model() const { return model_; }
	void setModel(const TimelineModel &m); // replace (undo/redo/load); clears selection
	// Per-source filmstrip for video clips (keyed by EditorSource id), same feed
	// as TrackEditor::setSourceThumbs.
	void setSourceThumbs(int sourceId, const QVector<QImage> &thumbs, qint64 durationMs);

	// Append a clip to a track (creating a default track of the right kind when
	// the timeline is empty). Emits clipsChanged + selects it.
	void addClip(TlTrack::Kind kind, const TlClip &clip);
	int trackCount() const { return model_.tracks.size(); }

	void setPlayhead(qint64 outMs);
	void clearPlayhead();
	qint64 playhead() const { return playheadMs_; }
	qint64 durationMs() const { return model_.durationMs(); }

	// Selection (track index, clip index). {-1,-1} = nothing.
	int selectedTrack() const { return selTrack_; }
	int selectedClip() const { return selClip_; }
	const TlClip *selectedClipPtr() const;
	void updateSelectedClip(const TlClip &c); // Inspector edits push back here

	// Magnet: snap clip edges to the playhead, 0 and other clips while dragging.
	void setSnapEnabled(bool on);
	bool snapEnabled() const { return snap_; }

	// Track operations (also available from the header's right-click menu).
	void addTrack(TlTrack::Kind kind, int atIndex = -1); // -1 = top of that kind's group
	void deleteTrack(int index);

	// Fit the whole timeline in the view (zoom out to 1:1 on the full span).
	void zoomToFit();

	const TimelineViewParams &layoutParams() const { return lp_; }
	void setLayoutParams(const TimelineViewParams &p);

signals:
	void clipsChanged();
	void selectionChanged(int track, int clip);
	void scrub(qint64 outMs);      // preview at this output time (click/drag)
	void hoverScrub(qint64 outMs); // preview while hovering (no click)
	void inspectClipRequested();   // "Show in inspector" from a clip's menu

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
	// ---- geometry / axis ----
	QRect contentRect() const;             // x past the gutter, y past the ruler
	int laneHeight(int track) const;
	QRect laneRect(int track) const;       // full-width lane row (content x-range)
	QRect trackHeaderRect(int track) const;
	int laneAtY(int y) const;              // track index for a y, else -1
	qint64 spanMs() const;                 // total axis length (duration + tail)
	qint64 visibleMs() const;              // spanMs / zoom
	int msToX(qint64 ms) const;
	qint64 xToMs(int x) const;
	QRect clipRect(int track, int clip) const;
	void clampView();
	// Fonts used by the paint path, built once instead of per track and per clip
	// (constructing a QFont and calling setFont re-resolves it every time).
	void ensureFonts() const;
	mutable QFont hdrFont_;    // track name
	mutable QFont toggleFont_; // L/H/M chips
	mutable QFont clipFont_;   // clip label bar
	mutable int fontsForPx_ = -1; // segFontPx the cached fonts were built for
	void changeEvent(QEvent *e) override; // drop the cache when the widget font changes

	void drawRuler(QPainter &p) const;
	void drawClip(QPainter &p, int track, int clip) const;

	int clipAtPoint(const QPoint &p, int *trackOut) const; // clip index or -1
	qint64 snap(qint64 ms, int ignoreTrack, int ignoreClip) const;

	// Where a dragged clip would land: an existing lane, or a brand-new track
	// inserted at `newTrackAt` (dragging past a lane edge creates one).
	struct DropTarget {
		int track = -1;
		int newTrackAt = -1;
		bool valid() const { return track >= 0 || newTrackAt >= 0; }
	};
	DropTarget dropTargetAt(int y, TlTrack::Kind kind) const;
	int insertYFor(int newTrackAt) const; // y of the "new track here" indicator

	// Header widgets: the small lock / hide / mute toggles in the gutter.
	enum class HeaderHit { None, Lock, Hide, Mute };
	QRect headerToggleRect(int track, HeaderHit which) const;
	HeaderHit headerHitAt(int track, const QPoint &p) const;
	void showTrackMenu(int track, const QPoint &globalPos);
	void renumberTracks(); // V1..Vn bottom-up, A1..An top-down
	static QColor randomPastel();

	// ---- data ----
	TimelineModel model_;
	QHash<int, QVector<QImage>> srcThumbs_;
	QHash<int, qint64> srcThumbDur_;
	QHash<int, double> srcAspect_;

	int selTrack_ = -1, selClip_ = -1;
	qint64 playheadMs_ = -1;
	qint64 hoverMs_ = -1;

	// ---- smooth zoom/pan over output-time ----
	double zoom_ = 1.0;
	qint64 viewStart_ = 0;
	double zoomTarget_ = 1.0;
	qint64 viewTarget_ = 0;
	qint64 zoomAnchorMs_ = 0;
	double zoomAnchorFrac_ = 0.0;
	QTimer *anim_ = nullptr;
	void animateStep();

	// ---- interaction ----
	enum class Mode { None, Move, ResizeLeft, ResizeRight, Scrub };
	Mode mode_ = Mode::None;
	QPoint pressPos_;
	bool dragMoved_ = false;
	// captured at press
	TlClip dragOrig_;
	int dragTrack_ = -1, dragClip_ = -1;
	double dragSrcPerPx_ = 0.0;
	qint64 dragGrabOffsetMs_ = 0; // cursor->clip-start at grab
	DropTarget drop_;             // live drop target while moving a clip
	bool snap_ = true;            // magnet

	TimelineViewParams lp_;

	void showClipMenu(int track, int clip, const QPoint &globalPos, qint64 atOutMs);
	void splitClip(int track, int clip, qint64 atOutMs);
	void deleteSelected();
	void emitScrubAt(qint64 outMs);
};

} // namespace harpia
