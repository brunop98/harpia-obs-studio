#include "TimelineView.hpp"

#include "editor/Filmstrip.hpp"

#include "../MediaFiles.hpp"

#include "../component/BuiltinComponents.hpp"
#include "../component/ComponentRegistry.hpp"

#include "../../ui/UiIcons.hpp"
#include "../../ui/ColorField.hpp"
#include "../TimeText.hpp"

#include <QColorDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QKeyEvent>
#include <QHash>
#include <QMenu>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QToolTip>
#include <QRandomGenerator>
#include <QTimer>
#include <QWheelEvent>

#include <limits>

namespace harpia {

namespace {
// An effect clip's strip label, asking the registry for each component's
// display name. Here rather than in the model so TimelineModel keeps knowing
// nothing about the registry.
QString effectClipLabel(const TlClip &c)
{
	return c.effectLabel([](const QString &id) {
		const ComponentType *t = ComponentRegistry::instance().find(id);
		return t ? t->displayName : id;
	});
}
} // namespace

namespace {
constexpr qint64 kMinClipMs = 100;
constexpr qint64 kTailMs = 5000;   // draggable space past the end
constexpr qint64 kMinSpanMs = 8000;

// Colours all live in EditorColors (cl_), edited live from the Developer Panel.
} // namespace

TimelineView::TimelineView(QWidget *parent) : QWidget(parent)
{
	setFocusPolicy(Qt::ClickFocus);
	setMouseTracking(true);
	// Drop a video or an image straight onto the lanes.
	setAcceptDrops(true);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setToolTip(QStringLiteral("Drag clips to move · drag an edge to trim · right-click to split · "
				  "scroll to zoom, Shift+scroll to pan"));
	anim_ = new QTimer(this);
	anim_->setInterval(16);
	connect(anim_, &QTimer::timeout, this, &TimelineView::animateStep);
}

void TimelineView::animateStep()
{
	auto easeI = [](qint64 &cur, qint64 tgt) -> bool {
		if (cur == tgt)
			return false;
		qint64 step = qint64((tgt - cur) * 0.25);
		if (step == 0)
			step = (tgt > cur) ? 1 : -1;
		cur += step;
		if ((tgt > cur) != (step > 0))
			cur = tgt;
		return true;
	};
	auto easeZ = [](double &cur, double tgt) -> bool {
		if (std::abs(cur - tgt) < 1e-4)
			return false;
		cur += (tgt - cur) * 0.25;
		if (std::abs(cur - tgt) / std::max(1.0, tgt) < 1e-3)
			cur = tgt;
		return true;
	};
	bool moving = false;
	if (easeZ(zoom_, zoomTarget_)) {
		viewStart_ = zoomAnchorMs_ - qint64(zoomAnchorFrac_ * visibleMs());
		clampView();
		viewTarget_ = viewStart_;
		moving = true;
	} else if (easeI(viewStart_, viewTarget_)) {
		moving = true;
	}
	update();
	if (moving)
		notifyView(); // a zoom or scroll in progress IS the interaction
	else
		anim_->stop();
}

// One place that knows what "where I am" means here, so every caller is a
// single line and none of them can disagree about it.
void TimelineView::setViewStart(qint64 startMs)
{
	viewStart_ = startMs;
	clampView();
	// The ease targets have to follow, or the next animation frame would drag
	// the view back to wherever the last wheel gesture was heading.
	viewTarget_ = viewStart_;
	zoomTarget_ = zoom_;
	update();
	notifyView();
}

void TimelineView::notifyView()
{
	emit viewChanged(spanMs(), viewStart_, visibleMs(), playheadMs_);
}

void TimelineView::setModel(const TimelineModel &m)
{
	model_ = m;
	selTrack_ = selClip_ = -1;
	// The extras are indices into the OLD model; a new one makes them meaningless
	// and possibly out of range. Clearing the primary without them was how undo
	// came back holding a selection that pointed past the end of a track.
	extraSel_.clear();
	selTransition_ = false;
	hoverTrTrack_ = hoverTrClip_ = -1;
	clampView();
	update();
	updateGeometry();
}

void TimelineView::setSourceThumbs(int sourceId, const QVector<QImage> &thumbs, qint64 durationMs)
{
	srcThumbs_[sourceId] = thumbs;
	srcThumbDur_[sourceId] = durationMs;
	double aspect = 16.0 / 9.0;
	for (const QImage &t : thumbs)
		if (!t.isNull()) {
			aspect = double(t.width()) / double(t.height());
			break;
		}
	srcAspect_[sourceId] = aspect;
	// New thumbs invalidate the scaled-pixmap cache built from the old ones.
	stripPix_.remove(sourceId);
	stripPixSize_.remove(sourceId);
	update();
}

const QVector<QPixmap> &TimelineView::scaledStrip(int sourceId, const QVector<QImage> &strip, int tileW,
						  int th) const
{
	// Rebuilt only when the thumbs changed (handled in setSourceThumbs) or the
	// tile geometry did (lane height / aspect) -- zooming the timeline does
	// NOT change tile size, so scrub and playback repaints always hit.
	auto szIt = stripPixSize_.constFind(sourceId);
	auto pxIt = stripPix_.find(sourceId);
	if (szIt == stripPixSize_.constEnd() || szIt.value() != qMakePair(tileW, th) ||
	    pxIt == stripPix_.end() || pxIt.value().size() != strip.size()) {
		QVector<QPixmap> v;
		v.reserve(strip.size());
		for (const QImage &im : strip)
			v.append(im.isNull() ? QPixmap()
					     : QPixmap::fromImage(im.scaled(
						       tileW, th, Qt::IgnoreAspectRatio,
						       Qt::SmoothTransformation)));
		pxIt = stripPix_.insert(sourceId, v);
		stripPixSize_.insert(sourceId, qMakePair(tileW, th));
	}
	return pxIt.value();
}

QColor TimelineView::randomPastel()
{
	// Random hue, gentle saturation: distinct but never garish, and dark enough
	// that white clip text stays readable.
	const int hue = QRandomGenerator::global()->bounded(360);
	return QColor::fromHsv(hue, 90, 150);
}

void TimelineView::renumberTracks()
{
	// Video lanes are numbered bottom-up (V1 is the bottom/background layer,
	// matching the convention that a higher lane renders in front); audio
	// top-down.
	const int nv = model_.videoTrackCount();
	int a = 0, e = 0, v = 0;
	for (int i = 0; i < model_.tracks.size(); ++i) {
		TlTrack &t = model_.tracks[i];
		switch (t.kind) {
		case TlTrack::Kind::Video:
			// Numbered bottom-up, so V1 is the lowest picture track.
			t.name = QStringLiteral("V%1").arg(nv - v++);
			break;
		case TlTrack::Kind::Effect:
			t.name = QStringLiteral("FX%1").arg(++e);
			break;
		case TlTrack::Kind::Audio:
			t.name = QStringLiteral("A%1").arg(++a);
			break;
		}
	}
}

void TimelineView::addTrack(TlTrack::Kind kind, int atIndex)
{
	const int np = model_.pictureTrackCount();
	// Picture tracks (video + effect) live above audio tracks; clamp the
	// insertion into that group. An effect track is a picture track: it has to
	// be able to sit between two video tracks, because where it sits is what
	// decides what it grades.
	const int lo = TimelineModel::isPictureKind(kind) ? 0 : np;
	const int hi = TimelineModel::isPictureKind(kind) ? np : model_.tracks.size();
	int at = (atIndex < 0) ? lo : std::clamp(atIndex, lo, hi);

	TlTrack t;
	t.kind = kind;
	t.color = randomPastel();
	model_.tracks.insert(at, t);
	if (selTrack_ >= at)
		++selTrack_;
	renumberTracks();
	updateGeometry();
	update();
	commitEdit();
}

void TimelineView::deleteTrack(int index)
{
	if (index < 0 || index >= model_.tracks.size())
		return;
	model_.tracks.remove(index);
	if (selTrack_ == index)
		selTrack_ = selClip_ = -1;
	else if (selTrack_ > index)
		--selTrack_;
	renumberTracks();
	clampView();
	updateGeometry();
	update();
	emit selectionChanged(selTrack_, selClip_);
	commitEdit();
}

void TimelineView::setSnapEnabled(bool on)
{
	if (snap_ == on)
		return;
	snap_ = on;
	if (!on)
		snapLineMs_ = -1; // toggled off mid-drag: drop the guide too
	update();
}

void TimelineView::zoomToFit()
{
	zoom_ = zoomTarget_ = 1.0; // zoom 1 == the whole span across the view
	viewStart_ = viewTarget_ = 0;
	if (anim_)
		anim_->stop();
	update();
}

void TimelineView::addClip(TlTrack::Kind kind, const TlClip &clip)
{
	int idx = -1;
	for (int i = model_.tracks.size() - 1; i >= 0; --i)
		if (model_.tracks[i].kind == kind) {
			idx = i;
			break;
		}
	if (idx < 0) {
		const int np = model_.pictureTrackCount();
		TlTrack t;
		t.kind = kind;
		t.color = randomPastel();
		idx = TimelineModel::isPictureKind(kind) ? np : model_.tracks.size();
		model_.tracks.insert(idx, t);
		renumberTracks();
	}
	model_.tracks[idx].clips.append(clip);
	selTrack_ = idx;
	selClip_ = model_.tracks[idx].clips.size() - 1;
	clampView();
	updateGeometry();
	update();
	commitEdit();
	emit selectionChanged(selTrack_, selClip_);
}

// addClip() appends to the LAST lane of a kind, which is fine for a menu action
// but wrong for a drop: the pointer was over a particular lane, or between two
// of them. This takes the answer dropTargetAt() already worked out.
int TimelineView::addClipAt(TlTrack::Kind kind, const TlClip &clip, int track, int newTrackAt)
{
	int target = -1;
	if (newTrackAt >= 0) {
		TlTrack t;
		t.kind = kind;
		t.color = randomPastel();
		target = std::clamp(newTrackAt, 0, int(model_.tracks.size()));
		model_.tracks.insert(target, t);
		renumberTracks();
	} else if (track >= 0 && track < model_.tracks.size() &&
		   model_.tracks[track].kind == kind && !model_.tracks[track].locked) {
		target = track;
	}
	if (target < 0) {
		// Nowhere sensible was named -- a locked lane, or one of the wrong kind.
		// Falling back to addClip is better than dropping the file on the floor:
		// something appears, and it appears somewhere the user can see and move.
		addClip(kind, clip);
		return selTrack_;
	}
	model_.tracks[target].clips.append(clip);
	selTrack_ = target;
	selClip_ = model_.tracks[target].clips.size() - 1;
	clampView();
	updateGeometry();
	update();
	commitEdit();
	emit selectionChanged(selTrack_, selClip_);
	return target;
}

bool TimelineView::copySelectedTrack(TlTrack *out) const
{
	if (!out || selHeaderTrack_ < 0 || selHeaderTrack_ >= model_.tracks.size())
		return false;
	*out = model_.tracks[selHeaderTrack_]; // clips and all -- a track IS its clips
	return true;
}

int TimelineView::pasteTrack(const TlTrack &t, int above)
{
	TlTrack copy = t;
	// A fresh colour, so the duplicate is telling apart from its original at a
	// glance. The NAME is left to renumberTracks below for the same reason: two
	// lanes both called V1 is how you lose track of which one you just made.
	copy.color = randomPastel();
	copy.name.clear();

	// Clamp into the kind's own group. Picture tracks occupy [0, np) and audio
	// [np, n): dropping a video lane in among the audio ones would break the
	// ordering the compositor and the mixer both read.
	const int np = model_.pictureTrackCount();
	const bool picture = TimelineModel::isPictureKind(copy.kind);
	const int lo = picture ? 0 : np;
	const int hi = picture ? np : int(model_.tracks.size());
	// "Above" is a smaller index: index 0 is the TOP lane.
	const int at = std::clamp(above >= 0 ? above : lo, lo, hi);

	model_.tracks.insert(at, copy);
	renumberTracks();
	// Select the new lane's header, not a clip: you just asked for a track, so
	// the thing now selected is a track -- and a second Ctrl+V stacks another
	// copy above this one rather than re-pasting onto the original.
	selHeaderTrack_ = at;
	selTrack_ = selClip_ = -1;
	selTransition_ = false;
	extraSel_.clear();
	clampView();
	updateGeometry();
	update();
	commitEdit();
	emit selectionChanged(-1, -1);
	return at;
}

// True when every file in the drag is audio-only, so the whole drag belongs on
// an audio lane. A mixed drag (a video and its music) is a picture drag: the
// audio in it finds its own lane once the window unpacks it.
bool TimelineView::allAudio(const QStringList &files)
{
	if (files.isEmpty())
		return false;
	for (const QString &f : files)
		if (!isAudioFile(f))
			return false;
	return true;
}

// Is this press a grab-to-pan? Middle button anywhere, or Alt+left for the
// mice and trackpads that have no middle button.
bool TimelineView::canPanFrom(const QMouseEvent *e)
{
	if (e->button() == Qt::MiddleButton)
		return true;
	return e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier);
}

QStringList TimelineView::droppableFiles(const QMimeData *mime)
{
	QStringList out;
	if (!mime || !mime->hasUrls())
		return out;
	for (const QUrl &u : mime->urls()) {
		const QString f = u.toLocalFile();
		if (!f.isEmpty() && isMediaFile(f))
			out << f;
	}
	return out;
}

void TimelineView::dragEnterEvent(QDragEnterEvent *e)
{
	if (droppableFiles(e->mimeData()).isEmpty())
		return;
	fileDrag_ = true;
	e->acceptProposedAction();
}

void TimelineView::dragMoveEvent(QDragMoveEvent *e)
{
	if (!fileDrag_)
		return;
	// An all-audio drag targets the AUDIO group, everything else the picture
	// group -- the same call an internal drag makes, so the indicator means the
	// same thing. Pointing a music file at a video lane and highlighting it
	// would have promised a landing place the drop cannot honour.
	drop_ = dropTargetAt(int(e->position().y()),
			     allAudio(droppableFiles(e->mimeData())) ? TlTrack::Kind::Audio
								     : TlTrack::Kind::Video);
	update();
	e->acceptProposedAction();
}

void TimelineView::dragLeaveEvent(QDragLeaveEvent *)
{
	fileDrag_ = false;
	drop_ = DropTarget();
	update();
}

void TimelineView::dropEvent(QDropEvent *e)
{
	const QStringList files = droppableFiles(e->mimeData());
	fileDrag_ = false;
	const DropTarget d = drop_;
	drop_ = DropTarget();
	update();
	if (files.isEmpty())
		return;
	// Where along the timeline it was dropped, not the playhead: the pointer is
	// the whole point of dropping onto the lanes rather than pressing Add.
	const qint64 at = std::max<qint64>(0, xToMs(int(e->position().x())));
	e->acceptProposedAction();
	emit filesDropped(files, d.track, d.newTrackAt, at);
}

const TlClip *TimelineView::selectedClipPtr() const
{
	if (selTrack_ < 0 || selTrack_ >= model_.tracks.size())
		return nullptr;
	const TlTrack &t = model_.tracks[selTrack_];
	if (selClip_ < 0 || selClip_ >= t.clips.size())
		return nullptr;
	return &t.clips[selClip_];
}

void TimelineView::updateSelectedClip(const TlClip &c)
{
	if (selTrack_ < 0 || selTrack_ >= model_.tracks.size())
		return;
	TlTrack &t = model_.tracks[selTrack_];
	if (selClip_ < 0 || selClip_ >= t.clips.size())
		return;
	t.clips[selClip_] = c;
	update();
	emit clipsChanged();
}

// One finished, discrete edit. Emitting the extra signal is what tells the
// window to write an undo entry NOW rather than 350ms from now: two drags in
// quick succession are two actions, and coalescing them into one entry is how
// "undo does nothing" happens (a drag out and back cancels to no change at all).
// The continuous streams -- an Inspector spin box, a held arrow key -- keep
// using plain clipsChanged() so they still coalesce.
void TimelineView::commitEdit()
{
	emit clipsChanged();
	emit editCommitted();
}

void TimelineView::setPlayhead(qint64 outMs)
{
	// Called 30 times a second during playback. A bare update() repainted the
	// ENTIRE widget -- every lane, every filmstrip tile rescale, every
	// waveform line -- to move a 1 px marker. Repaint only the two narrow
	// bands the marker actually occupies (old spot and new), unless the
	// playhead left the visible view, in which case follow-scroll or a full
	// repaint is genuinely needed.
	const qint64 old = playheadMs_;
	playheadMs_ = outMs;
	if (old >= 0 && outMs >= 0) {
		const int x0 = msToX(old);
		const int x1 = msToX(outMs);
		if (x0 >= -8 && x0 <= width() + 8 && x1 >= -8 && x1 <= width() + 8) {
			// ±4 px covers the line, its head triangle and antialiasing.
			update(QRect(std::min(x0, x1) - 4, 0, std::abs(x1 - x0) + 9, height()));
			return;
		}
	}
	update();
}

void TimelineView::clearPlayhead()
{
	playheadMs_ = -1;
	update();
}

void TimelineView::setLayoutParams(const TimelineViewParams &p)
{
	lp_ = p;
	zoom_ = std::clamp(zoom_, 1.0, lp_.maxZoom);
	clampView();
	updateGeometry();
	update();
}

// ---- geometry ---------------------------------------------------------------

// The gutter can never be narrower than what it has to hold.
//
// The header's toggles are laid out at fixed offsets from its left edge, so a
// gutter set narrower than they need does not squeeze them -- it pushes the
// rightmost one out into the content area, where the clips are painted
// afterwards and cover it. The Mute toggle simply vanished under the first
// clip, which is what "the thumbnail goes over the track controls" was.
//
// Derived from the same numbers headerToggleRect uses rather than written down
// twice, so adding a fourth toggle widens the floor instead of reintroducing
// this.
int TimelineView::minGutterWidth()
{
	constexpr int kPad = 8;    // headerToggleRect's left inset
	constexpr int kSize = 16;  // a toggle
	constexpr int kGap = 4;    // between them
	constexpr int kSlots = 3;  // lock, hide, mute -- the widest case (video)
	return kPad + kSlots * (kSize + kGap) - kGap + kPad;
}

int TimelineView::gutterWidth() const
{
	return std::max(lp_.gutterW, minGutterWidth());
}

QRect TimelineView::contentRect() const
{
	const int x = lp_.margin + gutterWidth();
	const int y = lp_.margin + lp_.rulerH;
	return QRect(x, y, std::max(1, width() - lp_.margin - x), std::max(1, height() - lp_.margin - y));
}

int TimelineView::laneHeight(int track) const
{
	if (track < 0 || track >= model_.tracks.size())
		return lp_.videoLaneH;
	switch (model_.tracks[track].kind) {
	case TlTrack::Kind::Video: return lp_.videoLaneH;
	case TlTrack::Kind::Effect: return lp_.effectLaneH;
	case TlTrack::Kind::Audio: break;
	}
	return lp_.audioLaneH;
}

QRect TimelineView::laneRect(int track) const
{
	const QRect c = contentRect();
	int y = c.y();
	for (int i = 0; i < track; ++i)
		y += laneHeight(i) + lp_.laneGap;
	return QRect(c.x(), y, c.width(), laneHeight(track));
}

QRect TimelineView::trackHeaderRect(int track) const
{
	const QRect l = laneRect(track);
	return QRect(lp_.margin, l.y(), gutterWidth(), l.height());
}

int TimelineView::laneAtY(int y) const
{
	for (int i = 0; i < model_.tracks.size(); ++i) {
		const QRect l = laneRect(i);
		if (y >= l.y() && y < l.y() + l.height())
			return i;
	}
	return -1;
}

qint64 TimelineView::spanMs() const
{
	if (spanCache_ >= 0)
		return spanCache_; // held by a SpanGuard, see the header
	return std::max<qint64>(kMinSpanMs, model_.durationMs() + kTailMs);
}

qint64 TimelineView::visibleMs() const
{
	return std::max<qint64>(1, qint64(spanMs() / zoom_));
}

int TimelineView::msToX(qint64 ms) const
{
	const QRect c = contentRect();
	return c.x() + int(double(ms - viewStart_) / double(visibleMs()) * c.width());
}

qint64 TimelineView::xToMs(int x) const
{
	const QRect c = contentRect();
	if (c.width() <= 0)
		return 0;
	const double t = double(x - c.x()) / double(c.width());
	return std::clamp<qint64>(viewStart_ + qint64(std::llround(t * visibleMs())), 0, spanMs());
}

QRect TimelineView::clipRect(int track, int clip) const
{
	const TlClip &c = model_.tracks[track].clips[clip];
	const QRect l = laneRect(track);
	const int x1 = msToX(c.outStartMs);
	const int x2 = msToX(c.outEndMs());
	return QRect(x1, l.y() + 2, std::max(lp_.minClipW, x2 - x1), l.height() - 4);
}

// The drawn shape: the time span, pulled in by clipGap on each side so that two
// clips which touch show a sliver of lane between them. Never below minClipW --
// on a clip already at the floor there is nothing to give away, and a boundary
// you cannot see is still better than a clip you cannot see.
QRect TimelineView::clipPaintRect(int track, int clip) const
{
	const QRect r = clipRect(track, clip);
	const int g = std::max(0, lp_.clipGap);
	if (r.width() <= lp_.minClipW + 2 * g)
		return r;
	return r.adjusted(g, 0, -g, 0);
}

void TimelineView::clampView()
{
	viewStart_ = std::clamp<qint64>(viewStart_, 0, std::max<qint64>(0, spanMs() - visibleMs()));
}

// ---- hit testing ------------------------------------------------------------

int TimelineView::clipAtPoint(const QPoint &p, int *trackOut) const
{
	const int track = laneAtY(p.y());
	if (trackOut)
		*trackOut = track;
	if (track < 0)
		return -1;
	const TlTrack &t = model_.tracks[track];
	for (int i = t.clips.size() - 1; i >= 0; --i)
		if (clipRect(track, i).contains(p))
			return i;
	return -1;
}

TimelineView::DropTarget TimelineView::dropTargetAt(int y, TlTrack::Kind kind) const
{
	DropTarget d;
	const int np = model_.pictureTrackCount();
	// Picture tracks occupy [0,np), audio [np,n). A clip can only land in its
	// own group, and a new track can only be inserted inside that group's range.
	const int lo = TimelineModel::isPictureKind(kind) ? 0 : np;
	const int hi = TimelineModel::isPictureKind(kind) ? np : model_.tracks.size();
	if (lo >= hi) { // no lane of this kind yet — the drop makes the first one
		d.newTrackAt = lo;
		return d;
	}
	const int band = std::max(3, lp_.dropBandPx);
	const QRect first = laneRect(lo);
	const QRect last = laneRect(hi - 1);

	if (y < first.top() + band) {
		d.newTrackAt = lo; // above the top lane of the group
		return d;
	}
	if (y > last.bottom() - band) {
		d.newTrackAt = hi; // below the bottom lane of the group
		return d;
	}
	for (int i = lo; i < hi; ++i) {
		const QRect l = laneRect(i);
		if (y < l.top() || y > l.bottom())
			continue;
		if (y < l.top() + band && i > lo) {
			d.newTrackAt = i; // between i-1 and i
			return d;
		}
		if (y > l.bottom() - band && i + 1 < hi) {
			d.newTrackAt = i + 1;
			return d;
		}
		d.track = i;
		return d;
	}
	// In a gap between lanes: insert there.
	for (int i = lo; i + 1 < hi; ++i)
		if (y > laneRect(i).bottom() && y < laneRect(i + 1).top()) {
			d.newTrackAt = i + 1;
			return d;
		}
	d.track = std::clamp(laneAtY(y), lo, hi - 1);
	return d;
}

int TimelineView::insertYFor(int newTrackAt) const
{
	if (model_.tracks.isEmpty())
		return contentRect().y();
	if (newTrackAt <= 0)
		return laneRect(0).top() - lp_.laneGap / 2;
	if (newTrackAt >= model_.tracks.size())
		return laneRect(model_.tracks.size() - 1).bottom() + lp_.laneGap / 2;
	return laneRect(newTrackAt).top() - lp_.laneGap / 2;
}

QRect TimelineView::headerToggleRect(int track, HeaderHit which) const
{
	const QRect h = trackHeaderRect(track);
	const int sz = 16;
	const int y = h.bottom() - sz - 3;
	int slot = 0;
	switch (which) {
	case HeaderHit::Lock:
		slot = 0;
		break;
	case HeaderHit::Hide:
		slot = 1;
		break;
	case HeaderHit::Mute:
		slot = (model_.tracks[track].kind == TlTrack::Kind::Video) ? 2 : 1;
		break;
	default:
		return QRect();
	}
	return QRect(h.x() + 8 + slot * (sz + 4), y, sz, sz);
}

TimelineView::HeaderHit TimelineView::headerHitAt(int track, const QPoint &p) const
{
	if (track < 0 || track >= model_.tracks.size())
		return HeaderHit::None;
	const bool video = model_.tracks[track].kind == TlTrack::Kind::Video;
	if (headerToggleRect(track, HeaderHit::Lock).contains(p))
		return HeaderHit::Lock;
	if (video && headerToggleRect(track, HeaderHit::Hide).contains(p))
		return HeaderHit::Hide;
	if (headerToggleRect(track, HeaderHit::Mute).contains(p))
		return HeaderHit::Mute;
	return HeaderHit::None;
}

void TimelineView::showTrackMenu(int track, const QPoint &globalPos, qint64 atOutMs)
{
	if (track < 0 || track >= model_.tracks.size())
		return;
	TlTrack &t = model_.tracks[track];
	const bool video = t.kind == TlTrack::Kind::Video;

	QMenu menu(this);
	// An effect track's whole purpose is to carry effects, so putting one on it
	// is the first thing its menu should offer. The submenu lists the types
	// directly: choosing one is otherwise "add a Brightness, then change it".
	QMenu *addFxMenu = nullptr;
	QHash<QAction *, int> addFxActions;
	if (t.kind == TlTrack::Kind::Effect && !t.locked) {
		addFxMenu = menu.addMenu(QStringLiteral("Add effect"));
		for (int i = 0; i < kFxTypeCount; ++i) {
			const FxType ft = FxType(i);
			// Inverse Selection is configured in the Spotlight panel, not by
			// dropping a clip, so offering it here would lead nowhere.
			if (ft == FxType::InverseSelection)
				continue;
			addFxActions.insert(addFxMenu->addAction(QString::fromLatin1(fxTypeName(ft))),
					    i);
		}
		menu.addSeparator();
	}
	QAction *lock = menu.addAction(t.locked ? QStringLiteral("Unlock track")
						: QStringLiteral("Lock track"));
	QAction *hide = video ? menu.addAction(t.hidden ? QStringLiteral("Show track")
							: QStringLiteral("Hide track"))
			      : nullptr;
	QAction *mute = menu.addAction(t.muted ? QStringLiteral("Unmute track")
					       : QStringLiteral("Mute track"));
	QAction *ripple = menu.addAction(QStringLiteral("Auto ripple on delete"));
	ripple->setCheckable(true);
	ripple->setChecked(t.ripple);
	menu.addSeparator();
	QAction *rename = menu.addAction(QStringLiteral("Rename track…"));
	QAction *colour = menu.addAction(QStringLiteral("Track colour…"));
	QAction *reroll = menu.addAction(QStringLiteral("Random colour"));
	menu.addSeparator();
	QAction *addAbove = menu.addAction(QStringLiteral("Add track above"));
	QAction *addBelow = menu.addAction(QStringLiteral("Add track below"));
	// An effect track can only go among the picture tracks: where it sits is
	// what decides which tracks it grades.
	QAction *addFx = TimelineModel::isPictureKind(t.kind)
				 ? menu.addAction(QStringLiteral("Add effect track above"))
				 : nullptr;
	QAction *del = menu.addAction(QStringLiteral("Delete track"));

	QAction *chosen = menu.exec(globalPos);
	if (!chosen)
		return;
	if (const auto it = addFxActions.constFind(chosen); it != addFxActions.constEnd()) {
		addEffectClipAt(track, atOutMs, FxType(it.value()));
		return;
	}
	if (chosen == lock) {
		t.locked = !t.locked;
	} else if (hide && chosen == hide) {
		t.hidden = !t.hidden;
	} else if (chosen == mute) {
		t.muted = !t.muted;
	} else if (chosen == ripple) {
		t.ripple = !t.ripple;
	} else if (chosen == rename) {
		bool ok = false;
		const QString n = QInputDialog::getText(this, QStringLiteral("Rename track"),
							QStringLiteral("Track name:"),
							QLineEdit::Normal, t.name, &ok)
					  .trimmed();
		if (!ok)
			return;
		// An empty name would leave the header blank with no way back, so it
		// falls back to the automatic V1/A1 numbering.
		t.name = n;
		if (t.name.isEmpty()) {
			renumberTracks();
			update();
			commitEdit();
			return;
		}
	} else if (chosen == colour) {
		// Live, and by index rather than through `t`: the reference points into
		// the track vector, and repainting between every movement is the point.
		const int ti = track;
		const QColor before = t.color;
		pickColorLive(this, QStringLiteral("Track colour"), before,
			      [this, ti](const QColor &c) {
				      if (ti >= 0 && ti < model_.tracks.size()) {
					      model_.tracks[ti].color = c;
					      update();
				      }
			      });
	} else if (chosen == reroll) {
		t.color = randomPastel();
	} else if (chosen == addAbove) {
		addTrack(t.kind, track);
		return;
	} else if (chosen == addBelow) {
		addTrack(t.kind, track + 1);
		return;
	} else if (addFx && chosen == addFx) {
		addTrack(TlTrack::Kind::Effect, track);
		return;
	} else if (chosen == del) {
		deleteTrack(track);
		return;
	}
	update();
	commitEdit();
}

// ---- transitions (clip overlaps) --------------------------------------------

QRect TimelineView::transitionRect(int track, int incoming) const
{
	if (track < 0 || track >= model_.tracks.size())
		return {};
	const TlTrack &t = model_.tracks[track];
	if (incoming < 0 || incoming >= t.clips.size())
		return {};
	return transitionRect(track, incoming, t.overlapBefore(incoming));
}

QRect TimelineView::transitionRect(int track, int incoming, qint64 span) const
{
	if (track < 0 || track >= model_.tracks.size())
		return {};
	const TlTrack &t = model_.tracks[track];
	if (incoming < 0 || incoming >= t.clips.size())
		return {};
	if (span <= 0)
		return {};
	const TlClip &c = t.clips[incoming];
	const QRect lane = laneRect(track);
	const int x0 = msToX(c.outStartMs);
	const int x1 = msToX(c.outStartMs + span);
	return QRect(x0, lane.y() + 2, std::max(2, x1 - x0), lane.height() - 4);
}

int TimelineView::transitionAtPoint(const QPoint &p, int *trackOut) const
{
	const int track = laneAtY(p.y());
	if (track < 0 || track >= model_.tracks.size())
		return -1;
	const TlTrack &t = model_.tracks[track];
	if (t.kind != TlTrack::Kind::Video)
		return -1; // only picture tracks show a transition
	// One sweep for the track, not one scan per clip: this runs on every single
	// mouse-move over the timeline.
	const QVector<qint64> spans = t.overlapsBefore();
	for (int i = 0; i < t.clips.size(); ++i) {
		if (spans[i] <= 0 || !t.clips[i].transition.enabled)
			continue;
		if (transitionRect(track, i, spans[i]).contains(p)) {
			if (trackOut)
				*trackOut = track;
			return i;
		}
	}
	return -1;
}

void TimelineView::drawTransition(QPainter &p, int track, int incoming, qint64 span) const
{
	const QRect r = transitionRect(track, incoming, span);
	if (r.isEmpty() || r.right() < contentRect().x() || r.x() > contentRect().right())
		return;
	const bool sel = selTransition_ && selTrack_ == track && selClip_ == incoming;
	const bool hot = hoverTrTrack_ == track && hoverTrClip_ == incoming;

	p.save();
	p.setClipRect(contentRect());
	// A shaded band so the overlap is unmistakably a region, not just two clips
	// that happen to touch.
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0xff, 0xff, 0xff, sel ? 62 : (hot ? 42 : 26)));
	p.drawRect(r);
	// The classic crossing diagonals: one line falling, one rising, so which
	// clip is leaving and which arriving is readable at a glance.
	p.setPen(QPen(QColor(0xff, 0xff, 0xff, sel ? 235 : 165), sel ? 2 : 1));
	p.drawLine(r.topLeft(), r.bottomRight());
	p.drawLine(r.bottomLeft(), r.topRight());
	p.setPen(QPen(QColor(0xff, 0xff, 0xff, sel ? 220 : 120), 1));
	p.drawLine(r.topLeft(), r.bottomLeft());
	p.drawLine(r.topRight(), r.bottomRight());
	// Name it when there is room, so a wipe is not mistaken for a crossfade.
	if (r.width() > 64) {
		p.setFont(clipFont_);
		p.setPen(QColor(0xff, 0xff, 0xff, 220));
		p.drawText(r, Qt::AlignCenter,
			   p.fontMetrics().elidedText(
				   QString::fromLatin1(
					   transitionName(model_.tracks[track].clips[incoming].transition.type)),
				   Qt::ElideRight, r.width() - 8));
	}
	p.restore();
}

// Which split seam the pointer is over, if any. Repaints only on a change: this
// runs on every mouse-move across the whole widget.
void TimelineView::updateHoverSeam(const QPoint &pos)
{
	int track = laneAtY(pos.y());
	qint64 best = -1;
	if (track >= 0 && track < model_.tracks.size() && !model_.tracks[track].hidden) {
		// A few pixels either side, not an exact hit: a 1px line is not
		// something anyone can put a cursor on, and the point is to confirm what
		// you are near rather than to select it.
		constexpr int kSeamHoverPx = 4;
		int bestDx = kSeamHoverPx + 1;
		for (const qint64 ms : model_.tracks[track].splitSeams()) {
			const int dx = std::abs(msToX(ms) - pos.x());
			if (dx <= kSeamHoverPx && dx < bestDx) {
				bestDx = dx;
				best = ms;
			}
		}
	}
	if (best < 0)
		track = -1;
	if (track == hoverSeamTrack_ && best == hoverSeamMs_)
		return;
	hoverSeamTrack_ = track;
	hoverSeamMs_ = best;
	update();
}

// The mark left where a clip was cut in two.
//
// Every clip boundary already shows a valley -- two rounded corners facing each
// other across the gap clipPaintRect leaves. That says "something ends here".
// This says something narrower and more useful: these two were ONE clip, and
// the cut is yours. Two unrelated pieces of footage that happen to abut get the
// valley and nothing else, so the two cases stop looking identical.
//
// Drawn after the clips and inside the same content clip, because it belongs to
// neither half -- the same reasoning as the transition band above it.
void TimelineView::drawSplitSeams(QPainter &p, int track) const
{
	const TlTrack &t = model_.tracks[track];
	if (t.hidden)
		return;
	const QVector<qint64> seams = t.splitSeams();
	if (seams.isEmpty())
		return;
	const QRect lane = laneRect(track);
	const QRect content = contentRect();
	const int y0 = lane.y() + 3, y1 = lane.bottom() - 3;

	for (const qint64 ms : seams) {
		const int x = msToX(ms);
		if (x < content.x() - 2 || x > content.right() + 2)
			continue;
		// Brighter while the pointer is near it, so hovering a seam confirms
		// which one you are about to trim rather than leaving you counting
		// pixels between two clips of the same colour.
		const bool near = hoverSeamTrack_ == track && hoverSeamMs_ == ms;
		QColor c = cl_.splitSeam;
		c.setAlpha(near ? 255 : 170);
		p.setPen(QPen(c, std::max(1, lp_.splitSeamW)));
		p.drawLine(x, y0, x, y1);
		// Caps: two short ticks that close the valley top and bottom, so the
		// seam reads as one mark rather than as a gap that happens to be lit.
		p.drawLine(x - 2, y0, x + 2, y0);
		p.drawLine(x - 2, y1, x + 2, y1);
	}
}

void TimelineView::removeSelectedTransition()
{
	if (!selTransition_ || selTrack_ < 0 || selTrack_ >= model_.tracks.size())
		return;
	TlTrack &t = model_.tracks[selTrack_];
	if (t.locked || selClip_ < 0 || selClip_ >= t.clips.size())
		return;
	const qint64 span = t.overlapBefore(selClip_);
	if (span <= 0)
		return;
	// The overlap IS the transition, so removing one means closing the overlap:
	// slide the incoming clip (and everything after it on this track) right by
	// the overlap, leaving the two butted together.
	const qint64 from = t.clips[selClip_].outStartMs;
	for (TlClip &c : t.clips)
		if (c.outStartMs >= from)
			c.outStartMs += span;
	selTransition_ = false;
	clampView();
	updateGeometry();
	update();
	emit selectionChanged(selTrack_, selClip_);
	commitEdit();
}

qint64 TimelineView::snap(qint64 ms, int ignoreTrack, int ignoreClip, bool *hit) const
{
	if (hit)
		*hit = false;
	if (!snap_)
		return ms;
	const qint64 tol = xToMs(contentRect().x() + lp_.snapPx) - xToMs(contentRect().x());
	qint64 best = ms;
	qint64 bestD = tol + 1;
	auto consider = [&](qint64 cand) {
		const qint64 d = std::llabs(cand - ms);
		if (d <= tol && d < bestD) {
			bestD = d;
			best = cand;
		}
	};
	consider(0);
	if (playheadMs_ >= 0)
		consider(playheadMs_);
	// Markers. A marker is a moment you noted ON PURPOSE -- the beat, the click,
	// the word -- so it is the strongest thing on the timeline to line something
	// up with. Until it was a candidate here it was decoration: you could see
	// the flag and drag a clip straight through it.
	for (const qint64 mk : model_.markers)
		consider(mk);
	// Every clip on every track is a candidate -- the magnet is deliberately
	// cross-track, so a cut on V2 can be lined up with one on V1.
	for (int ti = 0; ti < model_.tracks.size(); ++ti)
		for (int ci = 0; ci < model_.tracks[ti].clips.size(); ++ci) {
			if (ti == ignoreTrack && ci == ignoreClip)
				continue;
			// ...except the rest of a group being dragged. Those are moving by
			// the same delta, so their edges are not standing still and
			// snapping to one just pins the group to its own shape.
			if (mode_ == Mode::Move && dragStarts_.size() > 1 &&
			    dragStarts_.contains(qMakePair(ti, ci)))
				continue;
			const TlClip &cc = model_.tracks[ti].clips[ci];
			consider(cc.outStartMs);
			consider(cc.outEndMs());
			// Keyframes, of every kind. Lining a cut up with the moment a zoom
			// lands is the same job as lining it up with a marker.
			//
			// Not the keys of the clip whose OWN key is being dragged: those are
			// moving with the gesture (the one under the cursor is at the cursor,
			// so it would win every candidate and the key would never move), and
			// dropping one key onto another is a merge, not an alignment.
			if (mode_ == Mode::KeyDrag && ti == keyDrag_.track && ci == keyDrag_.clip)
				continue;
			forEachClipKeyTime(cc, [&](qint64 t) { consider(cc.outStartMs + t); });
		}
	if (hit)
		*hit = (bestD <= tol);
	return best;
}

void TimelineView::emitScrubAt(qint64 outMs)
{
	emit scrub(outMs);
	// Moving the playhead by hand is navigating too, and it is the moment you
	// most want to know which part of the project you are looking at.
	notifyView();
}

// ---- painting ---------------------------------------------------------------

QSize TimelineView::sizeHint() const
{
	return QSize(680, minimumSizeHint().height());
}

QSize TimelineView::minimumSizeHint() const
{
	int h = 2 * lp_.margin + lp_.rulerH;
	const int n = std::max(2, int(model_.tracks.size()));
	for (int i = 0; i < n; ++i)
		h += (i < model_.tracks.size() ? laneHeight(i) : lp_.videoLaneH) + lp_.laneGap;
	return QSize(320, h);
}

void TimelineView::drawRuler(QPainter &p) const
{
	const QRect c = contentRect();
	const QRect bar(c.x(), lp_.margin, c.width(), lp_.rulerH);
	p.fillRect(bar, cl_.gutter);
	const qint64 vis = visibleMs();
	static const qint64 kSteps[] = {200,   500,   1000,  2000,   5000,   10000,  15000, 30000,
					60000, 120000, 300000, 600000, 900000, 1800000, 3600000};
	const double msPerPx = double(vis) / std::max(1, c.width());
	const qint64 want = qint64(msPerPx * 80.0);
	qint64 step = kSteps[sizeof(kSteps) / sizeof(kSteps[0]) - 1];
	for (qint64 s : kSteps)
		if (s >= want) {
			step = s;
			break;
		}
	QFont f = p.font();
	f.setPixelSize(9);
	p.setFont(f);
	const qint64 first = (viewStart_ / step) * step;
	for (qint64 t = first; t <= viewStart_ + vis; t += step) {
		if (t < 0)
			continue;
		const int x = msToX(t);
		if (x < c.x() - 1 || x > c.right() + 1)
			continue;
		p.setPen(QColor(0xff, 0xff, 0xff, 40));
		p.drawLine(x, bar.top(), x, height() - lp_.margin); // faint gridline
		p.setPen(cl_.caption);
		p.drawText(x + 2, bar.bottom() - 4, timeTextRuler(t));
	}
}

void TimelineView::ensureFonts() const
{
	if (fontsForPx_ == lp_.segFontPx)
		return;
	fontsForPx_ = lp_.segFontPx;
	hdrFont_ = font();
	hdrFont_.setPixelSize(11);
	hdrFont_.setBold(true);
	toggleFont_ = font();
	toggleFont_.setPixelSize(11);
	toggleFont_.setBold(false);
	clipFont_ = font();
	clipFont_.setPixelSize(std::max(6, lp_.segFontPx));
}

namespace {
// Antialiasing, on only where something curved is being drawn.
//
// A timeline is overwhelmingly axis-aligned rectangles and text, and
// antialiasing costs a third of the whole paint whether or not there is a curve
// in it -- measured at 1600x420: 5.3 ms against 3.0 for a small project, 10.5
// against 6.9 for a dense one. That is paid on every mouse-move of every drag,
// which is the most interaction-sensitive path in the editor.
//
// Text is unaffected: Qt antialiases glyphs under the separate
// TextAntialiasing hint, which stays on.
struct AaOn {
	QPainter &p;
	explicit AaOn(QPainter &pp) : p(pp) { p.setRenderHint(QPainter::Antialiasing, true); }
	~AaOn() { p.setRenderHint(QPainter::Antialiasing, false); }
};
} // namespace

void TimelineView::drawClip(QPainter &p, int track, int clip) const
{
	const TlTrack &t = model_.tracks[track];
	const TlClip &c = t.clips[clip];
	const bool isText = c.type == TlClip::Type::Text;
	const bool isImage = c.type == TlClip::Type::Image;
	const bool isFx = c.type == TlClip::Type::Effect;
	const bool video = t.kind == TlTrack::Kind::Video && !isText && !isImage && !isFx;
	const bool sel = isSelected(track, clip);
	const QRect r = clipPaintRect(track, clip);
	// Scrolled-away clips cost the same as visible ones otherwise: the painter
	// clips the output, but the filmstrip tiling and the per-pixel waveform loop
	// below still run in full. On a long timeline that is most of the paint.
	const QRect content = contentRect();
	if (r.right() < content.x() || r.x() > content.right())
		return;
	QPainterPath path;
	path.addRoundedRect(r, lp_.clipRadius, lp_.clipRadius);
	p.setPen(Qt::NoPen);
	// Clips take their track's colour (selection brightens it); text clips get a
	// slight violet lean so they still read as captions.
	QColor fill = t.color.isValid() ? t.color
					: (t.kind == TlTrack::Kind::Video ? cl_.videoClip : cl_.audioClip);
	// An effect clip must not be mistakable for footage: it always takes the
	// palette's effect colour, ignoring the track tint, and a disabled one is
	// drained so "off" reads at a glance.
	if (isFx) {
		fill = cl_.effectClip;
		if (!c.fx.enabled)
			fill = QColor::fromHsv(fill.hue(), fill.saturation() / 3, fill.value() * 2 / 3);
	}
	if (isText)
		fill = QColor::fromHsv(fill.hue(), fill.saturation(), fill.value()).darker(105);
	if (sel)
		fill = fill.lighter(145);
	if (t.hidden || t.muted)
		fill = fill.darker(160);
	p.setBrush(fill);

	// A clip with nothing inside it — no filmstrip, no waveform, no keyframes,
	// no fades, too narrow for a label — is just a rounded rectangle, and zoomed
	// out that is most of them. Painting it as one call instead of a fill path,
	// a clip path and a border path is three rasterisations saved per clip.
	const bool dragging = (mode_ == Mode::Move && dragMoved_) || fileDrag_;
	// Below this a clip has no room for anything INSIDE it: a waveform, a
	// filmstrip tile or a keyframe pip drawn across a handful of pixels is
	// noise, and drawing it costs a QPainterPath, a clip-path change and a
	// save/restore per clip -- by far the most expensive thing in the paint.
	// A dense timeline is mostly clips this narrow.
	constexpr int kMinWidthForContents = 10;
	const bool roomInside = r.width() >= kMinWidthForContents;
	const bool wantsInside =
		isFx ||
		(roomInside && (((video || isImage) && srcThumbs_.contains(c.sourceId)) ||
				!c.peaks.isEmpty() ||
				(!dragging && clipHasKeys(c)) || clipTakesFades(track, clip))) ||
		(!dragging && r.width() >= 28);
	if (!wantsInside) {
		p.setPen(sel ? QPen(cl_.accent, 2) : QPen(cl_.border, 1));
		{
			AaOn aa(p);
			p.drawRoundedRect(r, lp_.clipRadius, lp_.clipRadius);
		}
		return;
	}

	{
		AaOn aa(p);
		p.drawPath(path);
	}

	p.save();
	// IntersectClip, not the default ReplaceClip. The caller has already clipped
	// the painter to the content area precisely so a clip starting left of the
	// visible range cannot paint over the track headers -- and replacing the
	// region here threw that away, which is how the filmstrip of a scrolled-off
	// clip ended up drawn across the gutter's name and L/H/M toggles.
	p.setClipPath(path, Qt::IntersectClip);
	if (video) {
		const auto it = srcThumbs_.constFind(c.sourceId);
		const qint64 sdur = srcThumbDur_.value(c.sourceId, 0);
		if (it != srcThumbs_.constEnd() && !it.value().isEmpty() && sdur > 0) {
			const QVector<QImage> &strip = it.value();
			const double aspect = srcAspect_.value(c.sourceId, 16.0 / 9.0);
			const int th = r.height() - 2;
			const int tileW = std::max(8, int(th * aspect));
			// Tiles are blitted from a cache of pre-scaled pixmaps. drawImage
			// with a target rect rescales the thumb on EVERY paint -- and this
			// widget paints 30 times a second during playback. The other two
			// timelines have cached their strips for a long time; this one,
			// the one with the most lanes, was the only one still rescaling.
			const QVector<QPixmap> &pix = scaledStrip(c.sourceId, strip, tileW, th);
			// Start at the first tile at or before the visible edge (tiles must stay
			// on their original grid, or they'd shift as the view scrolls).
			const int x0 = r.x() + 1;
			const int lastVis = std::min(r.right() - 1, content.right() + tileW + 1);
			// One tile per DISTINCT frame. Stepping by tile width repeated the
			// same thumbnail once a clip was zoomed past the strip's resolution;
			// this spaces them out instead, each at the moment it came from
			// (see Filmstrip.hpp).
			for (const StripTile &t :
			     filmstripTiles(int(strip.size()), sdur, c.srcStartMs, c.srcLenMs(), x0,
					    std::max(1, r.width()), tileW, 1, content.x(), lastVis)) {
				if (t.index < pix.size() && !pix[t.index].isNull())
					p.drawPixmap(QPoint(t.x, r.y() + 1), pix[t.index]);
			}
		}
	} else if (isImage) {
		// A still repeats across its clip, at the same tile size and aspect the
		// video filmstrip uses -- so a row of clips reads as one row whether the
		// pictures move or not. Repeated rather than stretched: stretching one
		// thumbnail across a long clip distorts it into a smear, and the clip's
		// LENGTH is a thing you read off the strip, which needs the tiles to be
		// a constant width whatever that length is.
		const auto it = srcThumbs_.constFind(c.sourceId);
		if (it != srcThumbs_.constEnd() && !it.value().isEmpty()) {
			const double aspect = srcAspect_.value(c.sourceId, 16.0 / 9.0);
			const int th = r.height() - 2;
			const int tileW = std::max(8, int(th * aspect));
			// Through the same pre-scaled pixmap cache: this widget repaints
			// 30 times a second during playback, and rescaling the still on
			// every one of those is exactly what that cache exists to avoid.
			const QVector<QPixmap> &pix = scaledStrip(c.sourceId, it.value(), tileW, th);
			if (!pix.isEmpty() && !pix.front().isNull()) {
				const int x0 = r.x() + 1;
				const int lastVis = std::min(r.right() - 1, content.right() + 1);
				// Start at the first tile at or before the visible edge, on
				// the clip's OWN grid -- tiles anchored to the viewport
				// would crawl sideways as the timeline scrolls.
				const int firstTile = std::max(0, (content.x() - x0) / tileW);
				for (int i = firstTile;; ++i) {
					const int x = x0 + i * tileW;
					if (x > lastVis)
						break;
					p.drawPixmap(QPoint(x, r.y() + 1), pix.front());
				}
			}
		}
	} else if (!c.peaks.isEmpty() && c.srcEndMs > 0) {
		p.setPen(QPen(cl_.waveform, 1));
		const int midY = r.center().y();
		const int halfH = r.height() / 2 - 3;
		const qint64 total = std::max<qint64>(1, srcThumbDur_.value(c.sourceId, c.srcEndMs));
		const int wx0 = std::max(r.left() + 1, content.x());
		const int wx1 = std::min(r.right() - 1, content.right() + 1);
		for (int x = wx0; x < wx1; ++x) {
			const double tt = double(x - r.left()) / std::max(1, r.width());
			const double srcFrac = double(c.srcStartMs + tt * c.srcLenMs()) / double(total);
			const int b = std::clamp<int>(int(srcFrac * c.peaks.size()), 0, c.peaks.size() - 1);
			const int hh = int(c.peaks[b] * halfH);
			p.drawLine(x, midY - hh, x, midY + hh);
		}
	}
	if (!dragging)
		drawKeyPips(p, track, clip);

	// Effect clip: a marker glyph and the effect's name, so what it does is
	// readable without selecting it. No filmstrip, no waveform -- it has neither.
	if (isFx) {
		p.setFont(clipFont_);
		p.setPen(c.fx.enabled ? QColor(0xf2, 0xf4, 0xf7) : cl_.caption);
		// Painted, not typed: the star this used to spell out is missing from
		// the default Windows UI font, so the badge showed a blank box.
		const int bs = std::min(11, r.height() - 6);
		if (bs > 4 && r.width() > bs + 12)
			paintGlyph(p, Glyph::Sparkle,
				   QRectF(r.x() + 5, r.center().y() - bs / 2.0, bs, bs),
				   c.fx.enabled ? QColor(0xf2, 0xf4, 0xf7) : cl_.caption);
		const int textX = (bs > 4 ? bs + 4 : 0);
		const QString label = QStringLiteral("%1%2").arg(effectClipLabel(c)).arg(
			c.fx.enabled ? QString() : QStringLiteral("  (off)"));
		p.setPen(c.fx.enabled ? QColor(0xf2, 0xf4, 0xf7) : cl_.caption);
		p.drawText(r.adjusted(5 + textX, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
			   p.fontMetrics().elidedText(label, Qt::ElideRight,
						      r.width() - 9 - textX));
		// A downward chevron on the lower edge: this reaches DOWN the stack.
		p.setPen(QPen(QColor(0xff, 0xff, 0xff, 90), 1));
		for (int x = r.x() + 6; x < r.right() - 4; x += 10) {
			p.drawLine(x, r.bottom() - 4, x + 3, r.bottom() - 1);
			p.drawLine(x + 3, r.bottom() - 1, x + 6, r.bottom() - 4);
		}
	}

	// Fade envelopes (audio clips): drawn over the waveform, under the label.
	if (clipTakesFades(track, clip))
		drawFades(p, track, clip);

	// Label bar along the bottom.
	if (!dragging && !isFx && r.width() >= 28) {
		p.setFont(clipFont_);
		p.fillRect(QRect(r.x(), r.bottom() - 12, r.width(), 13), QColor(0, 0, 0, 150));
		p.setPen(QColor(0xe6, 0xe6, 0xe6));
		QString what;
		if (isText)
			// The cased text, so the strip's label reads the way the
			// canvas does rather than showing what was typed.
			what = QStringLiteral("T  %1").arg(
				tlDisplayText(c.text).split(QLatin1Char('\n')).value(0));
		else if (isImage)
			what = QStringLiteral("IMG #%1").arg(c.sourceId);
		else
			what = QStringLiteral("#%1").arg(c.sourceId);
		const QString label =
			QStringLiteral("%1 · %2s").arg(what).arg(c.outDurationMs() / 1000.0, 0, 'f', 1);
		p.drawText(QRect(r.x() + 3, r.bottom() - 12, r.width() - 5, 13),
			   Qt::AlignVCenter | Qt::AlignLeft,
			   p.fontMetrics().elidedText(label, Qt::ElideRight, r.width() - 5));
	}
	p.restore();

	p.setPen(sel ? QPen(cl_.accent, 2) : QPen(cl_.border, 1));
	p.setBrush(Qt::NoBrush);
	AaOn aa(p);
	p.drawRoundedRect(r, lp_.clipRadius, lp_.clipRadius);
}

// ---- fade handles -----------------------------------------------------------

namespace {
// Below this the grips would be bigger than the clip and just get in the way,
// so a clip this narrow shows its envelope but no handles.
constexpr int kFadeMinClipW = 26;
constexpr int kFadeGrip = 9;   // drawn size
constexpr int kFadeGrab = 14;  // clickable half-width, so short clips stay usable
} // namespace

bool TimelineView::clipTakesFades(int track, int clip) const
{
	if (track < 0 || track >= model_.tracks.size())
		return false;
	const TlTrack &t = model_.tracks[track];
	if (t.kind != TlTrack::Kind::Audio || clip < 0 || clip >= t.clips.size())
		return false;
	// Captions and stills carry no sound to fade.
	return t.clips[clip].type == TlClip::Type::Video;
}

QRect TimelineView::fadeHandleRect(int track, int clip, FadeSide side) const
{
	if (side == FadeSide::None || !clipTakesFades(track, clip))
		return {};
	const TlClip &c = model_.tracks[track].clips[clip];
	const QRect r = clipRect(track, clip);
	if (r.width() < kFadeMinClipW)
		return {};
	const double perMs = double(r.width()) / double(std::max<qint64>(1, c.outDurationMs()));
	const int x = (side == FadeSide::In)
			      ? r.left() + int(std::lround(c.fadeInMs * perMs))
			      : r.right() - int(std::lround(c.fadeOutMs * perMs));
	const int y = r.top() + 1;
	return QRect(x - kFadeGrip / 2, y, kFadeGrip, kFadeGrip);
}

TimelineView::FadeHit TimelineView::fadeHandleAt(const QPoint &pt) const
{
	// One lane can hold the hit, so find it before walking any clips. Without
	// this the search is every clip in the project on every mouse-move, and
	// each clipRect() underneath used to cost an O(clips) span scan of its own
	// -- 1200 clips meant well over a million operations per pointer motion.
	const SpanGuard span(this);
	const int hitLane = laneAtY(pt.y());
	if (hitLane < 0)
		return {};
	for (int ti = hitLane; ti <= hitLane; ++ti) {
		if (model_.tracks[ti].locked)
			continue;
		for (int ci = 0; ci < model_.tracks[ti].clips.size(); ++ci) {
			if (!clipTakesFades(ti, ci))
				continue;
			const QRect r = clipRect(ti, ci);
			// Only the top band of the clip belongs to the grips; the rest
			// still moves and trims the clip as before.
			if (pt.y() < r.top() || pt.y() > r.top() + kFadeGrab)
				continue;
			for (const FadeSide s : {FadeSide::In, FadeSide::Out}) {
				const QRect h = fadeHandleRect(ti, ci, s);
				if (h.isNull())
					continue;
				if (std::abs(pt.x() - h.center().x()) <= kFadeGrab / 2)
					return FadeHit{ti, ci, s};
			}
		}
	}
	return {};
}

namespace {
// The pip's own geometry. Grab is generous against draw, for the same reason
// the fade grip is: a marker you can see but cannot reliably hit is worse than
// no marker, and these sit in the top few pixels of a clip that is also
// draggable.
constexpr int kPipDraw = 4;   // half-width as drawn
constexpr int kPipGrabX = 6;  // half-width you can click
constexpr int kPipBandY = 11; // how far down the clip the pip band reaches
constexpr int kPipMinClipW = 12;
} // namespace

QPoint TimelineView::keyPipCenter(const QRect &r, qint64 tMs, qint64 durMs) const
{
	const double f = durMs > 0 ? std::clamp(double(tMs) / double(durMs), 0.0, 1.0) : 0.0;
	return QPoint(r.x() + int(f * r.width()), r.y() + 5);
}

void TimelineView::drawKeyPips(QPainter &p, int track, int clip) const
{
	const TlClip &c = model_.tracks[track].clips[clip];
	const qint64 dur = c.outDurationMs();
	if (dur <= 0)
		return;
	const QRect r = clipRect(track, clip);
	if (r.width() < kPipMinClipW)
		return;
	keyTimes_ = clipKeyTimes(c);
	if (keyTimes_.isEmpty())
		return;

	p.setPen(Qt::NoPen);
	const AaOn aa(p);
	for (const qint64 t : keyTimes_) {
		const QPoint ctr = keyPipCenter(r, t, dur);
		// The one being dragged is drawn brighter and a size up, so the pip
		// under the cursor is unmistakably the one that is moving -- on a dense
		// clip the neighbours are only a few pixels away.
		const KeyHit me{track, clip, t};
		const bool held = (keyDrag_.valid() && keyDrag_ == me) ||
				  (keyHover_.valid() && keyHover_ == me);
		const int h = held ? kPipDraw + 2 : kPipDraw;
		p.setBrush(held ? QColor(0xff, 0xff, 0xff) : QColor(0xff, 0xd4, 0x4f));
		QPainterPath d;
		d.moveTo(ctr.x(), ctr.y() - h);
		d.lineTo(ctr.x() + h, ctr.y());
		d.lineTo(ctr.x(), ctr.y() + h);
		d.lineTo(ctr.x() - h, ctr.y());
		d.closeSubpath();
		p.drawPath(d);
	}
}

TimelineView::KeyHit TimelineView::keyPipAt(const QPoint &pt) const
{
	// Same shape as fadeHandleAt: one lane can hold the hit, so the search is
	// that lane's clips rather than the project's.
	const SpanGuard span(this);
	const int lane = laneAtY(pt.y());
	if (lane < 0)
		return {};
	for (int ci = 0; ci < model_.tracks[lane].clips.size(); ++ci) {
		const TlClip &c = model_.tracks[lane].clips[ci];
		const qint64 dur = c.outDurationMs();
		if (dur <= 0)
			continue;
		const QRect r = clipRect(lane, ci);
		if (r.width() < kPipMinClipW)
			continue;
		// Only the top band belongs to the pips; everything below still moves
		// and trims the clip exactly as it did.
		if (pt.y() < r.top() || pt.y() > r.top() + kPipBandY)
			continue;
		if (pt.x() < r.left() - kPipGrabX || pt.x() > r.right() + kPipGrabX)
			continue;
		KeyHit best;
		int bestD = kPipGrabX + 1;
		forEachClipKeyTime(c, [&](qint64 t) {
			const int d = std::abs(pt.x() - keyPipCenter(r, t, dur).x());
			// `<` not `<=`: two keys the same distance away means the earlier
			// one wins, which is stable rather than dependent on the order the
			// four key stores happen to be visited in.
			if (d <= kPipGrabX && d < bestD) {
				bestD = d;
				best = KeyHit{lane, ci, t};
			}
		});
		if (best.valid())
			return best;
	}
	return {};
}

QPoint TimelineView::keyPipCenterForTest(int track, int clip, qint64 tMs) const
{
	if (track < 0 || track >= model_.tracks.size() || clip < 0 ||
	    clip >= model_.tracks[track].clips.size())
		return {};
	const TlClip &c = model_.tracks[track].clips[clip];
	return keyPipCenter(clipRect(track, clip), tMs, c.outDurationMs());
}

bool TimelineView::keyPipHitForTest(const QPoint &p, int *track, int *clip, qint64 *tMs) const
{
	const KeyHit h = keyPipAt(p);
	if (track)
		*track = h.track;
	if (clip)
		*clip = h.clip;
	if (tMs)
		*tMs = h.tMs;
	return h.valid();
}

int TimelineView::fadeMsForX(const TlClip &c, const QRect &r, int x, FadeSide side,
			     bool fine) const
{
	const qint64 dur = c.outDurationMs();
	const double msPerPx = double(dur) / double(std::max(1, r.width()));
	double ms = (side == FadeSide::In) ? (x - r.left()) * msPerPx : (r.right() - x) * msPerPx;
	// Shift is the "let me place it exactly" modifier, matching the rest of the
	// timeline; otherwise a fade lands on a frame like every other edit.
	if (!fine && snap_) {
		const double frame = 1000.0 / (fps_ > 1.0 ? fps_ : 30.0);
		ms = std::round(ms / frame) * frame;
	}
	return int(std::clamp<double>(ms, 0.0, double(dur)));
}

void TimelineView::drawFades(QPainter &p, int track, int clip) const
{
	const TlClip &c = model_.tracks[track].clips[clip];
	if (c.fadeInMs <= 0 && c.fadeOutMs <= 0 && !(fadeHover_.track == track && fadeHover_.clip == clip))
		return;
	const QRect r = clipRect(track, clip);
	if (r.width() < 6)
		return;
	const double perMs = double(r.width()) / double(std::max<qint64>(1, c.outDurationMs()));

	const QColor line = cl_.fade;
	const QColor wash(0x00, 0x00, 0x00, 110);

	auto envelope = [&](int fadeMs, FadeCurve curve, bool in) {
		if (fadeMs <= 0)
			return;
		const int w = std::max(1, int(std::lround(fadeMs * perMs)));
		const int x0 = in ? r.left() : r.right() - w;
		// Zoomed far out a sampled curve is a waste of segments and reads as a
		// smudge — a single straight line says the same thing.
		const int steps = (w < 18) ? 1 : std::min(w, 48);
		QPolygonF curvePts;
		for (int i = 0; i <= steps; ++i) {
			const double t = double(i) / steps;
			const double g = fadeGain(curve, in ? t : 1.0 - t);
			curvePts << QPointF(x0 + t * w, r.bottom() - g * (r.height() - 2));
		}
		QPolygonF filled = curvePts;
		filled << QPointF(x0 + w, r.top()) << QPointF(x0, r.top());
		p.setPen(Qt::NoPen);
		p.setBrush(wash);
		AaOn aa(p);
		p.drawPolygon(filled);
		p.setPen(QPen(line, 2));
		p.setBrush(Qt::NoBrush);
		p.drawPolyline(curvePts);
	};
	envelope(c.fadeInMs, c.fadeInCurve, true);
	envelope(c.fadeOutMs, c.fadeOutCurve, false);

	if (r.width() < kFadeMinClipW)
		return;
	for (const FadeSide s : {FadeSide::In, FadeSide::Out}) {
		const QRect h = fadeHandleRect(track, clip, s);
		if (h.isNull())
			continue;
		const FadeHit me{track, clip, s};
		const bool hot = (fadeDrag_ == me) || (fadeHover_ == me);
		p.setPen(QPen(QColor(0x20, 0x20, 0x20), 1));
		p.setBrush(hot ? QColor(0xff, 0xff, 0xff) : line);
		AaOn aa(p);
		p.drawEllipse(hot ? h.adjusted(-1, -1, 1, 1) : h);
	}
}

void TimelineView::paintEvent(QPaintEvent *)
{
	// Measure the axis once for the whole paint instead of once per msToX call.
	const SpanGuard span(this);
	QPainter p(this);
	// Off by default; AaOn turns it on around the curved drawing. See above.
	p.setRenderHint(QPainter::Antialiasing, false);
	p.fillRect(rect(), cl_.timelineBg);

	clampView();
	viewTarget_ = viewStart_;
	ensureFonts();

	const bool dragging = (mode_ == Mode::Move && dragMoved_) || fileDrag_;

	// Lanes + headers.
	for (int i = 0; i < model_.tracks.size(); ++i) {
		const TlTrack &t = model_.tracks[i];
		const QRect lane = laneRect(i);
		p.setPen(Qt::NoPen);
		// Highlight the lane a dragged clip would land on.
		const bool dropHere = dragging && drop_.track == i;
		p.setBrush(dropHere ? QColor(cl_.accent.red(), cl_.accent.green(), cl_.accent.blue(), 40)
				    : ((i % 2) ? cl_.laneAlt : cl_.lane));
		p.drawRect(lane);
		if (t.locked) { // faint hatch so a locked lane reads as untouchable
			p.setBrush(QBrush(QColor(0xff, 0xff, 0xff, 10), Qt::BDiagPattern));
			p.drawRect(lane);
		}

		const QRect hdr = trackHeaderRect(i);
		p.setBrush(cl_.gutter);
		p.drawRect(hdr);
		// Colour chip down the left edge of the header.
		p.setBrush(t.color);
		p.drawRect(QRect(hdr.x(), hdr.y() + 1, 4, hdr.height() - 2));
		// A selected header, so Ctrl+C has something visible to act on. Without
		// a mark, "copy the track" would be a shortcut whose target the user has
		// to remember rather than see.
		if (i == selHeaderTrack_) {
			p.setBrush(Qt::NoBrush);
			p.setPen(QPen(cl_.accent, 2));
			p.drawRect(hdr.adjusted(1, 1, -1, -1));
			p.setPen(Qt::NoPen);
		}

		p.setPen((t.hidden || t.muted) ? cl_.caption : QColor(0xe8, 0xea, 0xed));
		p.setFont(hdrFont_);
		p.drawText(hdr.adjusted(10, 2, -4, 0), Qt::AlignTop | Qt::AlignLeft, t.name);
		// An effect track acts DOWNWARDS, on the tracks under it. That is easy
		// to state and hard to remember, especially since index 0 is the top
		// lane — so the header says which way it points instead.
		if (t.kind == TlTrack::Kind::Effect) {
			const int as = 9;
			paintGlyph(p, Glyph::ArrowDown,
				   QRectF(hdr.right() - as - 6, hdr.center().y() - as / 2.0, as, as),
				   t.hidden ? cl_.caption : cl_.effectClip);
		}

		// Lock / hide / mute toggles (hidden while dragging to cut clutter).
		if (!dragging) {
			p.setFont(toggleFont_);
			auto drawToggle = [&](HeaderHit which, const QString &glyph, bool on) {
				const QRect r = headerToggleRect(i, which);
				if (r.isEmpty())
					return;
				p.setPen(Qt::NoPen);
				p.setBrush(on ? cl_.accent : QColor(0x2b, 0x2f, 0x36));
				{
					AaOn aa(p);
					p.drawRoundedRect(r, 3, 3);
				}
				p.setPen(on ? QColor(0xff, 0xff, 0xff) : cl_.caption);
				p.drawText(r, Qt::AlignCenter, glyph);
			};
			drawToggle(HeaderHit::Lock, QStringLiteral("L"), t.locked);
			if (t.kind == TlTrack::Kind::Video)
				drawToggle(HeaderHit::Hide, QStringLiteral("H"), t.hidden);
			drawToggle(HeaderHit::Mute, QStringLiteral("M"), t.muted);
		}

		// Clips are painted after the header, and a clip that starts before the
		// visible range has an x inside the gutter — so without this it draws
		// straight over the track's name and its L/H/M toggles.
		p.save();
		p.setClipRect(contentRect());
		for (int ci = 0; ci < t.clips.size(); ++ci)
			drawClip(p, i, ci);
		drawSplitSeams(p, i);
		p.restore();

		// Overlaps last, over both clips: the band has to read as belonging to
		// neither of them.
		if (t.kind == TlTrack::Kind::Video) {
			const QVector<qint64> spans = t.overlapsBefore();
			for (int ci = 0; ci < t.clips.size(); ++ci)
				if (spans[ci] > 0 && t.clips[ci].transition.enabled)
					drawTransition(p, i, ci, spans[ci]);
		}
	}

	// While an effect track is selected, wash the lanes it covers. Saying
	// "grades every track below" is one thing; showing which ones is another,
	// and it settles the question the moment you move the track.
	if (selTrack_ >= 0 && selTrack_ < model_.tracks.size() &&
	    model_.tracks[selTrack_].kind == TlTrack::Kind::Effect &&
	    !model_.tracks[selTrack_].hidden) {
		p.save();
		p.setClipRect(contentRect());
		p.setPen(Qt::NoPen);
		QColor wash = cl_.effectClip;
		wash.setAlpha(26);
		for (int i = selTrack_ + 1; i < model_.tracks.size(); ++i) {
			// It grades the picture only; audio below it is untouched.
			if (!TimelineModel::isPictureKind(model_.tracks[i].kind))
				continue;
			p.setBrush(wash);
			p.drawRect(laneRect(i));
		}
		p.restore();
	}

	// "Release here to make a new track" indicator.
	if (dragging && drop_.newTrackAt >= 0) {
		const QRect c = contentRect();
		const int y = insertYFor(drop_.newTrackAt);
		p.setPen(QPen(cl_.accent, 3));
		p.drawLine(c.x(), y, c.right(), y);
		p.setPen(Qt::NoPen);
		p.setBrush(cl_.accent);
		const QRect tag(c.x() + 6, y - 9, 104, 18);
		{
			AaOn aa(p);
			p.drawRoundedRect(tag, 3, 3);
		}
		QFont tf = p.font();
		tf.setPixelSize(10);
		tf.setBold(true);
		p.setFont(tf);
		p.setPen(QColor(0xff, 0xff, 0xff));
		p.drawText(tag, Qt::AlignCenter, QStringLiteral("+ New track here"));
	}

	// Magnet guide: while a drag is being held on a snap candidate, show the
	// line it is stuck to. Without it the magnet is invisible — the clip just
	// refuses to follow the cursor for a few pixels and reads as lag.
	// (`dragging` above is move-only; a trim gets the guide too.)
	if (dragMoved_ && snapLineMs_ >= 0) {
		const int sx = msToX(snapLineMs_);
		const QRect c = contentRect();
		if (sx >= c.x() - 1 && sx <= c.right() + 1) {
			p.save();
			p.setClipRect(c);
			p.setPen(QPen(cl_.snapGuide, 1));
			p.drawLine(sx, c.y(), sx, c.bottom());
			// Two small nubs, so the line reads as a magnet rather than
			// as another playhead.
			p.setPen(Qt::NoPen);
			p.setBrush(cl_.snapGuide);
			p.drawRect(QRect(sx - 2, c.y(), 5, 3));
			p.drawRect(QRect(sx - 2, c.bottom() - 2, 5, 3));
			p.restore();
		}
	}

	if (model_.tracks.isEmpty()) {
		p.setPen(cl_.caption);
		p.drawText(contentRect(), Qt::AlignCenter,
			   QStringLiteral("Add a source to the timeline to start editing."));
	}

	drawRuler(p);

	// Project markers: a flag on the ruler and a faint line down the lanes, so a
	// noted moment stays findable while scrolling.
	for (const qint64 mk : model_.markers) {
		const int mx = msToX(mk);
		const QRect c = contentRect();
		if (mx < c.x() - 1 || mx > c.right() + 1)
			continue;
		p.setPen(QPen(cl_.marker, 1, Qt::DotLine));
		p.drawLine(mx, lp_.margin + lp_.rulerH, mx, height() - lp_.margin);
		p.setPen(Qt::NoPen);
		p.setBrush(cl_.marker);
		QPainterPath flag;
		flag.moveTo(mx, lp_.margin + 2);
		flag.lineTo(mx + 9, lp_.margin + 6);
		flag.lineTo(mx, lp_.margin + 10);
		flag.closeSubpath();
		AaOn aa(p);
		p.drawPath(flag);
	}

	// Hover marker: while the pointer is over a clip the preview follows it
	// rather than the playhead, which is otherwise invisible and reads as the
	// preview having jumped on its own. Dashed and amber so it can't be mistaken
	// for the playhead — an edit still lands at the playhead, not here.
	if (hoverMs_ >= 0 && !dragging) {
		const int hx = msToX(hoverMs_);
		const QRect c = contentRect();
		if (hx >= c.x() - 1 && hx <= c.right() + 1) {
			QPen hp(cl_.hover, 1, Qt::DashLine);
			p.setPen(hp);
			p.drawLine(hx, lp_.margin, hx, height() - lp_.margin);

			const QString lab = timeTextCentis(hoverMs_);
			p.setFont(toggleFont_);
			const int tw = p.fontMetrics().horizontalAdvance(lab) + 8;
			// Flip the tag to the left near the right edge so it stays readable.
			const bool flip = hx + tw + 2 > c.right();
			const QRect tag(flip ? hx - tw - 2 : hx + 2, lp_.margin + 1, tw, 14);
			p.setPen(Qt::NoPen);
			p.setBrush(cl_.hover);
			{
				AaOn aa(p);
				p.drawRoundedRect(tag, 2, 2);
			}
			p.setPen(QColor(0x15, 0x17, 0x1a));
			p.drawText(tag, Qt::AlignCenter, lab);
		}
	}

	// Playhead across all lanes.
	if (playheadMs_ >= 0) {
		const int x = msToX(playheadMs_);
		if (x >= contentRect().x() - 1 && x <= contentRect().right() + 1) {
			p.setPen(QPen(cl_.playhead, 2));
			p.drawLine(x, lp_.margin, x, height() - lp_.margin);
		}
	}

	// Left gutter separator.
	p.setPen(cl_.border);
	p.drawLine(lp_.margin + gutterWidth(), lp_.margin, lp_.margin + gutterWidth(),
		   height() - lp_.margin);
}

// ---- interaction ------------------------------------------------------------

void TimelineView::mousePressEvent(QMouseEvent *e)
{
	// Same reason as the hover pass: a press hit-tests several times over, and
	// all of it happens before anything in the model moves.
	const SpanGuard span(this);
	const QPoint pos = e->pos();
	setFocus();
	// The hover marker tracks the preview, which stops following the pointer the
	// moment a drag starts — leaving it drawn would point at nothing.
	//
	// And the hover is genuinely OVER: say so, the same as leaving the widget
	// does. Anything following the hovered frame rather than the playhead --
	// the preview, and the Inspector's read-only readout of it -- has to be
	// released here, or a press leaves it describing whichever frame the
	// pointer last crossed for as long as the drag lasts.
	const bool wasHovering = hoverMs_ >= 0;
	hoverMs_ = -1;
	if (wasHovering)
		emit hoverScrubEnded();

	// Grab the timeline and slide it. Tested first, before the ruler and before
	// anything under the pointer, because it has to work from anywhere: a pan
	// that stops working over a clip is a pan you cannot trust.
	//
	// The middle button, because left is already spoken for twice over (empty
	// space scrubs, a clip moves) and taking either would trade one navigation
	// gesture for another. Alt+left does the same thing for mice and trackpads
	// with no middle button.
	if (canPanFrom(e) && pos.x() >= contentRect().x()) {
		mode_ = Mode::Pan;
		panStartX_ = pos.x();
		panStartView_ = viewStart_;
		setCursor(Qt::ClosedHandCursor);
		return;
	}

	// Ruler or gutter → scrub / nothing.
	if (pos.y() < lp_.margin + lp_.rulerH && pos.x() >= contentRect().x()) {
		mode_ = Mode::Scrub;
		playheadMs_ = xToMs(pos.x());
		emitScrubAt(playheadMs_);
		update();
		return;
	}
	// ---- Track header (gutter): toggles + track menu ----
	if (pos.x() < contentRect().x()) {
		const int hTrack = laneAtY(pos.y());
		if (hTrack < 0)
			return;
		if (e->button() == Qt::RightButton) {
			showTrackMenu(hTrack, e->globalPosition().toPoint(), playheadMs_);
			return;
		}
		if (e->button() != Qt::LeftButton)
			return;
		TlTrack &ht = model_.tracks[hTrack];
		switch (headerHitAt(hTrack, pos)) {
		case HeaderHit::Lock:
			ht.locked = !ht.locked;
			break;
		case HeaderHit::Hide:
			ht.hidden = !ht.hidden;
			break;
		case HeaderHit::Mute:
			ht.muted = !ht.muted;
			break;
		case HeaderHit::None:
			// The header itself, away from the toggles: select the TRACK. That
			// is what gives Ctrl+C a whole track to copy, and it clears the clip
			// selection so the shortcut is never ambiguous about which of the
			// two it means -- one selection is live at a time, and you can see
			// which.
			selHeaderTrack_ = hTrack;
			selTrack_ = selClip_ = -1;
			selTransition_ = false;
			extraSel_.clear();
			emit selectionChanged(-1, -1);
			update();
			return;
		}
		update();
		commitEdit(); // repaints the preview + records an undo step
		return;
	}

	// Anything in the lanes is a clip gesture, so the header selection lets go.
	selHeaderTrack_ = -1;

	int track = -1;
	const int clip = clipAtPoint(pos, &track);

	if (e->button() == Qt::RightButton) {
		if (const KeyHit kh = keyPipAt(pos); kh.valid()) {
			showKeyMenu(kh, e->globalPosition().toPoint());
			return;
		}
		if (clip >= 0) {
			selTrack_ = track;
			selClip_ = clip;
			emit selectionChanged(selTrack_, selClip_);
			update();
			showClipMenu(track, clip, e->globalPosition().toPoint(), xToMs(pos.x()));
		} else if (track >= 0) {
			// Empty space on a lane used to do nothing on right-click. The
			// track's menu is what you want there -- and on an effect track
			// it is how you add an effect where you clicked.
			showTrackMenu(track, e->globalPosition().toPoint(), xToMs(pos.x()));
		}
		return;
	}
	if (e->button() != Qt::LeftButton)
		return;

	// An overlap belongs to neither clip: clicking it selects the TRANSITION.
	// Ctrl-click still falls through to the clips, so a group selection can
	// still be built across an overlap.
	if (!(e->modifiers() & Qt::ControlModifier)) {
		int trTrack = -1;
		const int trIncoming = transitionAtPoint(pos, &trTrack);
		if (trIncoming >= 0) {
			selTrack_ = trTrack;
			selClip_ = trIncoming;
			selTransition_ = true;
			extraSel_.clear();
			mode_ = Mode::None;
			emit selectionChanged(selTrack_, selClip_);
			emitScrubAt(xToMs(pos.x()));
			update();
			return;
		}
	}

	// A keyframe pip wins over the clip under it, for the same reason a fade
	// grip does: it is drawn inside the clip, and without this the only thing
	// the top edge could do was start a trim.
	if (const KeyHit kh = keyPipAt(pos); kh.valid() && !model_.tracks[kh.track].locked) {
		keyDrag_ = kh;
		keyDragFrom_ = kh.tMs;
		mode_ = Mode::KeyDrag;
		pressPos_ = pos;
		dragMoved_ = false;
		snapLineMs_ = -1;
		dragTrack_ = kh.track;
		dragClip_ = kh.clip;
		// Selecting the clip too: the Inspector is where the key's VALUES are,
		// and grabbing a key without its clip selected shows you a moving
		// diamond and no numbers.
		if (!isSelected(kh.track, kh.clip)) {
			extraSel_.clear();
			selTrack_ = kh.track;
			selClip_ = kh.clip;
			emit selectionChanged(selTrack_, selClip_);
		}
		update();
		return;
	}

	// A fade grip wins over the clip under it — it sits inside the clip, and
	// the top corners are also where a trim would otherwise start.
	if (const FadeHit fh = fadeHandleAt(pos); fh.valid()) {
		fadeDrag_ = fh;
		mode_ = Mode::Fade;
		pressPos_ = pos;
		dragMoved_ = false;
		dragTrack_ = fh.track;
		dragClip_ = fh.clip;
		selTrack_ = fh.track;
		selClip_ = fh.clip;
		extraSel_.clear();
		emit selectionChanged(selTrack_, selClip_);
		update();
		return;
	}

	selTransition_ = false; // anything else selects a clip, not a transition

	if (clip < 0) {
		// Empty area: move the playhead + scrub.
		mode_ = Mode::Scrub;
		playheadMs_ = xToMs(pos.x());
		extraSel_.clear();
		if (selTrack_ != -1 || selClip_ != -1) {
			selTrack_ = selClip_ = -1;
			emit selectionChanged(-1, -1);
		}
		emitScrubAt(playheadMs_);
		update();
		return;
	}

	// Ctrl-click adds to (or removes from) the selection instead of replacing it.
	if (e->modifiers() & Qt::ControlModifier) {
		if (track == selTrack_ && clip == selClip_) {
			// Dropping the primary promotes one of the extras, so a selection
			// never ends up with members but no primary.
			selTrack_ = selClip_ = -1;
			if (!extraSel_.isEmpty()) {
				const auto it = extraSel_.constBegin();
				selTrack_ = it->first;
				selClip_ = it->second;
				extraSel_.erase(extraSel_.constBegin());
			}
		} else if (extraSel_.contains({track, clip})) {
			extraSel_.remove({track, clip});
		} else {
			if (selTrack_ >= 0)
				extraSel_.insert({selTrack_, selClip_});
			selTrack_ = track;
			selClip_ = clip;
		}
		mode_ = Mode::None;
		emit selectionChanged(selTrack_, selClip_);
		update();
		return;
	}
	// A plain click on something already selected keeps the group, so a
	// multi-selection can be dragged; otherwise it replaces the selection.
	if (!isSelected(track, clip))
		extraSel_.clear();
	selTrack_ = track;
	selClip_ = clip;
	emit selectionChanged(selTrack_, selClip_);
	// A locked track still selects (so you can inspect it) but never edits.
	if (model_.tracks[track].locked) {
		mode_ = Mode::None;
		emitScrubAt(xToMs(pos.x()));
		update();
		return;
	}
	pressPos_ = pos;
	dragMoved_ = false;
	snapLineMs_ = -1;
	dragTrack_ = track;
	dragClip_ = clip;
	dragOrig_ = model_.tracks[track].clips[clip];
	const QRect r = clipRect(track, clip);
	dragSrcPerPx_ = double(dragOrig_.srcLenMs()) / double(std::max(1, r.width()));
	dragGrabOffsetMs_ = xToMs(pos.x()) - dragOrig_.outStartMs;
	// A group drag moves everything by the same amount, so each member's
	// starting position has to be remembered before the first delta is applied.
	dragStarts_.clear();
	for (const auto &sp : selectedPairs())
		dragStarts_.insert(sp, model_.tracks[sp.first].clips[sp.second].outStartMs);
	const int edge = std::min(8, r.width() / 3);
	if (pos.x() - r.left() <= edge)
		mode_ = Mode::ResizeLeft;
	else if (r.right() - pos.x() <= edge)
		mode_ = Mode::ResizeRight;
	else
		mode_ = Mode::Move;
	emitScrubAt(xToMs(pos.x()));
	update();
}

void TimelineView::mouseMoveEvent(QMouseEvent *e)
{
	const QPoint pos = e->pos();

	if (mode_ == Mode::Pan) {
		// Against the pose at the press, so the grabbed instant stays under the
		// pointer for the whole gesture however far it travels. Note the sign:
		// dragging RIGHT shows earlier material, the way a hand on a map does.
		const QRect c = contentRect();
		if (c.width() > 0) {
			const double msPerPx = double(visibleMs()) / double(c.width());
			viewStart_ = panStartView_ -
				     qint64(std::llround((pos.x() - panStartX_) * msPerPx));
			clampView();
			update();
			notifyView();
		}
		return;
	}
	if (mode_ == Mode::Scrub) {
		playheadMs_ = xToMs(pos.x());
		emitScrubAt(playheadMs_);
		update();
		return;
	}

	if (mode_ == Mode::KeyDrag && keyDrag_.valid() && (e->buttons() & Qt::LeftButton)) {
		if (!dragMoved_ && (pos - pressPos_).manhattanLength() > 3)
			dragMoved_ = true;
		if (!dragMoved_)
			return;
		if (keyDrag_.track >= model_.tracks.size() ||
		    keyDrag_.clip >= model_.tracks[keyDrag_.track].clips.size())
			return;
		TlClip &c = model_.tracks[keyDrag_.track].clips[keyDrag_.clip];
		// The magnet works in OUTPUT time, where the markers, the playhead and
		// every other clip's edges are; the key is stored clip-relative, so the
		// conversion happens here rather than in three places inside snap().
		const bool fine = e->modifiers() & Qt::ShiftModifier;
		bool hit = false;
		qint64 wantOut = xToMs(pos.x());
		if (!fine)
			wantOut = snap(wantOut, -1, -1, &hit);
		const qint64 want = std::clamp<qint64>(wantOut - c.outStartMs, 0, c.outDurationMs());
		if (retimeClipKeys(c, keyDrag_.tMs, want)) {
			keyDrag_.tMs = want;
			snapLineMs_ = hit ? c.outStartMs + want : -1;
			// Live, like a fade drag: the preview and the Inspector's key list
			// follow the diamond instead of jumping when it is dropped.
			emit clipsChanged();
			emit selectionChanged(keyDrag_.track, keyDrag_.clip);
		}
		QToolTip::showText(e->globalPosition().toPoint(),
				   QStringLiteral("%1 s").arg(keyDrag_.tMs / 1000.0, 0, 'f', 2), this);
		update();
		return;
	}

	if (mode_ == Mode::Fade && fadeDrag_.valid() && (e->buttons() & Qt::LeftButton)) {
		dragMoved_ = true;
		TlTrack &t = model_.tracks[fadeDrag_.track];
		if (fadeDrag_.clip < t.clips.size()) {
			TlClip &c = t.clips[fadeDrag_.clip];
			const QRect r = clipRect(fadeDrag_.track, fadeDrag_.clip);
			const bool fine = e->modifiers() & Qt::ShiftModifier;
			const int ms = fadeMsForX(c, r, pos.x(), fadeDrag_.side, fine);
			if (fadeDrag_.side == FadeSide::In)
				c.fadeInMs = ms;
			else
				c.fadeOutMs = ms;
			QToolTip::showText(e->globalPosition().toPoint(),
					   QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 2), this);
			update();
			// The Inspector's fade fields follow the drag.
			emit selectionChanged(fadeDrag_.track, fadeDrag_.clip);
		}
		return;
	}

	if (mode_ != Mode::None && dragTrack_ >= 0 && (e->buttons() & Qt::LeftButton)) {
		if (!dragMoved_ && (pos - pressPos_).manhattanLength() > 4)
			dragMoved_ = true;
		if (!dragMoved_)
			return;
		TlClip &c = model_.tracks[dragTrack_].clips[dragClip_];
		// Media clips can't be trimmed past their source; text clips have no
		// source, so they stretch freely.
		const qint64 srcTotal =
			dragOrig_.freeDuration()
				? std::numeric_limits<qint64>::max() / 4
				: srcThumbDur_.value(dragOrig_.sourceId, dragOrig_.srcEndMs + (1 << 30));

		if (mode_ == Mode::Move) {
			qint64 ns = std::max<qint64>(0, xToMs(pos.x()) - dragGrabOffsetMs_);
			const qint64 dur = c.outDurationMs();
			// Try both edges and take whichever actually caught something.
			//
			// Comparing the two distances alone does NOT work: when an edge
			// finds nothing, snap() hands back the position unchanged, i.e. a
			// distance of zero -- which beats every real candidate. That is why
			// dragging a clip's START up against another clip's END did nothing
			// unless its far edge happened to find a candidate too.
			bool hitStart = false, hitEnd = false;
			const qint64 snapStart = snap(ns, dragTrack_, dragClip_, &hitStart);
			const qint64 snapEnd = snap(ns + dur, dragTrack_, dragClip_, &hitEnd) - dur;
			bool useEnd = false, hit = hitStart || hitEnd;
			if (hitStart && hitEnd)
				useEnd = std::llabs(snapEnd - ns) < std::llabs(snapStart - ns);
			else if (hitEnd)
				useEnd = true;
			if (hit)
				ns = useEnd ? snapEnd : snapStart;
			c.outStartMs = std::max<qint64>(0, ns);
			// Only claim a snap if the clamp to 0 didn't move the edge away again.
			snapLineMs_ = (hit && c.outStartMs == ns)
					      ? (useEnd ? c.outStartMs + dur : c.outStartMs)
					      : -1;
			// Carry the rest of the selection along by the same delta. Only the
			// clip under the cursor changes lane; the others keep theirs, which
			// keeps a cross-track group predictable.
			if (dragStarts_.size() > 1) {
				const qint64 delta = c.outStartMs - dragOrig_.outStartMs;
				for (auto it = dragStarts_.constBegin(); it != dragStarts_.constEnd(); ++it) {
					if (it.key() == qMakePair(dragTrack_, dragClip_))
						continue;
					TlTrack &ot = model_.tracks[it.key().first];
					if (ot.locked || it.key().second >= ot.clips.size())
						continue;
					ot.clips[it.key().second].outStartMs =
						std::max<qint64>(0, it.value() + delta);
				}
			}
			// Vertical position picks the landing lane — or, past a lane edge,
			// a brand-new track (drawn as the "+ New track here" bar).
			drop_ = dropTargetAt(pos.y(), model_.tracks[dragTrack_].kind);
			emitScrubAt(c.outStartMs);
			setCursor(Qt::ClosedHandCursor);
		} else if (mode_ == Mode::ResizeLeft) {
			const double sp = c.speed > 0.01 ? c.speed : 1.0;
			qint64 dMs = qint64(std::llround((pos.x() - pressPos_.x()) * dragSrcPerPx_));
			// The magnet works on the edge the user can see, so snap in OUTPUT
			// time and convert the result back into the source delta.
			bool hit = false;
			const qint64 wantOut =
				dragOrig_.outStartMs + qint64(std::llround(double(dMs) / sp));
			const qint64 snapped = snap(wantOut, dragTrack_, dragClip_, &hit);
			if (hit)
				dMs = qint64(std::llround(double(snapped - dragOrig_.outStartMs) * sp));
			// Move left edge in source-time; keep right (source end) fixed.
			qint64 newSrcStart = std::clamp<qint64>(dragOrig_.srcStartMs + dMs, 0,
								dragOrig_.srcEndMs - kMinClipMs);
			const qint64 srcDelta = newSrcStart - dragOrig_.srcStartMs;
			const qint64 outDelta = qint64(std::llround(double(srcDelta) / sp));
			c.srcStartMs = newSrcStart;
			c.outStartMs = std::max<qint64>(0, dragOrig_.outStartMs + outDelta);
			// Clamping to the source bounds can pull the edge back off the
			// candidate — don't draw a guide the clip isn't actually on.
			snapLineMs_ = (hit && c.outStartMs == snapped) ? snapped : -1;
			c.clampFades(); // a shorter clip can't hold a longer fade
			emitScrubAt(c.outStartMs);
		} else if (mode_ == Mode::ResizeRight) {
			const double sp = c.speed > 0.01 ? c.speed : 1.0;
			qint64 dMs = qint64(std::llround((pos.x() - pressPos_.x()) * dragSrcPerPx_));
			bool hit = false;
			const qint64 wantOut =
				dragOrig_.outEndMs() + qint64(std::llround(double(dMs) / sp));
			const qint64 snapped = snap(wantOut, dragTrack_, dragClip_, &hit);
			if (hit)
				dMs = qint64(std::llround(double(snapped - dragOrig_.outEndMs()) * sp));
			c.srcEndMs = std::clamp<qint64>(dragOrig_.srcEndMs + dMs, dragOrig_.srcStartMs + kMinClipMs,
							srcTotal);
			snapLineMs_ = (hit && c.outEndMs() == snapped) ? snapped : -1;
			c.clampFades();
			emitScrubAt(c.outEndMs());
		}
		update();
		return;
	}

	// Idle: cursor hint + hover preview.
	//
	// Everything below asks where things are on screen, and every one of those
	// questions used to recompute the timeline's total span by walking every
	// clip on every track. Held once here, the whole hover pass shares one
	// answer. Deliberately NOT held over the drag branches above: those change
	// the model as they go, and the span has to follow them.
	const SpanGuard hoverSpan(this);
	// A fade grip lights up under the cursor, and claims the cursor shape from
	// the trim handle it overlaps.
	const FadeHit fh = fadeHandleAt(pos);
	if (!(fh == fadeHover_)) {
		fadeHover_ = fh;
		update();
	}
	int hoverTr = -1;
	const int hoverInc = transitionAtPoint(pos, &hoverTr);
	if (hoverTr != hoverTrTrack_ || hoverInc != hoverTrClip_) {
		hoverTrTrack_ = hoverInc >= 0 ? hoverTr : -1;
		hoverTrClip_ = hoverInc;
		update();
	}
	updateHoverSeam(pos);

	// A pip under the cursor claims the cursor shape too: it sits in the same
	// top band as the trim handle, and an arrow there promises a trim.
	const KeyHit kh = keyPipAt(pos);
	if (!(kh == keyHover_)) {
		keyHover_ = kh;
		update();
	}

	int track = -1;
	const int clip = clipAtPoint(pos, &track);
	if (clip >= 0) {
		const QRect r = clipRect(track, clip);
		const int edge = std::min(8, r.width() / 3);
		if (kh.valid())
			setCursor(Qt::SizeHorCursor);
		else if (fh.valid())
			setCursor(Qt::SizeHorCursor);
		else if (pos.x() - r.left() <= edge || r.right() - pos.x() <= edge)
			setCursor(Qt::SizeHorCursor);
		else
			setCursor(Qt::OpenHandCursor);
		if (e->buttons() == Qt::NoButton) {
			const qint64 om = xToMs(pos.x());
			if (om != hoverMs_) {
				hoverMs_ = om;
				update(); // move the hover marker with the mouse
				emit hoverScrub(om);
			}
		}
	} else {
		unsetCursor();
		if (hoverMs_ >= 0) { // off the clips: the preview is no longer following
			hoverMs_ = -1;
			update();
			emit hoverScrubEnded(); // ...so put it back on the playhead
		}
	}
}

// Double-clicking a fade grip clears that fade — the quickest way back to no
// fade at all, and it matches how the other NLEs behave.
void TimelineView::mouseDoubleClickEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	const FadeHit fh = fadeHandleAt(e->pos());
	if (!fh.valid()) {
		QWidget::mouseDoubleClickEvent(e);
		return;
	}
	TlClip &c = model_.tracks[fh.track].clips[fh.clip];
	if (fh.side == FadeSide::In)
		c.fadeInMs = 0;
	else
		c.fadeOutMs = 0;
	// A double-click also delivers a press, which armed a fade drag; drop it so
	// the following release doesn't re-apply the length that was just cleared.
	mode_ = Mode::None;
	fadeDrag_ = FadeHit();
	dragTrack_ = dragClip_ = -1;
	dragMoved_ = false;
	update();
	emit selectionChanged(fh.track, fh.clip);
	commitEdit();
}

void TimelineView::mouseReleaseEvent(QMouseEvent *e)
{
	// Before the left-button gate below: a pan is usually a MIDDLE-button drag,
	// and that gate would leave mode_ stuck at Pan forever -- after which every
	// mouse-move slides the view whether a button is down or not.
	if (mode_ == Mode::Pan) {
		mode_ = Mode::None;
		unsetCursor();
		return;
	}
	if (e->button() != Qt::LeftButton)
		return;
	if (mode_ == Mode::Scrub) {
		mode_ = Mode::None;
		return;
	}
	if (mode_ == Mode::KeyDrag) {
		const bool moved = dragMoved_ && keyDrag_.tMs != keyDragFrom_;
		const KeyHit kh = keyDrag_;
		mode_ = Mode::None;
		keyDrag_ = KeyHit();
		keyDragFrom_ = -1;
		dragTrack_ = dragClip_ = -1;
		dragMoved_ = false;
		snapLineMs_ = -1;
		QToolTip::hideText();
		update();
		if (moved) {
			commitEdit(); // one undo step for the whole drag
		} else if (kh.valid() && kh.track < model_.tracks.size() &&
			   kh.clip < model_.tracks[kh.track].clips.size()) {
			// A click that did not drag means "take me to this key" -- the
			// thing you always want before editing its values, and previously a
			// trip to the Inspector's Prev/Next buttons.
			playheadMs_ = model_.tracks[kh.track].clips[kh.clip].outStartMs + kh.tMs;
			emitScrubAt(playheadMs_);
			update();
		}
		return;
	}
	if (mode_ == Mode::Fade) {
		const bool changed = dragMoved_;
		mode_ = Mode::None;
		fadeDrag_ = FadeHit();
		dragTrack_ = dragClip_ = -1;
		dragMoved_ = false;
		QToolTip::hideText();
		update();
		if (changed)
			commitEdit(); // repaint the preview + record one undo step
		return;
	}
	if (mode_ != Mode::None && dragTrack_ >= 0) {
		// Land the clip: on another same-kind lane, or on a NEW track created at
		// the insertion point the drop indicator was showing.
		if (mode_ == Mode::Move && dragMoved_) {
			const TlTrack::Kind kind = model_.tracks[dragTrack_].kind;
			int target = -1;
			if (drop_.newTrackAt >= 0) {
				const int at = drop_.newTrackAt;
				TlTrack nt;
				nt.kind = kind;
				nt.color = randomPastel();
				model_.tracks.insert(at, nt);
				if (dragTrack_ >= at)
					++dragTrack_; // the source lane shifted down
				target = at;
			} else if (drop_.track >= 0 && drop_.track != dragTrack_ &&
				   model_.tracks[drop_.track].kind == kind &&
				   !model_.tracks[drop_.track].locked) {
				target = drop_.track;
			}
			if (target >= 0) {
				TlClip moved = model_.tracks[dragTrack_].clips.takeAt(dragClip_);
				model_.tracks[target].clips.append(moved);
				selTrack_ = target;
				selClip_ = model_.tracks[target].clips.size() - 1;
				// Tidy up: if moving the clip emptied its old lane, drop that
				// lane — unless it's the last one of its kind.
				if (model_.tracks[dragTrack_].clips.isEmpty()) {
					int ofKind = 0;
					for (const TlTrack &tr : model_.tracks)
						if (tr.kind == kind)
							++ofKind;
					if (ofKind > 1) {
						model_.tracks.remove(dragTrack_);
						if (selTrack_ > dragTrack_)
							--selTrack_;
					}
				}
				renumberTracks();
				emit selectionChanged(selTrack_, selClip_);
			}
		}
		const bool changed = dragMoved_;
		mode_ = Mode::None;
		dragTrack_ = dragClip_ = -1;
		dragMoved_ = false;
		drop_ = DropTarget();
		snapLineMs_ = -1;
		unsetCursor();
		clampView();
		updateGeometry();
		update();
		if (changed)
			commitEdit();
	}
}

void TimelineView::wheelEvent(QWheelEvent *e)
{
	const QPoint pos = e->position().toPoint();
	if (!contentRect().contains(pos) && !(pos.y() < lp_.margin + lp_.rulerH)) {
		e->ignore();
		return;
	}
	const QPoint ad = e->angleDelta();
	const bool pan = (e->modifiers() & Qt::ShiftModifier) || qAbs(ad.x()) > qAbs(ad.y());
	const int delta = pan ? (ad.x() != 0 ? ad.x() : ad.y()) : ad.y();
	if (delta == 0)
		return;
	const double steps = delta / 120.0;
	if (!pan) {
		const QRect c = contentRect();
		zoomAnchorMs_ = xToMs(pos.x());
		zoomAnchorFrac_ = std::clamp(double(pos.x() - c.x()) / std::max(1, c.width()), 0.0, 1.0);
		zoomTarget_ = std::clamp(zoomTarget_ * std::pow(1.3, steps), 1.0, lp_.maxZoom);
		anim_->start();
	} else {
		viewTarget_ -= qint64(steps * visibleMs() * 0.15);
		viewTarget_ = std::clamp<qint64>(viewTarget_, 0, std::max<qint64>(0, spanMs() - visibleMs()));
		anim_->start();
	}
	e->accept();
}

void TimelineView::keyPressEvent(QKeyEvent *e)
{
	if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
		deleteSelected();
		return;
	}
	QWidget::keyPressEvent(e);
}

void TimelineView::changeEvent(QEvent *e)
{
	// The cached fonts derive from the widget font, so a style or font change
	// has to invalidate them or the timeline keeps painting with the old ones.
	if (e->type() == QEvent::FontChange)
		fontsForPx_ = -1;
	QWidget::changeEvent(e);
}

void TimelineView::leaveEvent(QEvent *)
{
	const bool wasHovering = hoverMs_ >= 0;
	hoverMs_ = -1;
	hoverSeamTrack_ = -1;
	hoverSeamMs_ = -1;
	update(); // drop the hover marker and any lit seam
	// Only when the pointer really had the preview: leaving the view is also
	// what happens on the way to a menu or another panel, and re-rendering the
	// playhead frame every time the mouse crosses this edge is work for nothing.
	if (wasHovering)
		emit hoverScrubEnded();
}

// Every selected clip, primary included, as (track, clip).
void TimelineView::applyToSelection(const std::function<void(TlClip &)> &fn)
{
	const QVector<QPair<int, int>> sel = selectedPairs();
	if (sel.isEmpty())
		return;
	for (const auto &p : sel) {
		if (p.first < 0 || p.first >= model_.tracks.size())
			continue;
		TlTrack &t = model_.tracks[p.first];
		if (t.locked || p.second < 0 || p.second >= t.clips.size())
			continue;
		fn(t.clips[p.second]);
	}
	update();
	emit clipsChanged();
}

QVector<QPair<int, int>> TimelineView::selectedPairs() const
{
	// A selection can outlive the clips it names -- undo swaps the whole model
	// underneath it, and a shorter track leaves indices pointing past the end.
	// Dropping those here rather than trusting them keeps every caller (and the
	// comparator below, which dereferences them) safe.
	const auto live = [this](const QPair<int, int> &p) {
		return p.first >= 0 && p.first < model_.tracks.size() && p.second >= 0 &&
		       p.second < model_.tracks[p.first].clips.size();
	};
	QVector<QPair<int, int>> out;
	if (selTrack_ >= 0 && selClip_ >= 0 && live({selTrack_, selClip_}))
		out.append({selTrack_, selClip_});
	for (const auto &p : extraSel_)
		if (p != qMakePair(selTrack_, selClip_) && live(p))
			out.append(p);
	// Sorted by track then time so callers can rely on a stable order.
	std::sort(out.begin(), out.end(), [this](const auto &a, const auto &b) {
		if (a.first != b.first)
			return a.first < b.first;
		return model_.tracks[a.first].clips[a.second].outStartMs <
		       model_.tracks[b.first].clips[b.second].outStartMs;
	});
	return out;
}

TlSpan TimelineView::selectionSpan() const
{
	TlSpan span;
	for (const auto &p : selectedPairs()) {
		const TlClip &c = model_.tracks[p.first].clips[p.second];
		span.fromMs = span.fromMs < 0 ? c.outStartMs : std::min(span.fromMs, c.outStartMs);
		span.toMs = std::max(span.toMs, c.outEndMs());
	}
	return span;
}

bool TimelineView::isSelected(int track, int clip) const
{
	return (track == selTrack_ && clip == selClip_) || extraSel_.contains({track, clip});
}

void TimelineView::selectClip(int track, int clip)
{
	selTransition_ = false;
	extraSel_.clear();
	if (track < 0 || track >= model_.tracks.size() || clip < 0 ||
	    clip >= model_.tracks[track].clips.size()) {
		selTrack_ = selClip_ = -1;
	} else {
		selTrack_ = track;
		selClip_ = clip;
	}
	emit selectionChanged(selTrack_, selClip_);
	update();
}

void TimelineView::addToSelection(int track, int clip)
{
	if (track < 0 || track >= model_.tracks.size() || clip < 0 ||
	    clip >= model_.tracks[track].clips.size())
		return;
	if (selTrack_ < 0) { // nothing yet: this becomes the primary
		selectClip(track, clip);
		return;
	}
	if (track == selTrack_ && clip == selClip_)
		return;
	extraSel_.insert({track, clip});
	update();
	// The primary is unchanged, so the arguments repeat -- but the SELECTION
	// grew, and the Inspector shows what the whole selection has in common.
	// Without this, Ctrl-clicking a second clip leaves the panel describing
	// only the first.
	emit selectionChanged(selTrack_, selClip_);
}

void TimelineView::selectAllClips()
{
	extraSel_.clear();
	for (int t = 0; t < model_.tracks.size(); ++t)
		for (int c = 0; c < model_.tracks[t].clips.size(); ++c) {
			if (selTrack_ < 0) { // nothing was selected: make the first primary
				selTrack_ = t;
				selClip_ = c;
				continue;
			}
			extraSel_.insert({t, c});
		}
	emit selectionChanged(selTrack_, selClip_);
	update();
}

QVector<TimelineView::ClipboardEntry> TimelineView::copySelection() const
{
	QVector<ClipboardEntry> out;
	for (const auto &p : selectedPairs())
		out.append({model_.tracks[p.first].clips[p.second], p.first});
	return out;
}

void TimelineView::pasteAt(const QVector<ClipboardEntry> &entries, qint64 atMs)
{
	if (entries.isEmpty())
		return;
	qint64 earliest = std::numeric_limits<qint64>::max();
	for (const ClipboardEntry &e : entries)
		earliest = std::min(earliest, e.clip.outStartMs);

	extraSel_.clear();
	selTrack_ = selClip_ = -1;
	for (const ClipboardEntry &e : entries) {
		// Back onto its own track when that still exists and accepts it,
		// otherwise the nearest track of the right kind.
		int track = e.track;
		if (track < 0 || track >= model_.tracks.size() || model_.tracks[track].locked)
			track = -1;
		if (track < 0)
			for (int i = 0; i < model_.tracks.size(); ++i)
				if (!model_.tracks[i].locked) {
					track = i;
					break;
				}
		if (track < 0)
			continue; // every track is locked
		TlClip c = e.clip;
		c.outStartMs = std::max<qint64>(0, atMs + (e.clip.outStartMs - earliest));
		model_.tracks[track].clips.append(c);
		const int idx = int(model_.tracks[track].clips.size()) - 1;
		if (selTrack_ < 0) {
			selTrack_ = track;
			selClip_ = idx;
		} else {
			extraSel_.insert({track, idx});
		}
	}
	emit selectionChanged(selTrack_, selClip_);
	clampView();
	updateGeometry();
	update();
	commitEdit();
}

void TimelineView::nudgeSelection(qint64 deltaMs)
{
	const auto sel = selectedPairs();
	if (sel.isEmpty() || deltaMs == 0)
		return;
	// All or nothing: shifting only the unlocked half of a selection would
	// silently break the arrangement the user set up.
	for (const auto &p : sel)
		if (model_.tracks[p.first].locked)
			return;
	// Moving left is limited by whichever selected clip is nearest zero, so the
	// group keeps its shape instead of collapsing against the start.
	if (deltaMs < 0) {
		qint64 room = std::numeric_limits<qint64>::max();
		for (const auto &p : sel)
			room = std::min(room, model_.tracks[p.first].clips[p.second].outStartMs);
		deltaMs = -std::min(room, -deltaMs);
		if (deltaMs == 0)
			return;
	}
	for (const auto &p : sel)
		model_.tracks[p.first].clips[p.second].outStartMs += deltaMs;
	clampView();
	updateGeometry();
	update();
	emit clipsChanged();
}

void TimelineView::toggleMarkerAtPlayhead()
{
	if (playheadMs_ < 0)
		return;
	// Within a few pixels counts as the same marker, so the key removes the one
	// you can see rather than stacking a second on top of it.
	const qint64 tol = std::max<qint64>(1, xToMs(contentRect().x() + 6) - xToMs(contentRect().x()));
	for (int i = 0; i < model_.markers.size(); ++i)
		if (std::llabs(model_.markers[i] - playheadMs_) <= tol) {
			model_.markers.remove(i);
			update();
			commitEdit();
			return;
		}
	model_.markers.append(playheadMs_);
	std::sort(model_.markers.begin(), model_.markers.end());
	update();
	commitEdit();
}

qint64 TimelineView::markerNear(qint64 fromMs, bool forward) const
{
	qint64 best = -1;
	for (const qint64 m : model_.markers) {
		if (forward ? (m > fromMs + 1) : (m < fromMs - 1))
			if (best < 0 || std::llabs(m - fromMs) < std::llabs(best - fromMs))
				best = m;
	}
	return best;
}

void TimelineView::splitAtPlayhead()
{
	if (playheadMs_ < 0)
		return;
	bool any = false;
	for (int ti = 0; ti < model_.tracks.size(); ++ti) {
		TlTrack &t = model_.tracks[ti];
		if (t.locked)
			continue;
		// Iterate over the original count: the halves appended below must not be
		// re-split by this same pass.
		const int n = int(t.clips.size());
		for (int ci = 0; ci < n; ++ci) {
			TlClip &c = t.clips[ci];
			if (!c.coversOutput(playheadMs_))
				continue;
			const qint64 cut = playheadMs_ - c.outStartMs;
			if (cut < kMinClipMs || c.outDurationMs() - cut < kMinClipMs)
				continue; // too close to an edge to leave usable halves
			TlClip right = c;
			const qint64 srcCut = c.srcAtOutput(playheadMs_);
			c.srcEndMs = srcCut;
			right.srcStartMs = srcCut;
			right.outStartMs = playheadMs_;
			if (c.freeDuration()) { // stills/captions have no source clock
				c.srcEndMs = c.srcStartMs + cut;
				right.srcStartMs = 0;
				right.srcEndMs = right.srcEndMs - c.srcEndMs;
			}
			splitFades(c, right);
			t.clips.append(right);
			any = true;
		}
	}
	if (!any)
		return;
	extraSel_.clear();
	update();
	commitEdit();
}

void TimelineView::deleteSelected()
{
	// Delete on a selected transition removes the TRANSITION -- the clips stay
	// and snap together. Deleting the clip under it would be a nasty surprise.
	if (selTransition_) {
		removeSelectedTransition();
		return;
	}
	const auto sel = selectedPairs();
	if (sel.isEmpty())
		return;
	// Remove from the highest index down so earlier indices stay valid.
	QVector<QPair<int, int>> ordered = sel;
	std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
		if (a.first != b.first)
			return a.first > b.first;
		return a.second > b.second;
	});
	bool removed = false;
	for (const auto &p : ordered) {
		TlTrack &t = model_.tracks[p.first];
		if (t.locked || p.second < 0 || p.second >= t.clips.size())
			continue;
		// Auto ripple: close the gap by pulling every later clip on this track left.
		const qint64 gapStart = t.clips[p.second].outStartMs;
		const qint64 gapLen = t.clips[p.second].outDurationMs();
		t.clips.remove(p.second);
		if (t.ripple)
			for (TlClip &c : t.clips)
				if (c.outStartMs >= gapStart)
					c.outStartMs = std::max<qint64>(0, c.outStartMs - gapLen);
		removed = true;
	}
	if (!removed)
		return;
	extraSel_.clear();
	selClip_ = -1;
	emit selectionChanged(selTrack_, -1);
	clampView();
	updateGeometry();
	update();
	commitEdit();
}

void TimelineView::splitClip(int track, int clip, qint64 atOutMs)
{
	if (track < 0 || track >= model_.tracks.size())
		return;
	TlTrack &t = model_.tracks[track];
	if (t.locked || clip < 0 || clip >= t.clips.size())
		return;
	TlClip &a = t.clips[clip];
	if (atOutMs <= a.outStartMs + kMinClipMs || atOutMs >= a.outEndMs() - kMinClipMs)
		return;
	const qint64 splitSrc = a.srcAtOutput(atOutMs);
	TlClip b = a;
	b.srcStartMs = splitSrc;
	b.outStartMs = atOutMs;
	a.srcEndMs = splitSrc;
	splitFades(a, b);
	t.clips.insert(clip + 1, b);
	selClip_ = clip + 1;
	update();
	emit selectionChanged(track, selClip_);
	commitEdit();
}

// Put a new effect clip on `track` starting at `atOutMs`. Used by the effect
// track's context menu, so the clip lands on the track you right-clicked and at
// the point you clicked -- not on whichever track a generic "add" would pick.
void TimelineView::addEffectClipAt(int track, qint64 atOutMs, FxType type)
{
	if (track < 0 || track >= model_.tracks.size())
		return;
	TlTrack &t = model_.tracks[track];
	if (t.locked || t.kind != TlTrack::Kind::Effect)
		return;
	TlClip c;
	c.type = TlClip::Type::Effect;
	c.srcStartMs = 0;
	c.srcEndMs = 3000; // freely stretchable, like a caption
	c.outStartMs = std::max<qint64>(0, atOutMs);
	// The fx field is still filled in so the clip's LABEL and the older
	// readers keep working; what actually renders is the component.
	c.fx.type = type;
	c.fx.params = fxDefaults(type);
	c.components.append(
		TlClip::newEffectComponent(effectComponentId(type), fxDefaults(type)));
	t.clips.append(c);
	selTrack_ = track;
	selClip_ = t.clips.size() - 1;
	extraSel_.clear();
	selTransition_ = false;
	clampView();
	updateGeometry();
	update();
	emit selectionChanged(selTrack_, selClip_);
	commitEdit(); // repaints the preview and records one undo step
}

// The clip that starts next on this track, or -1. "Next" is by start time, not
// by index: clips are stored unordered.
int TimelineView::clipAfter(int track, int clip) const
{
	if (track < 0 || track >= model_.tracks.size())
		return -1;
	const TlTrack &t = model_.tracks[track];
	if (clip < 0 || clip >= t.clips.size())
		return -1;
	const qint64 from = t.clips[clip].outStartMs;
	int best = -1;
	for (int i = 0; i < t.clips.size(); ++i) {
		if (i == clip || t.clips[i].outStartMs <= from)
			continue;
		if (best < 0 || t.clips[i].outStartMs < t.clips[best].outStartMs)
			best = i;
	}
	return best;
}

// How much two clips on a track already overlap, in ms (0 when they do not).
qint64 TimelineView::overlapWith(int track, int a, int b) const
{
	if (track < 0 || track >= model_.tracks.size() || a < 0 || b < 0)
		return 0;
	const TlTrack &t = model_.tracks[track];
	if (a >= t.clips.size() || b >= t.clips.size())
		return 0;
	const qint64 lo = std::max(t.clips[a].outStartMs, t.clips[b].outStartMs);
	const qint64 hi = std::min(t.clips[a].outEndMs(), t.clips[b].outEndMs());
	return std::max<qint64>(0, hi - lo);
}

// Slide the following clip back so it overlaps this one, which IS the
// transition. Everything after it moves by the same amount, so the rest of the
// track keeps its spacing rather than the next clip landing on top of it.
void TimelineView::makeTransitionWithNext(int track, int clip)
{
	const int nx = clipAfter(track, clip);
	if (track < 0 || track >= model_.tracks.size() || nx < 0)
		return;
	TlTrack &t = model_.tracks[track];
	if (t.locked)
		return;
	// A second, or a third of the shorter clip if that is less -- an overlap
	// longer than either clip has nothing left to fade from.
	const qint64 shorter = std::min(t.clips[clip].outDurationMs(), t.clips[nx].outDurationMs());
	const qint64 want = std::max<qint64>(kMinClipMs, std::min<qint64>(1000, shorter / 3));
	// Close any gap first, then overlap by `want`.
	const qint64 shift = t.clips[nx].outStartMs - (t.clips[clip].outEndMs() - want);
	if (shift == 0)
		return;
	const qint64 from = t.clips[nx].outStartMs;
	for (TlClip &c : t.clips)
		if (c.outStartMs >= from)
			c.outStartMs = std::max<qint64>(0, c.outStartMs - shift);
	t.clips[nx].transition.enabled = true;
	selTrack_ = track;
	selClip_ = nx;
	selTransition_ = true; // select the transition, so its settings are right there
	extraSel_.clear();
	clampView();
	updateGeometry();
	update();
	emit selectionChanged(selTrack_, selClip_);
	commitEdit();
}

void TimelineView::showKeyMenu(const KeyHit &hit, const QPoint &globalPos)
{
	if (!hit.valid() || hit.track >= model_.tracks.size() ||
	    hit.clip >= model_.tracks[hit.track].clips.size())
		return;
	const bool locked = model_.tracks[hit.track].locked;
	const qint64 outMs = model_.tracks[hit.track].clips[hit.clip].outStartMs + hit.tMs;

	QMenu menu(this);
	QAction *goTo = menu.addAction(QStringLiteral("Go to this keyframe"));
	QAction *del = menu.addAction(QStringLiteral("Delete keyframe"));
	del->setEnabled(!locked);
	// One pip can stand for several channels keyed at the same instant, and
	// deleting "the keyframe" then takes all of them. Say so rather than
	// letting it be a surprise.
	del->setToolTip(QStringLiteral("Removes every channel keyed at this moment."));
	menu.setToolTipsVisible(true);

	const QAction *chosen = menu.exec(globalPos);
	if (chosen == goTo) {
		playheadMs_ = outMs;
		emitScrubAt(playheadMs_);
		update();
	} else if (chosen == del && !locked) {
		if (removeClipKeysAt(model_.tracks[hit.track].clips[hit.clip], hit.tMs)) {
			update();
			commitEdit();
		}
	}
}

void TimelineView::showClipMenu(int track, int clip, const QPoint &globalPos, qint64 atOutMs)
{
	const bool locked = model_.tracks[track].locked;
	QMenu menu(this);
	const TlClip &menuClip = model_.tracks[track].clips[clip];
	const bool isFx = menuClip.type == TlClip::Type::Effect;

	QAction *inspect = menu.addAction(QStringLiteral("Show in inspector"));
	QAction *keys = menu.addAction(QStringLiteral("Keyframes…"));

	// Export just this stretch. Right-clicking a clip that is not part of the
	// current selection means THAT clip -- picking it up as a selection first
	// would be a click nobody asked for -- so the span is worked out here
	// rather than read off selectionSpan().
	const bool inSel = isSelected(track, clip);
	const auto sel = selectedPairs();
	TlSpan exportSpan;
	if (inSel && sel.size() > 1) {
		exportSpan = selectionSpan();
	} else {
		exportSpan.fromMs = menuClip.outStartMs;
		exportSpan.toMs = menuClip.outEndMs();
	}
	QAction *exportSel = menu.addAction(
		(inSel && sel.size() > 1)
			? QStringLiteral("Export selection…  (%1 clips)").arg(sel.size())
			: QStringLiteral("Export this clip…"));
	exportSel->setToolTip(QStringLiteral(
		"Opens the usual Export window for just this stretch of the timeline — every "
		"track inside it, so the excerpt looks like what you were watching."));
	// An effect clip animates its own parameters, not a transform, so the
	// transform keyframe editor would be empty and misleading.
	keys->setEnabled(!locked && !isFx);
	QAction *fxToggle = isFx ? menu.addAction(menuClip.fx.enabled
							  ? QStringLiteral("Disable effect")
							  : QStringLiteral("Enable effect"))
				 : nullptr;
	QAction *fxRename = isFx ? menu.addAction(QStringLiteral("Rename effect…")) : nullptr;
	if (fxToggle)
		fxToggle->setEnabled(!locked);
	if (fxRename)
		fxRename->setEnabled(!locked);
	menu.addSeparator();
	QAction *split = menu.addAction(QStringLiteral("Split here"));
	const TlClip &c = model_.tracks[track].clips[clip];
	split->setEnabled(!locked && atOutMs > c.outStartMs + kMinClipMs &&
			  atOutMs < c.outEndMs() - kMinClipMs);
	QAction *dup = menu.addAction(QStringLiteral("Duplicate"));
	dup->setEnabled(!locked);
	// Transitions are made by OVERLAPPING two clips -- there is no transition
	// object to create. That is a good model (moving a clip retimes the
	// transition for free) but an invisible one: with no button anywhere, the
	// reasonable conclusion is that the editor has no transitions. This command
	// makes the overlap for you, and after using it once the model is obvious.
	QAction *mkTr = nullptr;
	const int nextClip = clipAfter(track, clip);
	if (TimelineModel::isPictureKind(model_.tracks[track].kind)) {
		const bool already = overlapWith(track, clip, nextClip) > 0;
		mkTr = menu.addAction(already ? QStringLiteral("Transition already here")
					      : QStringLiteral("Make transition with next clip"));
		mkTr->setEnabled(!locked && nextClip >= 0 && !already);
		mkTr->setToolTip(QStringLiteral(
			"Slides the next clip back so the two overlap. The overlap IS the "
			"transition — drag either clip to change its length."));
	}
	QAction *mute = menu.addAction(model_.tracks[track].muted ? QStringLiteral("Unmute track")
								 : QStringLiteral("Mute track"));
	menu.addSeparator();
	QAction *del = menu.addAction(QStringLiteral("Delete clip"));
	del->setEnabled(!locked);
	QAction *chosen = menu.exec(globalPos);
	if (chosen == inspect) {
		emit inspectClipRequested();
		return;
	}
	if (chosen == exportSel) {
		emit exportRangeRequested(exportSpan.fromMs, exportSpan.toMs);
		return;
	}
	if (chosen == keys) {
		selTrack_ = track;
		selClip_ = clip;
		emit selectionChanged(selTrack_, selClip_);
		update();
		emit keyframeEditorRequested();
		return;
	}
	if (mkTr && chosen == mkTr) {
		makeTransitionWithNext(track, clip);
		return;
	}
	if (fxToggle && chosen == fxToggle) {
		TlClip &fc = model_.tracks[track].clips[clip];
		fc.fx.enabled = !fc.fx.enabled;
		update();
		commitEdit();
		return;
	}
	if (fxRename && chosen == fxRename) {
		TlClip &fc = model_.tracks[track].clips[clip];
		bool ok = false;
		const QString n = QInputDialog::getText(this, QStringLiteral("Rename effect"),
							QStringLiteral("Effect name:"),
							QLineEdit::Normal, effectClipLabel(fc), &ok)
					  .trimmed();
		if (!ok)
			return;
		// Blank falls back to the effect type's own name, so a clip is never
		// left nameless with no way back.
		fc.fx.name = n;
		update();
		commitEdit();
		return;
	}
	if (chosen == split) {
		splitClip(track, clip, atOutMs);
	} else if (chosen == dup) {
		TlClip copy = model_.tracks[track].clips[clip];
		copy.outStartMs = copy.outEndMs();
		model_.tracks[track].clips.insert(clip + 1, copy);
		selClip_ = clip + 1;
		emit selectionChanged(track, selClip_);
		update();
		commitEdit();
	} else if (chosen == mute) {
		model_.tracks[track].muted = !model_.tracks[track].muted;
		update();
		commitEdit();
	} else if (chosen == del) {
		selTrack_ = track;
		selClip_ = clip;
		deleteSelected();
	}
}

} // namespace harpia
