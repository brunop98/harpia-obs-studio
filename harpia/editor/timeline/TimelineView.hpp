#pragma once

#include "TimelineModel.hpp"
#include "TimelineSlice.hpp" // TlSpan (what "the selected portion" is)
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
class QMouseEvent;
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
	// Breathing room drawn INSIDE each clip's time span, per side. Two clips
	// that touch therefore show 2*clipGap of lane between them, and their
	// rounded corners face each other across it -- the "valley" that makes a
	// boundary visible at all. Purely cosmetic: hit testing uses the full span,
	// so the gap is not a dead strip you can click into.
	int clipGap = 1;
	int clipRadius = 5;   // corner radius; the valley is only as deep as this
	int splitSeamW = 1;   // the line drawn in the gap where a clip was split
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
	// Placing the view directly, and reading the time at a pixel. Both are for
	// tests: a pan is only correct if the moment you grabbed is still under the
	// pointer afterwards, and that is a question about pixels, not about state.
	void setViewStartForTest(qint64 ms)
	{
		viewStart_ = ms;
		clampView();
		update();
	}
	void setZoomForTest(double z)
	{
		zoom_ = z;
		clampView();
		update();
	}
	qint64 timeAtXForTest(int x) const { return xToMs(x); }
	qint64 visibleMsForTest() const { return visibleMs(); }
	// Move the visible window so it starts here (the overview dragging it).
	// Snaps rather than glides: a drag has to track the hand, and easing toward
	// a target that moves every mouse-move reads as lag.
	void setViewStart(qint64 startMs);
	// The span cache and its guard, for the perf regression test. The guard's
	// re-entrancy has no visible effect -- the numbers stay right either way --
	// so the only way to pin it is to look at the cache directly.
	qint64 spanMsForTest() const { return spanMs(); }
	qint64 spanCacheForTest() const { return spanCache_; }
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
	// The output-time span the selection covers, earliest start to latest end.
	// Invalid when nothing is selected. This is what "export the selected
	// portion" means: not the clips themselves, but the window of time they
	// occupy — everything on every track inside it comes along, which is the
	// only reading under which the excerpt looks like what you were watching.
	TlSpan selectionSpan() const;

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
	// Give a lane a name of your own. It survives every later add/remove of
	// other lanes; an empty name (or an automatic one like "A2") hands the lane
	// back to the automatic numbering.
	void renameTrack(int index, const QString &name);

	// ---- Whole-track copy / paste -------------------------------------------
	//
	// Clicking a track's header selects the TRACK rather than a clip, which is
	// what tells Ctrl+C it is being asked for all of it. The two selections are
	// exclusive: selecting one clears the other, so the shortcut never has to
	// guess and the user can see which it will act on.
	int selectedHeaderTrack() const { return selHeaderTrack_; }
	// The selected track, clips and all, or false when no header is selected.
	bool copySelectedTrack(TlTrack *out) const;
	// Insert a copy directly ABOVE `above`, clips at their original times -- a
	// duplicate, not a re-timing. Returns the index it landed at, or -1.
	// Clamped into the kind's own group: a video track cannot land among the
	// audio lanes, which is the ordering the compositor relies on.
	int pasteTrack(const TlTrack &t, int above);

	// Fit the whole timeline in the view (zoom out to 1:1 on the full span).
	void zoomToFit();

	// The content area, for tests: everything left of it is the track gutter,
	// and a test that recomputed the margins itself would drift from the layout
	// it is meant to be checking.
	QRect contentRectForTest() const { return contentRect(); }
	// Slot 0/1/2 = lock/hide/mute. For tests: the invariant being checked is
	// "this rect is inside the gutter", and a test computing the rect itself
	// would be checking its own arithmetic.
	QRect headerToggleRectForTest(int track, int slot) const
	{
		const HeaderHit h = slot == 0 ? HeaderHit::Lock
					      : (slot == 1 ? HeaderHit::Hide : HeaderHit::Mute);
		return headerToggleRect(track, h);
	}

	// The volume line's position and what is under a point. For tests: the
	// line's height IS the value, so a test that computed the mapping itself
	// would be checking its own arithmetic rather than the widget's.
	int volumeLineYForTest(int track, int clip) const { return volumeLineY(track, clip); }
	bool volumeHitForTest(const QPoint &p, int *track, int *clip) const
	{
		const VolHit h = volumeLineAt(p);
		if (track)
			*track = h.track;
		if (clip)
			*clip = h.clip;
		return h.valid();
	}

	// The magnet, asked directly. Snapping is only observable through a drag,
	// and a test that drove a drag to check WHICH candidate won would be
	// testing the drag as much as the candidate list.
	qint64 snapForTest(qint64 ms) const { return snap(ms, -1, -1); }
	// Where a key's pip is, and what is under a point. Both for tests: the pip
	// has to sit on its key's time, and a test computing the rect itself would
	// be checking its own arithmetic instead of the widget's.
	QPoint keyPipCenterForTest(int track, int clip, qint64 tMs) const;
	bool keyPipHitForTest(const QPoint &p, int *track, int *clip, qint64 *tMs) const;

	const EditorColors &colors() const { return cl_; }
	void setColors(const EditorColors &c)
	{
		cl_ = c;
		update();
	}

	const TimelineViewParams &layoutParams() const { return lp_; }
	void setLayoutParams(const TimelineViewParams &p);

	// Which of a drag's (or a clipboard's) files this widget would actually
	// take. Public because pasting has exactly the same question to ask: a file
	// copied in the file manager arrives as a URL list, not as a picture.
	static QStringList droppableFiles(const QMimeData *mime);
	static bool allAudio(const QStringList &files);
	// Middle-drag, or Alt+left-drag, slides the view. Public so a test can ask
	// the same question the press handler does.
	static bool canPanFrom(const QMouseEvent *e);
	// Which lane the drag in progress is aimed at (-1 = none, or a new one).
	// For tests: the indicator has to promise where the drop will really land,
	// and that promise is only checkable mid-drag.
	int dropTargetTrackForTest() const { return drop_.track; }

	// The two rects a clip has, for tests: the one you can click and the one
	// that gets drawn. That they are DIFFERENT is the point of the gap, and
	// that the first still tiles the lane is what keeps the gap clickable.
	QRect clipRectForTest(int track, int clip) const { return clipRect(track, clip); }
	QRect clipPaintRectForTest(int track, int clip) const
	{
		return clipPaintRect(track, clip);
	}

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
	// The pointer stopped hovering clips -- it left the view, or moved off onto
	// empty lane space. The preview has to go back to the playhead: a hover
	// borrows the picture, and something has to give it back, or the preview is
	// left showing a frame nothing on screen points at.
	void hoverScrubEnded();
	void inspectClipRequested();   // "Show in inspector" from a clip's menu
	void keyframeEditorRequested();// "Keyframes…" from a clip's menu
	// "Export this clip…" / "Export selection…". The window owns the export
	// dialog and the media pool, so the view only says which slice of output
	// time was asked for.
	void exportRangeRequested(qint64 fromMs, qint64 toMs);
	// The window being LOOKED AT moved -- a zoom, a scroll, a pan, a scrub.
	// Drives the floating overview that says where you are in a long project.
	// Deliberately not emitted by playback: the overview is for navigating.
	void viewChanged(qint64 totalMs, qint64 startMs, qint64 visibleMs, qint64 playheadMs);

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
	QRect contentRect() const;
	// The gutter's width, never less than its contents need. See the .cpp: a
	// gutter set too narrow used to push the Mute toggle under the first clip.
	int gutterWidth() const;
	static int minGutterWidth();             // x past the gutter, y past the ruler
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
	// Re-entrant: it restores whatever it found rather than clearing outright.
	// A guard held by a caller and another taken by a helper it calls is the
	// normal case -- and an inner guard that reset the cache to "measure again"
	// on the way out would silently switch the OUTER one off for the rest of
	// its scope, which is a slowdown with no symptom.
	class SpanGuard {
	public:
		explicit SpanGuard(const TimelineView *v) : v_(v), prev_(v->spanCache_)
		{
			v_->spanCache_ = v_->spanMs();
		}
		~SpanGuard() { v_->spanCache_ = prev_; }
		SpanGuard(const SpanGuard &) = delete;
		SpanGuard &operator=(const SpanGuard &) = delete;

	private:
		const TimelineView *v_;
		qint64 prev_;
	};
	mutable qint64 spanCache_ = -1;
	// The axis span, held for the length of a drag that moves or trims a clip.
	// -1 = not dragging, measure it.
	//
	// Every pixel on this widget derives from the project's TOTAL span, so a
	// clip dragged past the end grows the span and rescales the whole timeline
	// -- while the drag is still going. The ruler stretches, the other clips
	// shrink, and the clip under the cursor stops tracking the cursor. Holding
	// it means the one rescale that has to happen happens once, on the drop.
	qint64 dragSpanMs_ = -1;

public:
	using SpanGuardForTest = SpanGuard;

private:
	int msToX(qint64 ms) const;
	qint64 xToMs(int x) const;
	// clipRect is the clip's TIME span, and is what hit testing, dragging and
	// the trim/fade grips all use. clipPaintRect is that inset by clipGap per
	// side -- the shape actually drawn. Keeping them apart is the whole reason
	// the gap can exist without turning into a strip that swallows clicks.
	QRect clipRect(int track, int clip) const;
	QRect clipPaintRect(int track, int clip) const;
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

	// ---- the volume rubber-band ------------------------------------------
	// A horizontal line across an audio clip whose HEIGHT is the level: the
	// bottom of the clip is silence, the top is kMaxClipVolume, and unity sits
	// across the middle. Drag it to set the level where you can see the
	// waveform it applies to, which is where every other editor puts it.
	//
	// Linear and 0..2, deliberately the same numbers as the Inspector's slider:
	// two controls for one value that disagree about its scale are two controls
	// you have to convert between in your head.
	static constexpr double kMaxClipVolume = 2.0;
	struct VolHit {
		int track = -1;
		int clip = -1;
		bool valid() const { return track >= 0 && clip >= 0; }
		bool operator==(const VolHit &o) const { return track == o.track && clip == o.clip; }
	};
	// The band the line is drawn and grabbed in: the clip's rect, inset so the
	// line is still visible (and grabbable) at 0 and at maximum.
	QRect volumeBand(int track, int clip) const;
	int volumeLineY(int track, int clip) const;              // -1 = no line here
	// The level a drag has reached: from where it STARTED, by how far the
	// pointer has moved. Relative rather than absolute so that "fine" can mean
	// something -- an audio lane is about thirty pixels tall, which is the whole
	// 0..2 range in thirty steps, and no amount of care gets you 1.05 out of
	// that. At normal speed the two agree exactly, because the drag starts on
	// the line.
	double volumeForDrag(int track, int clip, int pressY, double fromVol, int y,
			     bool fine) const;
	VolHit volumeLineAt(const QPoint &p) const;
	void drawVolumeLine(QPainter &p, int track, int clip) const;
	VolHit volDrag_;   // the line being dragged
	VolHit volHover_;  // the line under the pointer, lit so the grab is honest
	double volDragFrom_ = -1.0; // the level it started at, so a no-op is not an edit
	int volDragFromY_ = 0;      // and where the pointer was when it did
	// Where the grip sits: at the far end of the fade it controls.
	QRect fadeHandleRect(int track, int clip, FadeSide side) const;
	FadeHit fadeHandleAt(const QPoint &p) const;
	void drawFades(QPainter &p, int track, int clip) const;
	// Fade length for a cursor x on this clip, clamped and (unless Shift is
	// held) rounded to the timeline's frame grid.
	int fadeMsForX(const TlClip &c, const QRect &r, int x, FadeSide side, bool fine) const;

	// ---- keyframe pips (the diamonds along a clip's top edge) ----
	// One pip per MOMENT the clip has keys at, whatever kind they are (see
	// forEachClipKeyTime). They used to be paint-only: an animated clip said so
	// and that was all you could do with it -- retiming a key meant a separate
	// window. Now they are things you can click, drag and delete, on the
	// timeline where the rest of the edit happens.
	struct KeyHit {
		int track = -1;
		int clip = -1;
		qint64 tMs = -1; // clip-relative output ms of the key column
		bool valid() const { return track >= 0 && clip >= 0 && tMs >= 0; }
		bool operator==(const KeyHit &o) const
		{
			return track == o.track && clip == o.clip && tMs == o.tMs;
		}
	};
	// The pip's centre for a key at `tMs` inside the clip drawn in `r`.
	QPoint keyPipCenter(const QRect &r, qint64 tMs, qint64 durMs) const;
	KeyHit keyPipAt(const QPoint &p) const;
	void drawKeyPips(QPainter &p, int track, int clip) const;
	// Scratch for the paint and hit-test paths, reused rather than reallocated:
	// both run per clip, the second on every mouse-move.
	mutable QVector<qint64> keyTimes_;
	KeyHit keyDrag_;      // the pip being dragged (invalid when none)
	KeyHit keyHover_;     // the pip under the pointer, lit so the grab is honest
	qint64 keyDragFrom_ = -1; // where that pip started, so a drag can be undone

	int clipAtPoint(const QPoint &p, int *trackOut) const; // clip index or -1
	// The overlap under a point: the INCOMING clip's index, or -1.
	int transitionAtPoint(const QPoint &p, int *trackOut) const;
	QRect transitionRect(int track, int incoming) const;
	// Same, when the caller already has the whole track's overlaps in hand
	// (TlTrack::overlapsBefore) — asking per clip is O(clips²) across a track.
	QRect transitionRect(int track, int incoming, qint64 span) const;
	void drawTransition(QPainter &p, int track, int incoming, qint64 span) const;
	// The mark where a clip was split in two, drawn in the gap between the
	// halves. See the definition for why a split is worth telling apart from
	// two unrelated clips that merely touch.
	void drawSplitSeams(QPainter &p, int track) const;
	void updateHoverSeam(const QPoint &pos);
	int hoverSeamTrack_ = -1;   // -1 = the pointer is not near a seam
	qint64 hoverSeamMs_ = -1;
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
	// Filmstrip tiles pre-scaled to their painted size, so playback repaints
	// blit instead of rescaling every visible tile 30 times a second.
	mutable QHash<int, QVector<QPixmap>> stripPix_;
	mutable QHash<int, QPair<int, int>> stripPixSize_;
	const QVector<QPixmap> &scaledStrip(int sourceId, const QVector<QImage> &strip, int tileW,
					    int th) const;
	QHash<int, qint64> srcThumbDur_;
	QHash<int, double> srcAspect_;

	int selTrack_ = -1, selClip_ = -1;
	// The track whose HEADER is selected, or -1. Exclusive with the clip
	// selection above: a gesture in the lanes clears this, and a click on a
	// header clears that.
	int selHeaderTrack_ = -1;
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
	// Grab-to-pan state. The view is moved from the pose at the PRESS, not
	// nudged per mouse-move: accumulating deltas drifts, and the promise of a
	// grab is that the instant you grabbed stays under the pointer.
	int panStartX_ = 0;
	qint64 panStartView_ = 0;
	double zoomTarget_ = 1.0;
	qint64 viewTarget_ = 0;
	qint64 zoomAnchorMs_ = 0;
	double zoomAnchorFrac_ = 0.0;
	QTimer *anim_ = nullptr;
	void animateStep();
	void notifyView(); // emit viewChanged with the current numbers

	// ---- interaction ----
	// Pan drags the VIEW under a still timeline, the opposite of Scrub, which
	// drags the playhead across a still view. Both are "navigating", and mixing
	// them up is why it needs its own mode rather than a flag on Scrub.
	enum class Mode { None, Move, ResizeLeft, ResizeRight, Scrub, Fade, Pan, KeyDrag, Volume };
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
	// The right-click menu on a keyframe pip: go to it, or delete it. Its own
	// menu rather than entries on the clip's, because what is under the cursor
	// is one MOMENT of one clip, and half the clip menu (split, export, make a
	// transition) has nothing to do with it.
	void showKeyMenu(const KeyHit &hit, const QPoint &globalPos);
	void splitClip(int track, int clip, qint64 atOutMs);
	void emitScrubAt(qint64 outMs);
};

} // namespace harpia
