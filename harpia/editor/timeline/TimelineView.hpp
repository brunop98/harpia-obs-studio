#pragma once

#include "TimelineModel.hpp"
#include "../EditorColors.hpp"

#include <QFont>

#include <QHash>
#include <QSet>
#include <QImage>
#include <QPixmap>
#include <QVector>
#include <QWidget>

#include <functional>

class QMimeData;
class QPainter;
class QTimer;

namespace harpia {

// Runtime-tweakable geometry for the multi-track timeline (Dev-panel tunable).
struct TimelineViewParams {
	int gutterW = 124;    // left column: track headers (name + lock/hide/mute)
	int rulerH = 18;      // top time ruler
	int videoLaneH = 48;  // per video-track height
	int audioLaneH = 44;  // per audio-track height
	int effectLaneH = 30; // effect tracks are thinner: they carry no picture
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
	// Put a clip on an existing lane, or on a new one inserted at `newTrackAt`.
	// The drop handler needs this: addClip() appends to the LAST lane of a kind,
	// which is not where the pointer was. Returns the track it landed on.
	int addClipAt(TlTrack::Kind kind, const TlClip &clip, int track, int newTrackAt);
	int trackCount() const { return model_.tracks.size(); }

	void setPlayhead(qint64 outMs);
	void clearPlayhead();
	qint64 playhead() const { return playheadMs_; }
	qint64 durationMs() const { return model_.durationMs(); }
	qint64 viewStartMs() const { return viewStart_; } // leftmost visible time
	// The time<->pixel mapping, so callers (and tests) can aim at a time on the
	// widget instead of re-deriving the axis from the layout params.
	int xForMs(qint64 ms) const { return msToX(ms); }
	qint64 msForX(int x) const { return xToMs(x); }

	// One clip on the clipboard, with the track it came from so a paste can put
	// it back where it belongs rather than always on the first track.
	struct ClipboardEntry {
		TlClip clip;
		int track = 0;
	};
	// Everything selected, primary first, in timeline order.
	QVector<ClipboardEntry> copySelection() const;
	// Place `entries` with the earliest landing at `atMs`, keeping their relative
	// times and tracks. The pasted clips become the new selection.
	void pasteAt(const QVector<ClipboardEntry> &entries, qint64 atMs);
	bool hasSelection() const { return selClip_ >= 0 || !extraSel_.isEmpty(); }
	void selectAllClips();
	// Select one clip as the primary, dropping any other selection.
	void selectClip(int track, int clip);
	// Add a clip to the selection without making it primary (what Ctrl-click does).
	void addToSelection(int track, int clip);
	// Shift every selected clip in time (arrow-key nudge). Locked tracks ignore it.
	void nudgeSelection(qint64 deltaMs);
	// Cut every unlocked clip the playhead runs through, on every track.
	void splitAtPlayhead();
	void deleteSelected(); // every selected clip, honouring per-track ripple
	// Add an effect clip to a specific effect track at a specific time.
	void addEffectClipAt(int track, qint64 atOutMs, FxType type);
	// Overlap this clip with the next one, which is how a transition is made.
	void makeTransitionWithNext(int track, int clip);
	int clipAfter(int track, int clip) const;              // by start time, or -1
	qint64 overlapWith(int track, int a, int b) const;     // ms, 0 when apart
	// Markers: add one at the playhead, or remove the one already there.
	void toggleMarkerAtPlayhead();
	// Nearest marker before/after `fromMs`, or -1 when there is none that way.
	qint64 markerNear(qint64 fromMs, bool forward) const;
	bool isSelected(int track, int clip) const;

	// A transition is selected instead of a clip when the click lands in an
	// overlap. It is identified by the INCOMING clip, which is where its
	// settings live.
	bool transitionSelected() const { return selTransition_; }
	int selectedTransitionTrack() const { return selTransition_ ? selTrack_ : -1; }
	int selectedTransitionClip() const { return selTransition_ ? selClip_ : -1; }
	// Remove the selected transition by snapping the clips together: the overlap
	// IS the transition, so closing it is how you delete one.
	void removeSelectedTransition();

	// Selection (track index, clip index). {-1,-1} = nothing.
	int selectedTrack() const { return selTrack_; }
	int selectedClip() const { return selClip_; }
	const TlClip *selectedClipPtr() const;
	void updateSelectedClip(const TlClip &c); // Inspector edits push back here
	// Every selected clip as (track, clip), primary first, in timeline order.
	QVector<QPair<int, int>> selectedPairs() const;
	// Edit EVERY selected clip in one go, emitting clipsChanged once at the end.
	// One signal, so the window takes one undo snapshot however many clips the
	// edit touched — which is what makes a multi-clip change one Ctrl-Z.
	void applyToSelection(const std::function<void(TlClip &)> &fn);

	// Magnet: snap clip edges to the playhead, 0 and other clips while dragging.
	void setSnapEnabled(bool on);
	bool snapEnabled() const { return snap_; }
	// Frame grid a fade drag rounds to (the project frame rate).
	void setFrameRate(double fps) { fps_ = (fps > 1.0) ? fps : 30.0; }

	// Track operations (also available from the header's right-click menu).
	void addTrack(TlTrack::Kind kind, int atIndex = -1); // -1 = top of that kind's group
	void deleteTrack(int index);

	// Fit the whole timeline in the view (zoom out to 1:1 on the full span).
	void zoomToFit();

	const EditorColors &colors() const { return cl_; }
	void setColors(const EditorColors &c)
	{
		cl_ = c;
		update();
	}

	const TimelineViewParams &layoutParams() const { return lp_; }
	void setLayoutParams(const TimelineViewParams &p);

signals:
	void clipsChanged();
	// Media files dropped straight onto the lanes. The view knows WHERE they
	// landed; it does not know how to read a PNG or an MP4, and the media pool
	// they have to be registered in belongs to the window -- so it reports the
	// drop and the window builds the clips. `newTrackAt >= 0` means the drop was
	// between lanes (or past the last one) and asked for a track to be made
	// there; otherwise `track` is the lane it landed on.
	void filesDropped(const QStringList &paths, int track, int newTrackAt, qint64 outMs);
	// A finished, discrete edit (drag released, split, delete, paste, track op).
	// Always paired with clipsChanged; the window uses it to close an undo entry
	// immediately instead of waiting out the coalescing timer.
	void editCommitted();
	void selectionChanged(int track, int clip);
	void scrub(qint64 outMs);      // preview at this output time (click/drag)
	void hoverScrub(qint64 outMs); // preview while hovering (no click)
	void inspectClipRequested();   // "Show in inspector" from a clip's menu
	void keyframeEditorRequested();// "Keyframes…" from a clip's menu

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void mouseDoubleClickEvent(QMouseEvent *) override;
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

	// spanMs() asks the model for its duration, which walks every clip in the
	// project — and msToX() asks spanMs() on every single call. A paint does
	// that twice per clip, so the axis cost grew with the SQUARE of the clip
	// count and dominated the paint on a busy timeline.
	//
	// The model cannot change while a paint is running, so a paint measures the
	// span once and answers from that. The guard is RAII so an early return
	// cannot leave a stale value behind; outside its lifetime spanMs() goes
	// back to measuring, which keeps every editing path exact.
	class SpanGuard {
	public:
		explicit SpanGuard(const TimelineView *v) : v_(v) { v_->spanCache_ = v_->spanMs(); }
		~SpanGuard() { v_->spanCache_ = -1; }
		SpanGuard(const SpanGuard &) = delete;
		SpanGuard &operator=(const SpanGuard &) = delete;

	private:
		const TimelineView *v_;
	};
	mutable qint64 spanCache_ = -1;
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

	void commitEdit(); // clipsChanged + editCommitted, for a finished action

	void drawRuler(QPainter &p) const;
	void drawClip(QPainter &p, int track, int clip) const;

	// ---- fade handles (audio clips) ----
	// Two grips at the top corners of an audio clip: drag them inwards to set
	// the fade, double-click to clear it. The envelope under them is the same
	// curve the mixer applies, so what is drawn is what is heard.
	enum class FadeSide { None, In, Out };
	struct FadeHit {
		int track = -1;
		int clip = -1;
		FadeSide side = FadeSide::None;
		bool valid() const { return side != FadeSide::None && track >= 0; }
		bool operator==(const FadeHit &o) const
		{
			return track == o.track && clip == o.clip && side == o.side;
		}
	};
	bool clipTakesFades(int track, int clip) const; // audio-track media clips
	// Where the grip sits: at the far end of the fade it controls.
	QRect fadeHandleRect(int track, int clip, FadeSide side) const;
	FadeHit fadeHandleAt(const QPoint &p) const;
	void drawFades(QPainter &p, int track, int clip) const;
	// Fade length for a cursor x on this clip, clamped and (unless Shift is
	// held) rounded to the timeline's frame grid.
	int fadeMsForX(const TlClip &c, const QRect &r, int x, FadeSide side, bool fine) const;

	int clipAtPoint(const QPoint &p, int *trackOut) const; // clip index or -1
	// The overlap under a point: the INCOMING clip's index, or -1.
	int transitionAtPoint(const QPoint &p, int *trackOut) const;
	QRect transitionRect(int track, int incoming) const;
	// Same, when the caller already has the whole track's overlaps in hand
	// (TlTrack::overlapsBefore) — asking per clip is O(clips²) across a track.
	QRect transitionRect(int track, int incoming, qint64 span) const;
	void drawTransition(QPainter &p, int track, int incoming, qint64 span) const;
	// Nearest snap candidate to `ms`, or `ms` itself when nothing is in range.
	// `hit` (optional) reports whether a candidate was actually taken — the
	// caller uses it to light up the guide line.
	qint64 snap(qint64 ms, int ignoreTrack, int ignoreClip, bool *hit = nullptr) const;

	// Where a dragged clip would land: an existing lane, or a brand-new track
	// inserted at `newTrackAt` (dragging past a lane edge creates one).
	struct DropTarget {
		int track = -1;
		int newTrackAt = -1;
		bool valid() const { return track >= 0 || newTrackAt >= 0; }
	};
	DropTarget dropTargetAt(int y, TlTrack::Kind kind) const;
	int insertYFor(int newTrackAt) const; // y of the "new track here" indicator

	// Dragging FILES in from outside. Reuses the same drop indicator as an
	// internal clip move, so the two read identically -- the highlighted lane or
	// the "new track here" line means the same thing whichever drag it is.
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dragMoveEvent(QDragMoveEvent *e) override;
	void dragLeaveEvent(QDragLeaveEvent *e) override;
	void dropEvent(QDropEvent *e) override;
	static QStringList droppableFiles(const QMimeData *mime);
	bool fileDrag_ = false;

	// Header widgets: the small lock / hide / mute toggles in the gutter.
	enum class HeaderHit { None, Lock, Hide, Mute };
	QRect headerToggleRect(int track, HeaderHit which) const;
	HeaderHit headerHitAt(int track, const QPoint &p) const;
	// `atOutMs` is where the new-effect entries place their clip.
	void showTrackMenu(int track, const QPoint &globalPos, qint64 atOutMs);
	void renumberTracks(); // V1..Vn bottom-up, A1..An top-down
	static QColor randomPastel();

	// ---- data ----
	TimelineModel model_;
	QHash<int, QVector<QImage>> srcThumbs_;
	QHash<int, qint64> srcThumbDur_;
	QHash<int, double> srcAspect_;

	int selTrack_ = -1, selClip_ = -1;
	// True when the click landed in an overlap: the same (track, clip) pair, but
	// meaning "the transition arriving on this clip" rather than the clip.
	bool selTransition_ = false;
	// The overlap under the cursor, for the hover highlight. {-1,-1} = none.
	int hoverTrTrack_ = -1, hoverTrClip_ = -1;
	// Clips selected ALONGSIDE the primary. The primary is what the Inspector
	// edits; delete, copy and drag act on the primary plus these.
	QSet<QPair<int, int>> extraSel_;
	// Kept so a drag can move every selected clip by the same amount.
	QHash<QPair<int, int>, qint64> dragStarts_;
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
	enum class Mode { None, Move, ResizeLeft, ResizeRight, Scrub, Fade };
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
	// Where the magnet is currently holding the dragged edge, so the drag can
	// show it. -1 = not snapped right now (or not dragging).
	qint64 snapLineMs_ = -1;
	FadeHit fadeHover_; // grip under the cursor (drawn highlighted)
	FadeHit fadeDrag_;  // grip being dragged
	// Frames per second the fade grid rounds to. Set by the window from the
	// project rate so a fade lands on a frame like every other edit does.
	double fps_ = 30.0;

	TimelineViewParams lp_;
	EditorColors cl_;

	void showClipMenu(int track, int clip, const QPoint &globalPos, qint64 atOutMs);
	void splitClip(int track, int clip, qint64 atOutMs);
	void emitScrubAt(qint64 outMs);
};

} // namespace harpia
