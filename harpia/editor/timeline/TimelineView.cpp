#include "TimelineView.hpp"

#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>

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
const QColor kWave(0x6f, 0xd0, 0xb0);
const QColor kPlayhead(0xe5, 0x48, 0x4d);
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

void TimelineView::addClip(TlTrack::Kind kind, const TlClip &clip)
{
	int idx = -1;
	for (int i = model_.tracks.size() - 1; i >= 0; --i)
		if (model_.tracks[i].kind == kind) {
			idx = i;
			break;
		}
	if (idx < 0) {
		TlTrack t;
		t.kind = kind;
		if (kind == TlTrack::Kind::Video) {
			int firstAudio = model_.tracks.size();
			for (int i = 0; i < model_.tracks.size(); ++i)
				if (model_.tracks[i].kind == TlTrack::Kind::Audio) {
					firstAudio = i;
					break;
				}
			int nVid = 0;
			for (const TlTrack &tr : model_.tracks)
				if (tr.kind == TlTrack::Kind::Video)
					++nVid;
			t.name = QStringLiteral("V%1").arg(nVid + 1);
			model_.tracks.insert(firstAudio, t);
			idx = firstAudio;
		} else {
			int nAud = 0;
			for (const TlTrack &tr : model_.tracks)
				if (tr.kind == TlTrack::Kind::Audio)
					++nAud;
			t.name = QStringLiteral("A%1").arg(nAud + 1);
			model_.tracks.append(t);
			idx = model_.tracks.size() - 1;
		}
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

qint64 TimelineView::snap(qint64 ms, int ignoreTrack, int ignoreClip) const
{
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

void TimelineView::drawClip(QPainter &p, int track, int clip) const
{
	const TlTrack &t = model_.tracks[track];
	const TlClip &c = t.clips[clip];
	const bool video = t.kind == TlTrack::Kind::Video;
	const bool sel = (track == selTrack_ && clip == selClip_);
	const QRect r = clipRect(track, clip);
	QPainterPath path;
	path.addRoundedRect(r, 4, 4);
	p.setPen(Qt::NoPen);
	p.setBrush(video ? (sel ? kVidFillSel : kVidFill) : (sel ? kAudFillSel : kAudFill));
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
			for (int x = r.x() + 1; x < r.right() - 1; x += tileW + 1) {
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
		for (int x = r.left() + 1; x < r.right() - 1; ++x) {
			const double tt = double(x - r.left()) / std::max(1, r.width());
			const double srcFrac = double(c.srcStartMs + tt * c.srcLenMs()) / double(total);
			const int b = std::clamp<int>(int(srcFrac * c.peaks.size()), 0, c.peaks.size() - 1);
			const int hh = int(c.peaks[b] * halfH);
			p.drawLine(x, midY - hh, x, midY + hh);
		}
	}
	// Label bar along the bottom.
	if (r.width() >= 28) {
		QFont sf = p.font();
		sf.setPixelSize(std::max(6, lp_.segFontPx));
		p.setFont(sf);
		p.fillRect(QRect(r.x(), r.bottom() - 12, r.width(), 13), QColor(0, 0, 0, 150));
		p.setPen(QColor(0xe6, 0xe6, 0xe6));
		const QString label = QStringLiteral("%1 · %2s").arg(c.sourceId).arg(c.outDurationMs() / 1000.0,
									       0, 'f', 1);
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

	// Lanes + headers.
	for (int i = 0; i < model_.tracks.size(); ++i) {
		const TlTrack &t = model_.tracks[i];
		const QRect lane = laneRect(i);
		p.setPen(Qt::NoPen);
		p.setBrush((i % 2) ? kLaneAlt : kLane);
		p.drawRect(lane);

		const QRect hdr = trackHeaderRect(i);
		p.setBrush(kGutter);
		p.drawRect(hdr);
		p.setPen(t.muted ? kCaption : QColor(0xe8, 0xea, 0xed));
		QFont hf = p.font();
		hf.setPixelSize(11);
		hf.setBold(true);
		p.setFont(hf);
		p.drawText(hdr.adjusted(8, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
			   t.name + (t.muted ? QStringLiteral(" (muted)") : QString()));

		for (int ci = 0; ci < t.clips.size(); ++ci)
			drawClip(p, i, ci);
	}

	if (model_.tracks.isEmpty()) {
		p.setPen(kCaption);
		p.drawText(contentRect(), Qt::AlignCenter,
			   QStringLiteral("Add a source to the timeline to start editing."));
	}

	drawRuler(p);

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

	// Ruler or gutter → scrub / nothing.
	if (pos.y() < lp_.margin + lp_.rulerH && pos.x() >= contentRect().x()) {
		mode_ = Mode::Scrub;
		playheadMs_ = xToMs(pos.x());
		emitScrubAt(playheadMs_);
		update();
		return;
	}
	if (pos.x() < contentRect().x())
		return; // gutter clicks ignored for now

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
		if (selTrack_ != -1 || selClip_ != -1) {
			selTrack_ = selClip_ = -1;
			emit selectionChanged(-1, -1);
		}
		emitScrubAt(playheadMs_);
		update();
		return;
	}

	selTrack_ = track;
	selClip_ = clip;
	emit selectionChanged(selTrack_, selClip_);
	pressPos_ = pos;
	dragMoved_ = false;
	dragTrack_ = track;
	dragClip_ = clip;
	dragOrig_ = model_.tracks[track].clips[clip];
	const QRect r = clipRect(track, clip);
	dragSrcPerPx_ = double(dragOrig_.srcLenMs()) / double(std::max(1, r.width()));
	dragGrabOffsetMs_ = xToMs(pos.x()) - dragOrig_.outStartMs;
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
		const qint64 srcTotal = srcThumbDur_.value(dragOrig_.sourceId, dragOrig_.srcEndMs + (1 << 30));

		if (mode_ == Mode::Move) {
			qint64 ns = std::max<qint64>(0, xToMs(pos.x()) - dragGrabOffsetMs_);
			const qint64 dur = c.outDurationMs();
			// Snap the start; if snapping the END lands closer, use that instead.
			const qint64 snapStart = snap(ns, dragTrack_, dragClip_);
			const qint64 snapEnd = snap(ns + dur, dragTrack_, dragClip_) - dur;
			ns = (std::llabs(snapEnd - ns) < std::llabs(snapStart - ns)) ? snapEnd : snapStart;
			c.outStartMs = std::max<qint64>(0, ns);
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
				emit hoverScrub(om);
			}
		}
	} else {
		unsetCursor();
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
		// On a move drag, if the cursor is over a different SAME-KIND lane, move
		// the clip element to that track.
		if (mode_ == Mode::Move && dragMoved_) {
			const int target = laneAtY(e->pos().y());
			if (target >= 0 && target != dragTrack_ &&
			    model_.tracks[target].kind == model_.tracks[dragTrack_].kind) {
				TlClip moved = model_.tracks[dragTrack_].clips.takeAt(dragClip_);
				model_.tracks[target].clips.append(moved);
				selTrack_ = target;
				selClip_ = model_.tracks[target].clips.size() - 1;
				emit selectionChanged(selTrack_, selClip_);
			}
		}
		const bool changed = dragMoved_;
		mode_ = Mode::None;
		dragTrack_ = dragClip_ = -1;
		dragMoved_ = false;
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

void TimelineView::leaveEvent(QEvent *)
{
	hoverMs_ = -1;
}

void TimelineView::deleteSelected()
{
	if (selTrack_ < 0 || selTrack_ >= model_.tracks.size())
		return;
	TlTrack &t = model_.tracks[selTrack_];
	if (selClip_ < 0 || selClip_ >= t.clips.size())
		return;
	t.clips.remove(selClip_);
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
	if (clip < 0 || clip >= t.clips.size())
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
	QMenu menu(this);
	QAction *split = menu.addAction(QStringLiteral("Split here"));
	const TlClip &c = model_.tracks[track].clips[clip];
	split->setEnabled(atOutMs > c.outStartMs + kMinClipMs && atOutMs < c.outEndMs() - kMinClipMs);
	QAction *dup = menu.addAction(QStringLiteral("Duplicate"));
	QAction *mute = menu.addAction(model_.tracks[track].muted ? QStringLiteral("Unmute track")
								 : QStringLiteral("Mute track"));
	menu.addSeparator();
	QAction *del = menu.addAction(QStringLiteral("Delete clip"));
	QAction *chosen = menu.exec(globalPos);
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
