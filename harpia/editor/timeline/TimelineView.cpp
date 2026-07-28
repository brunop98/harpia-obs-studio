#include "TimelineView.hpp"

#include <QColorDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QTimer>
#include <QWheelEvent>

#include <limits>

namespace harpia {

namespace {
constexpr qint64 kMinClipMs = 100;
constexpr qint64 kTailMs = 5000;   // draggable space past the end
constexpr qint64 kMinSpanMs = 8000;

const QColor kBg(0x15, 0x17, 0x1a);
const QColor kGutter(0x1b, 0x1e, 0x23);
const QColor kLane(0x1f, 0x22, 0x27);
const QColor kLaneAlt(0x23, 0x27, 0x2d);
const QColor kBorder(0x30, 0x33, 0x38);
const QColor kCaption(0x9a, 0x9f, 0xa8);
const QColor kAccent(0x00, 0xae, 0xef);
const QColor kVidFill(0x2e, 0x4d, 0x6e);
const QColor kVidFillSel(0x3a, 0x6e, 0xa5);
const QColor kAudFill(0x2c, 0x50, 0x45);
const QColor kAudFillSel(0x37, 0x74, 0x63);
const QColor kTextFill(0x4a, 0x3a, 0x5e);
const QColor kTextFillSel(0x6b, 0x51, 0x8c);
const QColor kWave(0x6f, 0xd0, 0xb0);
const QColor kPlayhead(0xe5, 0x48, 0x4d);
// Distinct from the playhead on purpose: the hover marker is where the PREVIEW
// is looking right now, which is not where an edit will land.
const QColor kHover(0xf5, 0xc0, 0x42);
// Project markers: green, so they read as "a place", not "a time now".
const QColor kMarker(0x5c, 0xd6, 0x8a);
} // namespace

TimelineView::TimelineView(QWidget *parent) : QWidget(parent)
{
	setFocusPolicy(Qt::ClickFocus);
	setMouseTracking(true);
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
	if (!moving)
		anim_->stop();
}

void TimelineView::setModel(const TimelineModel &m)
{
	model_ = m;
	selTrack_ = selClip_ = -1;
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
	update();
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
	int a = 0;
	for (int i = 0; i < model_.tracks.size(); ++i) {
		TlTrack &t = model_.tracks[i];
		if (t.kind == TlTrack::Kind::Video)
			t.name = QStringLiteral("V%1").arg(nv - i);
		else
			t.name = QStringLiteral("A%1").arg(++a);
	}
}

void TimelineView::addTrack(TlTrack::Kind kind, int atIndex)
{
	const int nv = model_.videoTrackCount();
	// Video tracks live above audio tracks; clamp the insertion into that group.
	const int lo = (kind == TlTrack::Kind::Video) ? 0 : nv;
	const int hi = (kind == TlTrack::Kind::Video) ? nv : model_.tracks.size();
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
	emit clipsChanged();
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
	emit clipsChanged();
}

void TimelineView::setSnapEnabled(bool on)
{
	snap_ = on;
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
		const int nv = model_.videoTrackCount();
		TlTrack t;
		t.kind = kind;
		t.color = randomPastel();
		idx = (kind == TlTrack::Kind::Video) ? nv : model_.tracks.size();
		model_.tracks.insert(idx, t);
		renumberTracks();
	}
	model_.tracks[idx].clips.append(clip);
	selTrack_ = idx;
	selClip_ = model_.tracks[idx].clips.size() - 1;
	clampView();
	updateGeometry();
	update();
	emit clipsChanged();
	emit selectionChanged(selTrack_, selClip_);
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

void TimelineView::setPlayhead(qint64 outMs)
{
	playheadMs_ = outMs;
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

QRect TimelineView::contentRect() const
{
	const int x = lp_.margin + lp_.gutterW;
	const int y = lp_.margin + lp_.rulerH;
	return QRect(x, y, std::max(1, width() - lp_.margin - x), std::max(1, height() - lp_.margin - y));
}

int TimelineView::laneHeight(int track) const
{
	if (track < 0 || track >= model_.tracks.size())
		return lp_.videoLaneH;
	return model_.tracks[track].kind == TlTrack::Kind::Video ? lp_.videoLaneH : lp_.audioLaneH;
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
	return QRect(lp_.margin, l.y(), lp_.gutterW, l.height());
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
	const int nv = model_.videoTrackCount();
	// Video tracks occupy [0,nv), audio [nv,n). A clip can only land in its own
	// group, and a new track can only be inserted inside that group's range.
	const int lo = (kind == TlTrack::Kind::Video) ? 0 : nv;
	const int hi = (kind == TlTrack::Kind::Video) ? nv : model_.tracks.size();
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

void TimelineView::showTrackMenu(int track, const QPoint &globalPos)
{
	if (track < 0 || track >= model_.tracks.size())
		return;
	TlTrack &t = model_.tracks[track];
	const bool video = t.kind == TlTrack::Kind::Video;

	QMenu menu(this);
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
	QAction *del = menu.addAction(QStringLiteral("Delete track"));

	QAction *chosen = menu.exec(globalPos);
	if (!chosen)
		return;
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
			emit clipsChanged();
			return;
		}
	} else if (chosen == colour) {
		const QColor c = QColorDialog::getColor(t.color, this, QStringLiteral("Track colour"));
		if (c.isValid())
			t.color = c;
	} else if (chosen == reroll) {
		t.color = randomPastel();
	} else if (chosen == addAbove) {
		addTrack(t.kind, track);
		return;
	} else if (chosen == addBelow) {
		addTrack(t.kind, track + 1);
		return;
	} else if (chosen == del) {
		deleteTrack(track);
		return;
	}
	update();
	emit clipsChanged();
}

qint64 TimelineView::snap(qint64 ms, int ignoreTrack, int ignoreClip) const
{
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
	for (int ti = 0; ti < model_.tracks.size(); ++ti)
		for (int ci = 0; ci < model_.tracks[ti].clips.size(); ++ci) {
			if (ti == ignoreTrack && ci == ignoreClip)
				continue;
			consider(model_.tracks[ti].clips[ci].outStartMs);
			consider(model_.tracks[ti].clips[ci].outEndMs());
		}
	return best;
}

void TimelineView::emitScrubAt(qint64 outMs)
{
	emit scrub(outMs);
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
	p.fillRect(bar, kGutter);
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
		p.setPen(kCaption);
		const qint64 m = t / 60000, sec = (t / 1000) % 60;
		const QString lab = (t % 1000 && t < 60000)
					    ? QStringLiteral("%1.%2s").arg(t / 1000).arg((t % 1000) / 100)
					    : QStringLiteral("%1:%2").arg(m).arg(sec, 2, 10, QLatin1Char('0'));
		p.drawText(x + 2, bar.bottom() - 4, lab);
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

void TimelineView::drawClip(QPainter &p, int track, int clip) const
{
	const TlTrack &t = model_.tracks[track];
	const TlClip &c = t.clips[clip];
	const bool isText = c.type == TlClip::Type::Text;
	const bool isImage = c.type == TlClip::Type::Image;
	const bool video = t.kind == TlTrack::Kind::Video && !isText && !isImage;
	const bool sel = isSelected(track, clip);
	const QRect r = clipRect(track, clip);
	// Scrolled-away clips cost the same as visible ones otherwise: the painter
	// clips the output, but the filmstrip tiling and the per-pixel waveform loop
	// below still run in full. On a long timeline that is most of the paint.
	const QRect content = contentRect();
	if (r.right() < content.x() || r.x() > content.right())
		return;
	QPainterPath path;
	path.addRoundedRect(r, 4, 4);
	p.setPen(Qt::NoPen);
	// Clips take their track's colour (selection brightens it); text clips get a
	// slight violet lean so they still read as captions.
	QColor fill = t.color.isValid() ? t.color
					: (t.kind == TlTrack::Kind::Video ? kVidFill : kAudFill);
	if (isText)
		fill = QColor::fromHsv(fill.hue(), fill.saturation(), fill.value()).darker(105);
	if (sel)
		fill = fill.lighter(145);
	if (t.hidden || t.muted)
		fill = fill.darker(160);
	p.setBrush(fill);
	p.drawPath(path);

	p.save();
	p.setClipPath(path);
	if (video) {
		const auto it = srcThumbs_.constFind(c.sourceId);
		const qint64 sdur = srcThumbDur_.value(c.sourceId, 0);
		if (it != srcThumbs_.constEnd() && !it.value().isEmpty() && sdur > 0) {
			const QVector<QImage> &strip = it.value();
			const double aspect = srcAspect_.value(c.sourceId, 16.0 / 9.0);
			const int th = r.height() - 2;
			const int tileW = std::max(8, int(th * aspect));
			// Start at the first tile at or before the visible edge (tiles must stay
			// on their original grid, or they'd shift as the view scrolls).
			const int x0 = r.x() + 1;
			const int step = tileW + 1;
			const int firstVis = x0 + std::max(0, (content.x() - x0) / step) * step;
			const int lastVis = std::min(r.right() - 1, content.right() + step);
			for (int x = firstVis; x < lastVis; x += step) {
				const double f =
					std::clamp(double(x + tileW / 2 - r.x()) / std::max(1, r.width()),
						   0.0, 1.0);
				const qint64 ms = c.srcStartMs + qint64(f * double(c.srcLenMs()));
				const int ti = std::clamp(int(double(ms) / sdur * strip.size()), 0,
							  int(strip.size()) - 1);
				if (!strip[ti].isNull())
					p.drawImage(QRect(x, r.y() + 1, tileW, th), strip[ti]);
			}
		}
	} else if (!c.peaks.isEmpty() && c.srcEndMs > 0) {
		p.setPen(QPen(kWave, 1));
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
	// While dragging, drop the per-clip decoration so the drop indicator reads
	// clearly.
	const bool dragging = (mode_ == Mode::Move && dragMoved_);

	// Keyframe diamonds along the top edge, so an animated clip reads as such.
	if (!dragging && !c.keys.isEmpty() && c.outDurationMs() > 0) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0xff, 0xd4, 0x4f));
		for (const TlKeyframe &k : c.keys) {
			const double f = std::clamp(double(k.tMs) / double(c.outDurationMs()), 0.0, 1.0);
			const int kx = r.x() + int(f * r.width());
			const int ky = r.y() + 5;
			QPainterPath d;
			d.moveTo(kx, ky - 4);
			d.lineTo(kx + 4, ky);
			d.lineTo(kx, ky + 4);
			d.lineTo(kx - 4, ky);
			d.closeSubpath();
			p.drawPath(d);
		}
	}

	// Label bar along the bottom.
	if (!dragging && r.width() >= 28) {
		p.setFont(clipFont_);
		p.fillRect(QRect(r.x(), r.bottom() - 12, r.width(), 13), QColor(0, 0, 0, 150));
		p.setPen(QColor(0xe6, 0xe6, 0xe6));
		QString what;
		if (isText)
			what = QStringLiteral("T  %1").arg(
				c.text.text.split(QLatin1Char('\n')).value(0));
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

	p.setPen(sel ? QPen(kAccent, 2) : QPen(kBorder, 1));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(r, 4, 4);
}

void TimelineView::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.fillRect(rect(), kBg);

	clampView();
	viewTarget_ = viewStart_;
	ensureFonts();

	const bool dragging = (mode_ == Mode::Move && dragMoved_);

	// Lanes + headers.
	for (int i = 0; i < model_.tracks.size(); ++i) {
		const TlTrack &t = model_.tracks[i];
		const QRect lane = laneRect(i);
		p.setPen(Qt::NoPen);
		// Highlight the lane a dragged clip would land on.
		const bool dropHere = dragging && drop_.track == i;
		p.setBrush(dropHere ? QColor(kAccent.red(), kAccent.green(), kAccent.blue(), 40)
				    : ((i % 2) ? kLaneAlt : kLane));
		p.drawRect(lane);
		if (t.locked) { // faint hatch so a locked lane reads as untouchable
			p.setBrush(QBrush(QColor(0xff, 0xff, 0xff, 10), Qt::BDiagPattern));
			p.drawRect(lane);
		}

		const QRect hdr = trackHeaderRect(i);
		p.setBrush(kGutter);
		p.drawRect(hdr);
		// Colour chip down the left edge of the header.
		p.setBrush(t.color);
		p.drawRect(QRect(hdr.x(), hdr.y() + 1, 4, hdr.height() - 2));

		p.setPen((t.hidden || t.muted) ? kCaption : QColor(0xe8, 0xea, 0xed));
		p.setFont(hdrFont_);
		p.drawText(hdr.adjusted(10, 2, -4, 0), Qt::AlignTop | Qt::AlignLeft, t.name);

		// Lock / hide / mute toggles (hidden while dragging to cut clutter).
		if (!dragging) {
			p.setFont(toggleFont_);
			auto drawToggle = [&](HeaderHit which, const QString &glyph, bool on) {
				const QRect r = headerToggleRect(i, which);
				if (r.isEmpty())
					return;
				p.setPen(Qt::NoPen);
				p.setBrush(on ? kAccent : QColor(0x2b, 0x2f, 0x36));
				p.drawRoundedRect(r, 3, 3);
				p.setPen(on ? QColor(0xff, 0xff, 0xff) : kCaption);
				p.drawText(r, Qt::AlignCenter, glyph);
			};
			drawToggle(HeaderHit::Lock, QStringLiteral("L"), t.locked);
			if (t.kind == TlTrack::Kind::Video)
				drawToggle(HeaderHit::Hide, QStringLiteral("H"), t.hidden);
			drawToggle(HeaderHit::Mute, QStringLiteral("M"), t.muted);
		}

		for (int ci = 0; ci < t.clips.size(); ++ci)
			drawClip(p, i, ci);
	}

	// "Release here to make a new track" indicator.
	if (dragging && drop_.newTrackAt >= 0) {
		const QRect c = contentRect();
		const int y = insertYFor(drop_.newTrackAt);
		p.setPen(QPen(kAccent, 3));
		p.drawLine(c.x(), y, c.right(), y);
		p.setPen(Qt::NoPen);
		p.setBrush(kAccent);
		const QRect tag(c.x() + 6, y - 9, 104, 18);
		p.drawRoundedRect(tag, 3, 3);
		QFont tf = p.font();
		tf.setPixelSize(10);
		tf.setBold(true);
		p.setFont(tf);
		p.setPen(QColor(0xff, 0xff, 0xff));
		p.drawText(tag, Qt::AlignCenter, QStringLiteral("+ New track here"));
	}

	if (model_.tracks.isEmpty()) {
		p.setPen(kCaption);
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
		p.setPen(QPen(kMarker, 1, Qt::DotLine));
		p.drawLine(mx, lp_.margin + lp_.rulerH, mx, height() - lp_.margin);
		p.setPen(Qt::NoPen);
		p.setBrush(kMarker);
		QPainterPath flag;
		flag.moveTo(mx, lp_.margin + 2);
		flag.lineTo(mx + 9, lp_.margin + 6);
		flag.lineTo(mx, lp_.margin + 10);
		flag.closeSubpath();
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
			QPen hp(kHover, 1, Qt::DashLine);
			p.setPen(hp);
			p.drawLine(hx, lp_.margin, hx, height() - lp_.margin);

			const qint64 mm = hoverMs_ / 60000, ss = (hoverMs_ / 1000) % 60,
				     cs = (hoverMs_ % 1000) / 10;
			const QString lab = QStringLiteral("%1:%2.%3")
						    .arg(mm)
						    .arg(ss, 2, 10, QLatin1Char('0'))
						    .arg(cs, 2, 10, QLatin1Char('0'));
			p.setFont(toggleFont_);
			const int tw = p.fontMetrics().horizontalAdvance(lab) + 8;
			// Flip the tag to the left near the right edge so it stays readable.
			const bool flip = hx + tw + 2 > c.right();
			const QRect tag(flip ? hx - tw - 2 : hx + 2, lp_.margin + 1, tw, 14);
			p.setPen(Qt::NoPen);
			p.setBrush(kHover);
			p.drawRoundedRect(tag, 2, 2);
			p.setPen(QColor(0x15, 0x17, 0x1a));
			p.drawText(tag, Qt::AlignCenter, lab);
		}
	}

	// Playhead across all lanes.
	if (playheadMs_ >= 0) {
		const int x = msToX(playheadMs_);
		if (x >= contentRect().x() - 1 && x <= contentRect().right() + 1) {
			p.setPen(QPen(kPlayhead, 2));
			p.drawLine(x, lp_.margin, x, height() - lp_.margin);
		}
	}

	// Left gutter separator.
	p.setPen(kBorder);
	p.drawLine(lp_.margin + lp_.gutterW, lp_.margin, lp_.margin + lp_.gutterW, height() - lp_.margin);
}

// ---- interaction ------------------------------------------------------------

void TimelineView::mousePressEvent(QMouseEvent *e)
{
	const QPoint pos = e->pos();
	setFocus();
	// The hover marker tracks the preview, which stops following the pointer the
	// moment a drag starts — leaving it drawn would point at nothing.
	hoverMs_ = -1;

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
			showTrackMenu(hTrack, e->globalPosition().toPoint());
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
			return;
		}
		update();
		emit clipsChanged(); // repaints the preview + records an undo step
		return;
	}

	int track = -1;
	const int clip = clipAtPoint(pos, &track);

	if (e->button() == Qt::RightButton) {
		if (clip >= 0) {
			selTrack_ = track;
			selClip_ = clip;
			emit selectionChanged(selTrack_, selClip_);
			update();
			showClipMenu(track, clip, e->globalPosition().toPoint(), xToMs(pos.x()));
		}
		return;
	}
	if (e->button() != Qt::LeftButton)
		return;

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

	if (mode_ == Mode::Scrub) {
		playheadMs_ = xToMs(pos.x());
		emitScrubAt(playheadMs_);
		update();
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
			// Snap the start; if snapping the END lands closer, use that instead.
			const qint64 snapStart = snap(ns, dragTrack_, dragClip_);
			const qint64 snapEnd = snap(ns + dur, dragTrack_, dragClip_) - dur;
			ns = (std::llabs(snapEnd - ns) < std::llabs(snapStart - ns)) ? snapEnd : snapStart;
			c.outStartMs = std::max<qint64>(0, ns);
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
			const qint64 dMs = qint64(std::llround((pos.x() - pressPos_.x()) * dragSrcPerPx_));
			// Move left edge in source-time; keep right (source end) fixed.
			qint64 newSrcStart = std::clamp<qint64>(dragOrig_.srcStartMs + dMs, 0,
								dragOrig_.srcEndMs - kMinClipMs);
			const qint64 srcDelta = newSrcStart - dragOrig_.srcStartMs;
			const qint64 outDelta = qint64(std::llround(double(srcDelta) / (c.speed > 0.01 ? c.speed : 1.0)));
			c.srcStartMs = newSrcStart;
			c.outStartMs = std::max<qint64>(0, dragOrig_.outStartMs + outDelta);
			emitScrubAt(c.outStartMs);
		} else if (mode_ == Mode::ResizeRight) {
			const qint64 dMs = qint64(std::llround((pos.x() - pressPos_.x()) * dragSrcPerPx_));
			c.srcEndMs = std::clamp<qint64>(dragOrig_.srcEndMs + dMs, dragOrig_.srcStartMs + kMinClipMs,
							srcTotal);
			emitScrubAt(c.outEndMs());
		}
		update();
		return;
	}

	// Idle: cursor hint + hover preview.
	int track = -1;
	const int clip = clipAtPoint(pos, &track);
	if (clip >= 0) {
		const QRect r = clipRect(track, clip);
		const int edge = std::min(8, r.width() / 3);
		if (pos.x() - r.left() <= edge || r.right() - pos.x() <= edge)
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
		}
	}
}

void TimelineView::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	if (mode_ == Mode::Scrub) {
		mode_ = Mode::None;
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
		unsetCursor();
		clampView();
		updateGeometry();
		update();
		if (changed)
			emit clipsChanged();
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
	hoverMs_ = -1;
	update(); // drop the hover marker
}

// Every selected clip, primary included, as (track, clip).
QVector<QPair<int, int>> TimelineView::selectedPairs() const
{
	QVector<QPair<int, int>> out;
	if (selTrack_ >= 0 && selClip_ >= 0)
		out.append({selTrack_, selClip_});
	for (const auto &p : extraSel_)
		if (p != qMakePair(selTrack_, selClip_))
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

bool TimelineView::isSelected(int track, int clip) const
{
	return (track == selTrack_ && clip == selClip_) || extraSel_.contains({track, clip});
}

void TimelineView::selectClip(int track, int clip)
{
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
	emit clipsChanged();
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
			emit clipsChanged();
			return;
		}
	model_.markers.append(playheadMs_);
	std::sort(model_.markers.begin(), model_.markers.end());
	update();
	emit clipsChanged();
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
			t.clips.append(right);
			any = true;
		}
	}
	if (!any)
		return;
	extraSel_.clear();
	update();
	emit clipsChanged();
}

void TimelineView::deleteSelected()
{
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
	emit clipsChanged();
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
	t.clips.insert(clip + 1, b);
	selClip_ = clip + 1;
	update();
	emit selectionChanged(track, selClip_);
	emit clipsChanged();
}

void TimelineView::showClipMenu(int track, int clip, const QPoint &globalPos, qint64 atOutMs)
{
	const bool locked = model_.tracks[track].locked;
	QMenu menu(this);
	QAction *inspect = menu.addAction(QStringLiteral("Show in inspector"));
	menu.addSeparator();
	QAction *split = menu.addAction(QStringLiteral("Split here"));
	const TlClip &c = model_.tracks[track].clips[clip];
	split->setEnabled(!locked && atOutMs > c.outStartMs + kMinClipMs &&
			  atOutMs < c.outEndMs() - kMinClipMs);
	QAction *dup = menu.addAction(QStringLiteral("Duplicate"));
	dup->setEnabled(!locked);
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
	if (chosen == split) {
		splitClip(track, clip, atOutMs);
	} else if (chosen == dup) {
		TlClip copy = model_.tracks[track].clips[clip];
		copy.outStartMs = copy.outEndMs();
		model_.tracks[track].clips.insert(clip + 1, copy);
		selClip_ = clip + 1;
		emit selectionChanged(track, selClip_);
		update();
		emit clipsChanged();
	} else if (chosen == mute) {
		model_.tracks[track].muted = !model_.tracks[track].muted;
		update();
		emit clipsChanged();
	} else if (chosen == del) {
		selTrack_ = track;
		selClip_ = clip;
		deleteSelected();
	}
}

} // namespace harpia
