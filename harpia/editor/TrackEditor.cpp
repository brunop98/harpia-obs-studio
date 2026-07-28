#include "TrackEditor.hpp"
#include "TimeText.hpp"

#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>

#include <utility>

namespace harpia {

namespace {
// Layout values live in TrackLayoutParams (lp_) so the Developer Panel can
// tweak them at runtime; only non-layout constants remain here.
constexpr qint64 kMinCutMs = 150;

// Colours all live in EditorColors (cl_), edited live from the Developer Panel.
} // namespace

TrackEditor::TrackEditor(QWidget *parent) : QWidget(parent)
{
	setFocusPolicy(Qt::ClickFocus); // so Delete works after clicking a segment
	setMouseTracking(true);         // cursor hints over the tracks
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setToolTip(QStringLiteral("Scroll to zoom, Shift+scroll to pan (both the source and output tracks)"));

	// ~60fps easing timer for smooth panning; runs only while the view is gliding.
	scrollAnim_ = new QTimer(this);
	scrollAnim_->setInterval(16);
	connect(scrollAnim_, &QTimer::timeout, this, &TrackEditor::animateScrollStep);
}

void TrackEditor::animateScrollStep()
{
	auto easeI = [](qint64 &cur, qint64 tgt) -> bool {
		if (cur == tgt)
			return false;
		qint64 step = qint64((tgt - cur) * 0.25); // ease-out: big steps, then small
		if (step == 0)
			step = (tgt > cur) ? 1 : -1;
		cur += step;
		if ((tgt > cur) != (step > 0)) // overshoot guard
			cur = tgt;
		return true;
	};
	auto easeZoom = [](double &cur, double tgt) -> bool {
		if (std::abs(cur - tgt) < 1e-4)
			return false;
		cur += (tgt - cur) * 0.25;
		if (std::abs(cur - tgt) / std::max(1.0, tgt) < 1e-3)
			cur = tgt;
		return true;
	};

	bool moving = false;

	// Source: zoom (anchor held under the cursor) takes priority over scroll.
	if (easeZoom(zoom_, zoomTarget_)) {
		viewStart_ = zoomAnchorMs_ - qint64(zoomAnchorFrac_ * visibleMs());
		clampView();
		viewTarget_ = viewStart_;
		moving = true;
	} else if (easeI(viewStart_, viewTarget_)) {
		moving = true;
	}

	// Output track, same rules.
	if (easeZoom(outZoom_, outZoomTarget_)) {
		outViewStart_ = outZoomAnchorMs_ - qint64(outZoomAnchorFrac_ * outVisibleMs());
		clampOutView();
		outViewTarget_ = outViewStart_;
		moving = true;
	} else if (easeI(outViewStart_, outViewTarget_)) {
		moving = true;
	}

	update();
	if (!moving)
		scrollAnim_->stop();
}

QSize TrackEditor::sizeHint() const
{
	return QSize(480, minimumSizeHint().height());
}

QSize TrackEditor::minimumSizeHint() const
{
	return QSize(240, 2 * lp_.margin + 2 * lp_.captionH + lp_.srcH + lp_.trackGap + lp_.outH);
}

void TrackEditor::setLayoutParams(const TrackLayoutParams &p)
{
	lp_ = p;
	zoom_ = std::clamp(zoom_, 1.0, lp_.maxZoom);
	clampView();
	viewTarget_ = viewStart_;
	zoomTarget_ = zoom_;
	outZoomTarget_ = outZoom_;
	stripCache_ = QPixmap(); // geometry changed — rebuild the filmstrip
	updateGeometry();        // min-size hint depends on the track heights
	update();
}

void TrackEditor::setDuration(qint64 ms)
{
	duration_ = std::max<qint64>(0, ms);
	// Reset the SOURCE-track view (this is the video the source track shows).
	// Leave the OUTPUT-track view intact — switching the active source must not
	// throw away the output timeline's zoom/scroll (the mix is unchanged).
	zoom_ = 1.0;
	zoomTarget_ = 1.0;
	viewStart_ = 0;
	viewTarget_ = 0;
	clampOutView();
	outViewTarget_ = outViewStart_;
	outZoomTarget_ = outZoom_;
	if (scrollAnim_)
		scrollAnim_->stop();
	update();
}

void TrackEditor::setThumbs(const QVector<QImage> &thumbs)
{
	thumbs_ = thumbs;
	++thumbsRev_; // invalidates the strip cache
	update();
}

void TrackEditor::setSourceThumbs(int sourceId, const QVector<QImage> &thumbs, qint64 durationMs)
{
	srcThumbs_[sourceId] = thumbs;
	srcThumbDur_[sourceId] = durationMs;
	// Cache the tile aspect once here instead of rescanning the strip every paint.
	double aspect = 16.0 / 9.0;
	for (const QImage &t : thumbs)
		if (!t.isNull()) {
			aspect = double(t.width()) / double(t.height());
			break;
		}
	srcAspect_[sourceId] = aspect;
	update(); // Output cut tiles for this source can now render
}

void TrackEditor::ensureStripCache(const QRect &src)
{
	const qreal dpr = devicePixelRatioF();
	if (!stripCache_.isNull() && stripCacheSize_ == src.size() && stripCacheZoom_ == zoom_ &&
	    stripCacheView_ == viewStart_ && stripCacheRev_ == thumbsRev_ && stripCacheDpr_ == dpr)
		return;
	stripCacheSize_ = src.size();
	stripCacheZoom_ = zoom_;
	stripCacheView_ = viewStart_;
	stripCacheRev_ = thumbsRev_;
	stripCacheDpr_ = dpr;

	stripCache_ = QPixmap(src.size() * dpr);
	stripCache_.setDevicePixelRatio(dpr);
	stripCache_.fill(Qt::transparent);
	QPainter cp(&stripCache_);
	cp.setRenderHint(QPainter::Antialiasing);
	const QRect local(0, 0, src.width(), src.height());
	cp.setPen(cl_.border);
	cp.setBrush(cl_.panelBg);
	cp.drawRoundedRect(local, 5, 5);

	if (!thumbs_.isEmpty() && duration_ > 0) {
		QPainterPath clip;
		clip.addRoundedRect(local, 5, 5);
		cp.setClipPath(clip);
		const int n = thumbs_.size();
		double aspect = 16.0 / 9.0;
		for (const QImage &t : thumbs_) {
			if (!t.isNull()) {
				aspect = double(t.width()) / double(t.height());
				break;
			}
		}
		const int tileH = local.height();
		const int tileW = std::max(8, int(tileH * aspect));
		const int tileGap = std::max(0, lp_.tileGap);
		const double sliceMs = double(duration_) / n;
		for (int x = 0; x < local.width(); x += tileW + tileGap) {
			const qint64 ms = xToMs(src.x() + x + tileW / 2);
			const int i = std::clamp(int(ms / sliceMs), 0, n - 1);
			if (!thumbs_[i].isNull())
				cp.drawImage(QRect(x, 0, tileW, tileH), thumbs_[i]);
		}
	}
}

qint64 TrackEditor::visibleMs() const
{
	return std::max<qint64>(1, qint64(duration_ / zoom_));
}

void TrackEditor::drawTimeRuler(QPainter &p, const QRect &bar, qint64 viewStart, qint64 visible) const
{
	if (visible <= 0 || bar.width() <= 0)
		return;
	// Pick a "nice" tick interval so labels are ~70px apart at the current zoom.
	static const qint64 kSteps[] = {200,   500,    1000,   2000,   5000,  10000,
					15000, 30000,  60000,  120000, 300000, 600000,
					900000, 1800000, 3600000};
	const double msPerPx = double(visible) / std::max(1, bar.width());
	const qint64 want = qint64(msPerPx * 70.0);
	qint64 step = kSteps[sizeof(kSteps) / sizeof(kSteps[0]) - 1];
	for (qint64 s : kSteps) {
		if (s >= want) {
			step = s;
			break;
		}
	}

	const auto label = timeTextRuler;
	auto toX = [&](qint64 t) {
		return bar.left() + int(double(t - viewStart) / double(visible) * bar.width());
	};

	// A subtle dark band across the top of the strip keeps labels legible.
	const int bandH = 12;
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0, 0, 0, 90));
	p.drawRect(QRect(bar.left(), bar.top(), bar.width(), bandH));

	QFont rf = p.font();
	rf.setPixelSize(9);
	p.setFont(rf);

	const qint64 first = (viewStart / step) * step;
	for (qint64 t = first; t <= viewStart + visible; t += step) {
		if (t < 0)
			continue;
		const int x = toX(t);
		if (x < bar.left() - 1 || x > bar.right() + 1)
			continue;
		p.setPen(QColor(0xff, 0xff, 0xff, 60));
		p.drawLine(x, bar.top(), x, bar.top() + bandH); // tick
		p.setPen(QColor(0xc8, 0xcc, 0xd2));
		p.drawText(x + 2, bar.top() + bandH - 2, label(t)); // label
	}
}

void TrackEditor::clampView()
{
	viewStart_ = std::clamp<qint64>(viewStart_, 0, std::max<qint64>(0, duration_ - visibleMs()));
}

qint64 TrackEditor::outVisibleMs() const
{
	return std::max<qint64>(1, qint64(totalOutputMs() / outZoom_));
}

int TrackEditor::outMsToX(qint64 ms) const
{
	const QRect r = outputRect();
	if (totalOutputMs() <= 0 || r.width() <= 0)
		return r.x();
	return r.x() + int(double(ms - outViewStart_) / double(outVisibleMs()) * r.width());
}

qint64 TrackEditor::outXToMs(int x) const
{
	const QRect r = outputRect();
	const qint64 total = totalOutputMs();
	if (total <= 0 || r.width() <= 0)
		return 0;
	const double t = double(x - r.x()) / double(r.width());
	return std::clamp<qint64>(outViewStart_ + qint64(std::llround(t * outVisibleMs())), 0, total);
}

void TrackEditor::clampOutView()
{
	outViewStart_ = std::clamp<qint64>(outViewStart_, 0,
					   std::max<qint64>(0, totalOutputMs() - outVisibleMs()));
}

void TrackEditor::wheelEvent(QWheelEvent *e)
{
	const QPoint pos = e->position().toPoint();
	const bool onSource = duration_ > 0 && sourceRect().contains(pos);
	const bool onOutput = totalOutputMs() > 0 && outputRect().contains(pos);
	if (!onSource && !onOutput) {
		e->ignore();
		return;
	}
	const QPoint ad = e->angleDelta();
	// Plain scroll = zoom (up in, down out); Shift+scroll or a horizontal
	// wheel/touchpad axis pans the zoomed view.
	const bool pan = (e->modifiers() & Qt::ShiftModifier) || qAbs(ad.x()) > qAbs(ad.y());
	const int delta = pan ? (ad.x() != 0 ? ad.x() : ad.y()) : ad.y();
	if (delta == 0)
		return;
	const double steps = delta / 120.0;
	const int x = pos.x();

	if (onSource) {
		if (!pan) {
			// Smooth zoom: glide zoom_ toward a target while holding the ms under
			// the cursor fixed (recomputed each animation frame).
			const QRect r = sourceRect();
			zoomAnchorMs_ = xToMs(x);
			zoomAnchorFrac_ = std::clamp(double(x - r.x()) / std::max(1, r.width()), 0.0, 1.0);
			zoomTarget_ = std::clamp(zoomTarget_ * std::pow(1.3, steps), 1.0, lp_.maxZoom);
			scrollAnim_->start();
		} else {
			// Pan: glide toward a target instead of snapping there.
			viewTarget_ -= qint64(steps * visibleMs() * 0.15);
			viewTarget_ = std::clamp<qint64>(viewTarget_, 0,
							 std::max<qint64>(0, duration_ - visibleMs()));
			scrollAnim_->start();
		}
		if (mode_ == Mode::None) { // preview the frame under the cursor
			hoverMs_ = xToMs(x);
			emitHover(hoverMs_, activeSourceId_);
		}
	} else { // onOutput — same controls over the assembled output timeline
		if (!pan) {
			const QRect r = outputRect();
			outZoomAnchorMs_ = outXToMs(x);
			outZoomAnchorFrac_ =
				std::clamp(double(x - r.x()) / std::max(1, r.width()), 0.0, 1.0);
			outZoomTarget_ = std::clamp(outZoomTarget_ * std::pow(1.3, steps), 1.0, lp_.maxZoom);
			scrollAnim_->start();
		} else {
			outViewTarget_ -= qint64(steps * outVisibleMs() * 0.15);
			outViewTarget_ = std::clamp<qint64>(
				outViewTarget_, 0, std::max<qint64>(0, totalOutputMs() - outVisibleMs()));
			scrollAnim_->start();
		}
		if (mode_ == Mode::None) { // preview the output frame under the cursor
			qint64 srcMs = 0;
			const int seg = sourceForOutput(outXToMs(x), &srcMs);
			if (seg >= 0) {
				hoverOutSeg_ = seg;
				hoverOutX_ = x;
				emitHover(srcMs, segs_[seg].sourceId);
			}
		}
	}
	update();
	e->accept();
}

void TrackEditor::setSegmentSpeed(int index, double speed)
{
	if (index < 0 || index >= segs_.size())
		return;
	segs_[index].speed = std::clamp(speed, 0.1, 50.0);
	++segsRev_;
	update();
}

QList<int> TrackEditor::selectedIndices() const
{
	QList<int> list(multiSel_.begin(), multiSel_.end());
	std::sort(list.begin(), list.end());
	return list;
}

void TrackEditor::setSegments(const QVector<CutSegment> &segs)
{
	segs_ = segs;
	++segsRev_;
	selected_ = -1;
	multiSel_.clear();
	playheadOutMs_ = -1;
	update();
}

void TrackEditor::addSegments(const QVector<CutSegment> &segs)
{
	if (segs.isEmpty())
		return;
	segs_ += segs;
	++segsRev_;
	selected_ = segs_.size() - 1;
	multiSel_ = QSet<int>{selected_};
	emit segmentsChanged();
	emit selectionChanged(selected_);
	update();
}

void TrackEditor::removeSegment(int index)
{
	if (index < 0 || index >= segs_.size())
		return;
	segs_.remove(index);
	++segsRev_;
	selected_ = -1;
	multiSel_.clear();
	emit segmentsChanged();
	emit selectionChanged(-1);
	update();
}

void TrackEditor::removeSelected()
{
	if (multiSel_.isEmpty())
		return;
	QList<int> list = selectedIndices();
	for (int i = list.size() - 1; i >= 0; --i)
		segs_.remove(list[i]);
	++segsRev_;
	selected_ = -1;
	multiSel_.clear();
	emit segmentsChanged();
	emit selectionChanged(-1);
	update();
}

qint64 TrackEditor::totalOutputMs() const
{
	// Memoized: recomputed only when the cut list changes (segsRev_). This is
	// called ~8× per paint plus inside outVisibleMs/clampOutView/segmentRects.
	if (totalOutCacheRev_ != segsRev_) {
		qint64 total = 0;
		for (const CutSegment &s : segs_)
			total += s.outDurationMs();
		totalOutCache_ = total;
		totalOutCacheRev_ = segsRev_;
	}
	return totalOutCache_;
}

qint64 TrackEditor::outputStartOf(int index) const
{
	qint64 acc = 0;
	for (int i = 0; i < index && i < segs_.size(); ++i)
		acc += segs_[i].outDurationMs();
	return acc;
}

int TrackEditor::sourceForOutput(qint64 outMs, qint64 *srcMs) const
{
	if (segs_.isEmpty())
		return -1;
	qint64 acc = 0;
	for (int i = 0; i < segs_.size(); ++i) {
		const qint64 d = segs_[i].outDurationMs();
		if (outMs < acc + d || i == segs_.size() - 1) {
			const qint64 off = std::clamp<qint64>(outMs - acc, 0, d);
			if (srcMs)
				*srcMs = segs_[i].srcStartMs +
					 qint64(std::llround(double(off) * segs_[i].speed));
			return i;
		}
		acc += d;
	}
	return -1; // unreachable
}

void TrackEditor::setPlayhead(qint64 outMs)
{
	playheadOutMs_ = outMs;
	update();
}

void TrackEditor::clearPlayhead()
{
	playheadOutMs_ = -1;
	update();
}

QRect TrackEditor::sourceRect() const
{
	return QRect(lp_.margin, lp_.margin + lp_.captionH, width() - 2 * lp_.margin, lp_.srcH);
}

QRect TrackEditor::outputRect() const
{
	return QRect(lp_.margin, lp_.margin + lp_.captionH + lp_.srcH + lp_.trackGap + lp_.captionH,
		     width() - 2 * lp_.margin, lp_.outH);
}

int TrackEditor::msToX(qint64 ms) const
{
	const QRect r = sourceRect();
	if (duration_ <= 0 || r.width() <= 0)
		return r.x();
	return r.x() + int(double(ms - viewStart_) / double(visibleMs()) * r.width());
}

qint64 TrackEditor::xToMs(int x) const
{
	const QRect r = sourceRect();
	if (duration_ <= 0 || r.width() <= 0)
		return 0;
	const double t = double(x - r.x()) / double(r.width());
	return std::clamp<qint64>(viewStart_ + qint64(std::llround(t * visibleMs())), 0, duration_);
}

const QVector<QRect> &TrackEditor::segmentRects() const
{
	// Output-time axis: each cut occupies [outputStartOf(i), +outDuration] mapped
	// through the output zoom/view, so the track zooms and pans like the source.
	// Memoized: the result depends only on the cut list (segsRev_) and the output
	// view/geometry, so identical paints and mouse-moves reuse the cached vector.
	const QRect r = outputRect();
	const int gap = std::max(0, lp_.segGap);
	if (segRectsRev_ == segsRev_ && segRectsSize_ == r.size() && segRectsZoom_ == outZoom_ &&
	    segRectsView_ == outViewStart_ && segRectsGap_ == gap)
		return segRectsCache_;
	segRectsRev_ = segsRev_;
	segRectsSize_ = r.size();
	segRectsZoom_ = outZoom_;
	segRectsView_ = outViewStart_;
	segRectsGap_ = gap;

	segRectsCache_.clear();
	const int n = segs_.size();
	if (!n)
		return segRectsCache_;
	const qint64 total = totalOutputMs();
	const qint64 vis = std::max<qint64>(1, qint64(total / outZoom_));
	const double scale = double(r.width()) / double(vis);
	auto mapX = [&](qint64 ms) {
		return (total <= 0 || r.width() <= 0) ? r.x() : r.x() + int(double(ms - outViewStart_) * scale);
	};
	segRectsCache_.reserve(n);
	qint64 acc = 0;
	for (int i = 0; i < n; ++i) {
		const qint64 d = segs_[i].outDurationMs();
		const int x1 = mapX(acc);
		const int x2 = mapX(acc + d);
		segRectsCache_.append(QRect(x1, r.y(), std::max(2, x2 - x1 - gap), r.height()));
		acc += d;
	}
	return segRectsCache_;
}

int TrackEditor::segmentAt(const QPoint &p) const
{
	const QVector<QRect> &rects = segmentRects();
	for (int i = 0; i < rects.size(); ++i) {
		if (rects[i].contains(p))
			return i;
	}
	return -1;
}

int TrackEditor::insertSlotAt(int x) const
{
	const QVector<QRect> &rects = segmentRects();
	int slot = 0;
	for (int i = 0; i < rects.size(); ++i) {
		if (x > rects[i].center().x())
			slot = i + 1;
	}
	return slot;
}

void TrackEditor::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	QFont capFont = font();
	capFont.setPixelSize(std::max(6, lp_.captionFontPx));
	p.setFont(capFont);

	clampOutView(); // keep the output window valid after edits/resizes
	outViewTarget_ = outViewStart_;

	const QRect src = sourceRect();
	const QRect out = outputRect();

	// ---- Captions --------------------------------------------------------
	p.setPen(cl_.caption);
	p.drawText(QRect(src.x(), lp_.margin, src.width(), lp_.captionH),
		   Qt::AlignVCenter | Qt::AlignLeft,
		   QStringLiteral("Source — press and drag to select a section to keep"));
	QString outCaption = QStringLiteral("Output — %1 cut%2, %3s")
				     .arg(segs_.size())
				     .arg(segs_.size() == 1 ? QString() : QStringLiteral("s"))
				     .arg(totalOutputMs() / 1000.0, 0, 'f', 1);
	if (!segs_.isEmpty())
		outCaption += QStringLiteral(
			"   (drag to reorder · drag edge to trim · Shift+edge to change speed · Del to delete)");
	p.drawText(QRect(out.x(), out.y() - lp_.captionH, out.width(), lp_.captionH),
		   Qt::AlignVCenter | Qt::AlignLeft, outCaption);

	// ---- Source track ----------------------------------------------------
	// Background + filmstrip come from the render cache (rebuilt only when the
	// view/zoom/thumbs change) — repaints are a blit, not 20+ image rescales.
	ensureStripCache(src);
	p.drawPixmap(src.topLeft(), stripCache_);

	{
		QPainterPath clip;
		clip.addRoundedRect(src, 5, 5);
		p.save();
		p.setClipPath(clip);

		// Parts of THIS source already used in the output — only the active
		// source's cuts map onto this timeline (others live in other videos).
		for (int i = 0; i < segs_.size(); ++i) {
			if (segs_[i].sourceId != activeSourceId_)
				continue;
			const int x1 = msToX(segs_[i].srcStartMs);
			const int x2 = msToX(segs_[i].srcEndMs);
			QColor fill = cl_.accent;
			fill.setAlpha(i == selected_ ? 110 : 60);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			p.drawRect(QRect(QPoint(x1, src.y() + 2), QPoint(x2, src.bottom() - 2)));
		}
		// The in-progress drag selection.
		if (mode_ == Mode::CreatingCut) {
			const int x1 = msToX(std::min(dragStartMs_, dragCurMs_));
			const int x2 = msToX(std::max(dragStartMs_, dragCurMs_));
			QColor fill = cl_.accent;
			fill.setAlpha(140);
			p.setPen(QPen(cl_.accent, 1));
			p.setBrush(fill);
			p.drawRect(QRect(QPoint(x1, src.y() + 1), QPoint(x2, src.bottom() - 1)));
		}
		// Hover marker — the previewed frame's position.
		if (hoverMs_ >= 0 && mode_ == Mode::None) {
			const int hx = msToX(hoverMs_);
			p.setPen(QPen(QColor(0xff, 0xff, 0xff, 170), 1));
			p.drawLine(hx, src.y() + 1, hx, src.bottom() - 1);
		}
		// Time ruler (ticks + labels) over the top of the source strip.
		drawTimeRuler(p, src, viewStart_, visibleMs());
		p.restore();
	}

	// Zoom indicator: which part of the clip the source track is showing.
	if (zoom_ > 1.001 && duration_ > 0) {
		const int y = src.bottom() + 2;
		p.setPen(Qt::NoPen);
		p.setBrush(cl_.panelBg);
		p.drawRect(QRect(src.left(), y, src.width(), 2));
		p.setBrush(cl_.caption);
		const int ix = src.left() + int(double(viewStart_) / duration_ * src.width());
		const int iw = std::max(8, int(double(visibleMs()) / duration_ * src.width()));
		p.drawRect(QRect(ix, y, iw, 2));
	}

	// ---- Output track ----------------------------------------------------
	p.setPen(cl_.border);
	p.setBrush(cl_.panelBg);
	p.drawRoundedRect(out, 5, 5);

	if (segs_.isEmpty()) {
		p.setPen(cl_.caption);
		p.drawText(out, Qt::AlignCenter,
			   QStringLiteral("Cuts you select above appear here in order"));
	}

	p.save();
	p.setClipRect(out.adjusted(1, 0, -1, 0));
	const QVector<QRect> &rects = segmentRects();
	QFont segFont = font();
	segFont.setPixelSize(std::max(6, lp_.segFontPx));
	for (int i = 0; i < rects.size(); ++i) {
		const QRect r = rects[i].adjusted(0, 3, 0, -3);
		const bool sel = multiSel_.contains(i);
		p.setPen(Qt::NoPen);
		p.setBrush(sel ? cl_.videoClipSel : cl_.videoClip);
		p.drawRoundedRect(r, 4, 4);

		// Filmstrip across the whole cut, sampled from the cut's OWN source, so a
		// long segment shows its progression (not just one frame) and a mix shows
		// the right frames regardless of which source is active. If the source's
		// strip hasn't decoded yet, the block stays plain.
		const auto tIt = srcThumbs_.constFind(segs_[i].sourceId);
		const qint64 srcDur = srcThumbDur_.value(segs_[i].sourceId, 0);
		if (tIt != srcThumbs_.constEnd() && !tIt.value().isEmpty() && srcDur > 0) {
			const QVector<QImage> &strip = tIt.value();
			const double aspect = srcAspect_.value(segs_[i].sourceId, 16.0 / 9.0);
			const int th = r.height() - 2;
			const int tileW = std::max(8, int(th * aspect));
			const int tileGap = std::max(0, lp_.tileGap); // same gap as the source strip
			const qint64 s0 = segs_[i].srcStartMs, s1 = segs_[i].srcEndMs;
			QPainterPath segClip;
			segClip.addRoundedRect(r, 4, 4);
			p.save();
			p.setClipPath(segClip);
			for (int x = r.x() + 1; x < r.right() - 1; x += tileW + tileGap) {
				// This tile's center → source-time within the cut → strip index.
				const double f = std::clamp(
					double(x + tileW / 2 - r.x()) / std::max(1, r.width()), 0.0, 1.0);
				const qint64 ms = s0 + qint64(f * double(s1 - s0));
				const int ti =
					std::clamp(int(double(ms) / srcDur * strip.size()), 0,
						   int(strip.size()) - 1);
				if (!strip[ti].isNull())
					p.drawImage(QRect(x, r.y() + 1, tileW, th), strip[ti]);
			}
			p.restore();
		}

		// Label overlay: a dark bar along the bottom with cut #, speed, duration.
		if (r.width() >= 26) {
			const QString label = QStringLiteral("#%1 · %2× · %3s")
						      .arg(i + 1)
						      .arg(segs_[i].speed, 0, 'g', 3)
						      .arg(segs_[i].outDurationMs() / 1000.0, 0, 'f', 1);
			p.setFont(segFont);
			QPainterPath segClip;
			segClip.addRoundedRect(r, 4, 4);
			p.save();
			p.setClipPath(segClip);
			p.fillRect(QRect(r.x(), r.bottom() - 12, r.width(), 13), QColor(0, 0, 0, 160));
			p.setPen(QColor(0xe6, 0xe6, 0xe6));
			p.drawText(QRect(r.x() + 3, r.bottom() - 12, r.width() - 5, 13),
				   Qt::AlignVCenter | Qt::AlignLeft,
				   p.fontMetrics().elidedText(label, Qt::ElideRight, r.width() - 5));
			p.restore();
		}

		// Selection border on top so it stays visible over the thumbnail.
		p.setPen(sel ? QPen(cl_.accent, 2) : QPen(cl_.border, 1));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(r, 4, 4);
	}

	// Reorder visuals while dragging a segment: dim the one being moved, draw a
	// bold insertion caret at the drop slot, and float a ghost under the cursor.
	if (mode_ == Mode::DraggingSegment && dragMoved_ && dragInsertSlot_ >= 0) {
		// Dim the segment at its original position so it reads as "in flight".
		if (selected_ >= 0 && selected_ < rects.size()) {
			p.save();
			QPainterPath clip;
			clip.addRoundedRect(rects[selected_], 4, 4);
			p.setClipPath(clip);
			p.fillRect(rects[selected_], QColor(0, 0, 0, 130));
			p.restore();
		}

		// Insertion caret at the drop slot, with a triangle marker top & bottom.
		int cx;
		if (rects.isEmpty())
			cx = out.x();
		else if (dragInsertSlot_ >= rects.size())
			cx = rects.last().right() + lp_.segGap / 2 + 1;
		else
			cx = rects[dragInsertSlot_].left() - lp_.segGap / 2 - 1;
		p.setPen(QPen(cl_.accent, 3));
		p.drawLine(cx, out.y(), cx, out.bottom());
		QPainterPath caret;
		caret.moveTo(cx - 5, out.y());
		caret.lineTo(cx + 5, out.y());
		caret.lineTo(cx, out.y() + 6);
		caret.closeSubpath();
		caret.moveTo(cx - 5, out.bottom());
		caret.lineTo(cx + 5, out.bottom());
		caret.lineTo(cx, out.bottom() - 6);
		caret.closeSubpath();
		p.setPen(Qt::NoPen);
		p.setBrush(cl_.accent);
		p.drawPath(caret);

		// A translucent ghost of the dragged segment following the cursor.
		if (selected_ >= 0 && selected_ < rects.size() && dragGhostX_ >= 0) {
			const int gw = rects[selected_].width();
			QRect g(dragGhostX_ - gw / 2, out.y() + 2, gw, out.height() - 4);
			p.save();
			p.setOpacity(0.8);
			p.setPen(QPen(cl_.accent, 2));
			p.setBrush(QColor(cl_.accent.red(), cl_.accent.green(), cl_.accent.blue(), 70));
			p.drawRoundedRect(g, 4, 4);
			p.setPen(QColor(0xff, 0xff, 0xff));
			p.drawText(g, Qt::AlignCenter, QStringLiteral("#%1").arg(selected_ + 1));
			p.restore();
		}
	}

	// Output playhead (mapped through the output zoom/view).
	if (playheadOutMs_ >= 0 && !segs_.isEmpty()) {
		const int px = outMsToX(playheadOutMs_);
		p.setPen(QPen(cl_.playhead, 2));
		p.drawLine(px, out.y() + 1, px, out.bottom() - 1);
	}

	// Hover marker on the output track — mirrors the source-track marker so
	// it's obvious which frame is being previewed.
	if (hoverOutSeg_ >= 0 && hoverOutSeg_ < rects.size() && mode_ == Mode::None) {
		const QRect hr = rects[hoverOutSeg_];
		const int hx = std::clamp(hoverOutX_, hr.left() + 1, hr.right() - 1);
		p.setPen(QPen(QColor(0xff, 0xff, 0xff, 170), 1));
		p.drawLine(hx, hr.top() + 4, hx, hr.bottom() - 4);
	}

	// Time ruler over the output track.
	if (!segs_.isEmpty())
		drawTimeRuler(p, out, outViewStart_, outVisibleMs());
	p.restore();

	// Output zoom indicator: which part of the assembled output is visible.
	if (outZoom_ > 1.001 && totalOutputMs() > 0) {
		const int y = out.bottom() + 2;
		p.setPen(Qt::NoPen);
		p.setBrush(cl_.panelBg);
		p.drawRect(QRect(out.left(), y, out.width(), 2));
		p.setBrush(cl_.caption);
		const int ix = out.left() + int(double(outViewStart_) / totalOutputMs() * out.width());
		const int iw = std::max(8, int(double(outVisibleMs()) / totalOutputMs() * out.width()));
		p.drawRect(QRect(ix, y, iw, 2));
	}
}

void TrackEditor::mousePressEvent(QMouseEvent *e)
{
	const QPoint pos = e->pos();

	if (e->button() == Qt::RightButton) {
		const int idx = segmentAt(pos);
		if (idx >= 0) {
			if (selected_ != idx) {
				selected_ = idx;
				emit selectionChanged(idx);
				update();
			}
			showSegmentMenu(idx, e->globalPosition().toPoint(), pos);
		} else if (duration_ > 0 && sourceRect().contains(pos)) {
			// Source-track menu: auto-populate cuts from scene changes.
			QMenu menu(this);
			QAction *autoCut = menu.addAction(QStringLiteral("Auto-cut on scene changes…"));
			if (menu.exec(e->globalPosition().toPoint()) == autoCut)
				emit autoCutRequested();
		}
		return;
	}
	if (e->button() != Qt::LeftButton)
		return;

	if (duration_ > 0 && sourceRect().contains(pos)) {
		mode_ = Mode::CreatingCut;
		dragStartMs_ = dragCurMs_ = xToMs(pos.x());
		emitScrub(dragCurMs_, activeSourceId_);
		update();
		return;
	}

	if (outputRect().contains(pos)) {
		const QVector<QRect> &segRects = segmentRects(); // computed once per press
		int idx = -1;
		for (int i = 0; i < segRects.size(); ++i) {
			if (segRects[i].contains(pos)) {
				idx = i;
				break;
			}
		}

		if (idx >= 0 && (e->modifiers() & Qt::ControlModifier)) {
			// Ctrl+click toggles membership; no drag starts.
			if (multiSel_.contains(idx)) {
				multiSel_.remove(idx);
				selected_ = multiSel_.isEmpty() ? -1 : *multiSel_.begin();
			} else {
				multiSel_.insert(idx);
				selected_ = idx;
				emitScrub(segs_[idx].srcStartMs, segs_[idx].sourceId);
			}
			emit selectionChanged(selected_);
			update();
			return;
		}
		if (idx >= 0 && (e->modifiers() & Qt::ShiftModifier) && selected_ >= 0) {
			// Shift+click selects the range between the primary and here.
			for (int i = std::min(selected_, idx); i <= std::max(selected_, idx); ++i)
				multiSel_.insert(i);
			selected_ = idx;
			emit selectionChanged(selected_);
			emitScrub(segs_[idx].srcStartMs, segs_[idx].sourceId);
			update();
			return;
		}

		// Plain click: keep an existing multi-selection when grabbing one of its
		// members, otherwise select just this segment.
		if (idx < 0 || !multiSel_.contains(idx))
			multiSel_ = (idx >= 0) ? QSet<int>{idx} : QSet<int>();
		if (idx != selected_) {
			selected_ = idx;
			emit selectionChanged(idx);
		}
		if (idx >= 0) {
			pressPos_ = pos;
			dragMoved_ = false;
			dragInsertSlot_ = -1;
			// Near an edge → trim that boundary (with live frame preview);
			// otherwise drag the whole segment to reorder.
			const QRect r = segRects[idx];
			const CutSegment &seg = segs_[idx];
			const int edgeZone = std::min(7, r.width() / 3);
			const bool onLeft = pos.x() - r.left() <= edgeZone;
			const bool onRight = r.right() - pos.x() <= edgeZone;
			if (onLeft || onRight) {
				mode_ = onLeft ? Mode::ResizingLeft : Mode::ResizingRight;
				resizeOrigStart_ = seg.srcStartMs;
				resizeOrigEnd_ = seg.srcEndMs;
				resizeSrcPerPx_ = double(seg.srcEndMs - seg.srcStartMs) /
						  double(std::max(1, r.width()));
				resizeOrigOutDur_ = seg.outDurationMs();
				resizeOutPerPx_ =
					double(outVisibleMs()) / double(std::max(1, outputRect().width()));
				emitScrub(onLeft ? seg.srcStartMs : seg.srcEndMs, seg.sourceId);
			} else {
				mode_ = Mode::DraggingSegment;
				// Simple click: move the output playhead to exactly where you
				// clicked and preview that frame (not just the segment's start).
				const double f = std::clamp(
					double(pos.x() - r.left()) / std::max(1, r.width()), 0.0, 1.0);
				const qint64 outMs = outputStartOf(idx) +
						     qint64(f * segs_[idx].outDurationMs());
				playheadOutMs_ = outMs;
				qint64 srcMs = seg.srcStartMs;
				sourceForOutput(outMs, &srcMs);
				emitScrub(srcMs, seg.sourceId);
			}
		}
		update();
	}
}

void TrackEditor::mouseMoveEvent(QMouseEvent *e)
{
	const QPoint pos = e->pos();

	if (mode_ == Mode::CreatingCut) {
		dragCurMs_ = xToMs(pos.x());
		emitScrub(dragCurMs_, activeSourceId_);
		update();
		return;
	}
	if (mode_ == Mode::DraggingSegment) {
		if (!dragMoved_ && (pos - pressPos_).manhattanLength() > 6)
			dragMoved_ = true;
		if (dragMoved_) {
			setCursor(Qt::ClosedHandCursor); // "grabbing" while reordering
			dragInsertSlot_ = insertSlotAt(pos.x());
			dragGhostX_ = pos.x();
			update();
		}
		return;
	}
	if ((mode_ == Mode::ResizingLeft || mode_ == Mode::ResizingRight) && selected_ >= 0) {
		CutSegment &seg = segs_[selected_];

		// Shift+trim = re-time: keep the source range, change SPEED so the same
		// content fits the new output width the drag defines.
		if (e->modifiers() & Qt::ShiftModifier) {
			const double deltaPx = pos.x() - pressPos_.x();
			double newOutDur = (mode_ == Mode::ResizingRight)
						   ? resizeOrigOutDur_ + deltaPx * resizeOutPerPx_
						   : resizeOrigOutDur_ - deltaPx * resizeOutPerPx_;
			const double srcRange = double(resizeOrigEnd_ - resizeOrigStart_);
			// Bound the width so speed stays within 0.1×..50×.
			newOutDur = std::clamp(newOutDur, srcRange / 50.0, srcRange / 0.1);
			seg.srcStartMs = resizeOrigStart_; // source content unchanged
			seg.srcEndMs = resizeOrigEnd_;
			seg.speed = std::clamp(srcRange / std::max(1.0, newOutDur), 0.1, 50.0);
			++segsRev_;
			emitScrub(mode_ == Mode::ResizingLeft ? seg.srcStartMs : seg.srcEndMs, seg.sourceId);
			dragMoved_ = true;
			update();
			return;
		}

		// Live edge trim: convert the pixel delta into source ms with the scale
		// captured at press, clamp, and preview the frame at the moving edge.
		const qint64 deltaMs = qint64(std::llround((pos.x() - pressPos_.x()) * resizeSrcPerPx_));
		if (mode_ == Mode::ResizingLeft) {
			seg.srcStartMs = std::clamp<qint64>(resizeOrigStart_ + deltaMs, 0,
							    resizeOrigEnd_ - kMinCutMs);
			emitScrub(seg.srcStartMs, seg.sourceId);
		} else {
			seg.srcEndMs = std::clamp<qint64>(resizeOrigEnd_ + deltaMs,
							  resizeOrigStart_ + kMinCutMs, duration_);
			emitScrub(seg.srcEndMs, seg.sourceId);
		}
		++segsRev_;
		dragMoved_ = true;
		update();
		return;
	}

	// Idle: cursor hints + hover preview (no click needed to see a frame).
	// segmentRects() is computed ONCE per event and repaints are limited to
	// the marker areas that actually changed.
	qint64 newHover = -1;
	int newOutSeg = -1;
	int newOutX = -1;
	const QVector<QRect> &rects = segmentRects();
	if (sourceRect().contains(pos)) {
		setCursor(Qt::CrossCursor);
		if (e->buttons() == Qt::NoButton && duration_ > 0) {
			newHover = xToMs(pos.x());
			emitHover(newHover, activeSourceId_);
		}
	} else {
		int idx = -1;
		for (int i = 0; i < rects.size(); ++i) {
			if (rects[i].contains(pos)) {
				idx = i;
				break;
			}
		}
		if (idx >= 0) {
			const QRect r = rects[idx];
			const int edgeZone = std::min(7, r.width() / 3);
			if (pos.x() - r.left() <= edgeZone || r.right() - pos.x() <= edgeZone)
				setCursor(Qt::SizeHorCursor);
			else
				setCursor(Qt::PointingHandCursor);
			if (e->buttons() == Qt::NoButton) {
				// Hovering along a cut previews within its source range; the
				// source-track marker shows where that frame comes from.
				const double f = std::clamp(
					double(pos.x() - r.x()) / std::max(1, r.width()), 0.0, 1.0);
				const CutSegment &s = segs_[idx];
				newOutSeg = idx;
				newOutX = pos.x();
				newHover = s.srcStartMs +
					   qint64(f * double(s.srcEndMs - s.srcStartMs));
				emitHover(newHover, s.sourceId);
			}
		} else {
			unsetCursor();
		}
	}
	if (newHover != hoverMs_ || newOutSeg != hoverOutSeg_ || newOutX != hoverOutX_) {
		QRegion dirty;
		const QRect src = sourceRect();
		if (hoverMs_ >= 0)
			dirty += QRect(msToX(hoverMs_) - 2, src.y(), 5, src.height());
		if (newHover >= 0)
			dirty += QRect(msToX(newHover) - 2, src.y(), 5, src.height());
		if (hoverOutSeg_ >= 0 && hoverOutSeg_ < rects.size())
			dirty += rects[hoverOutSeg_].adjusted(-2, -2, 2, 2);
		if (newOutSeg >= 0 && newOutSeg < rects.size())
			dirty += rects[newOutSeg].adjusted(-2, -2, 2, 2);
		hoverMs_ = newHover;
		hoverOutSeg_ = newOutSeg;
		hoverOutX_ = newOutX;
		update(dirty);
	}
}

void TrackEditor::leaveEvent(QEvent *)
{
	if (hoverMs_ >= 0 || hoverOutSeg_ >= 0) {
		hoverMs_ = -1;
		hoverOutSeg_ = -1;
		hoverOutX_ = -1;
		update();
	}
}

void TrackEditor::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;

	if (mode_ == Mode::CreatingCut) {
		mode_ = Mode::None;
		const qint64 a = std::min(dragStartMs_, dragCurMs_);
		const qint64 b = std::max(dragStartMs_, dragCurMs_);
		if (b - a >= kMinCutMs) {
			CutSegment seg;
			seg.srcStartMs = a;
			seg.srcEndMs = b;
			seg.sourceId = activeSourceId_;
			segs_.append(seg);
			++segsRev_;
			selected_ = segs_.size() - 1;
			multiSel_ = QSet<int>{selected_};
			emit segmentsChanged();
			emit selectionChanged(selected_);
		}
		update();
		return;
	}

	if (mode_ == Mode::DraggingSegment) {
		mode_ = Mode::None;
		if (dragMoved_ && selected_ >= 0 && dragInsertSlot_ >= 0) {
			int to = dragInsertSlot_;
			if (to > selected_)
				--to; // removing the item shifts later slots left
			if (to != selected_ && to >= 0 && to < segs_.size()) {
				segs_.move(selected_, to);
				++segsRev_;
				selected_ = to;
				// Indices shifted — collapse the selection to the moved cut.
				multiSel_ = QSet<int>{selected_};
				emit segmentsChanged();
				emit selectionChanged(selected_);
			}
		}
		dragInsertSlot_ = -1;
		dragGhostX_ = -1;
		dragMoved_ = false;
		unsetCursor(); // drop the "grabbing" cursor; hover logic re-hints
		update();
		return;
	}

	if (mode_ == Mode::ResizingLeft || mode_ == Mode::ResizingRight) {
		mode_ = Mode::None;
		if (dragMoved_) {
			emit segmentsChanged(); // durations changed
			if (selected_ >= 0)
				emit selectionChanged(selected_); // refresh the speed slider row
		}
		dragMoved_ = false;
		update();
	}
}

void TrackEditor::keyPressEvent(QKeyEvent *e)
{
	if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && !multiSel_.isEmpty()) {
		removeSelected();
		return;
	}
	QWidget::keyPressEvent(e);
}

void TrackEditor::splitSegment(int index, qint64 splitSrcMs)
{
	if (index < 0 || index >= segs_.size())
		return;
	CutSegment &a = segs_[index];
	// Keep both halves at least kMinCutMs long.
	splitSrcMs = std::clamp<qint64>(splitSrcMs, a.srcStartMs + kMinCutMs, a.srcEndMs - kMinCutMs);
	if (splitSrcMs <= a.srcStartMs || splitSrcMs >= a.srcEndMs)
		return;

	CutSegment b = a; // shares speed
	b.srcStartMs = splitSrcMs;
	a.srcEndMs = splitSrcMs;
	segs_.insert(index + 1, b);
	++segsRev_;

	selected_ = index + 1;
	multiSel_ = QSet<int>{selected_};
	emit segmentsChanged();
	emit selectionChanged(selected_);
	update();
}

void TrackEditor::showSegmentMenu(int index, const QPoint &globalPos, const QPoint &localPos)
{
	// Acting on a member of a multi-selection applies to the whole selection.
	const bool group = multiSel_.size() > 1 && multiSel_.contains(index);
	const int n = group ? multiSel_.size() : 1;

	// Where along this cut the cursor is → the source-time to split at.
	const QVector<QRect> &rects = segmentRects();
	const CutSegment &seg = segs_[index];
	qint64 splitSrcMs = -1;
	if (index < rects.size()) {
		const QRect r = rects[index];
		const double f = std::clamp(double(localPos.x() - r.left()) / std::max(1, r.width()),
					    0.0, 1.0);
		splitSrcMs = seg.srcStartMs + qint64(f * (seg.srcEndMs - seg.srcStartMs));
	}
	const bool canSplit = splitSrcMs > seg.srcStartMs + kMinCutMs &&
			      splitSrcMs < seg.srcEndMs - kMinCutMs;

	QMenu menu(this);
	QAction *inspect = menu.addAction(QStringLiteral("Show in inspector"));
	menu.addSeparator();
	QAction *del = menu.addAction(group ? QStringLiteral("Delete %1 cuts").arg(n)
					    : QStringLiteral("Delete cut"));
	QAction *split = menu.addAction(QStringLiteral("Split here"));
	split->setEnabled(canSplit);
	QAction *dup = menu.addAction(QStringLiteral("Duplicate"));
	QAction *reset = menu.addAction(group ? QStringLiteral("Reset speed to 1× (%1 cuts)").arg(n)
					      : QStringLiteral("Reset speed to 1×"));
	QAction *chosen = menu.exec(globalPos);
	if (chosen == inspect) {
		emit inspectRequested();
	} else if (chosen == split) {
		splitSegment(index, splitSrcMs);
	} else if (chosen == dup) {
		// Insert an identical copy of this cut immediately to its right.
		const CutSegment copy = segs_[index];
		segs_.insert(index + 1, copy);
		++segsRev_;
		selected_ = index + 1;
		multiSel_ = QSet<int>{selected_};
		emit segmentsChanged();
		emit selectionChanged(selected_);
		update();
	} else if (chosen == del) {
		if (group)
			removeSelected();
		else
			removeSegment(index);
	} else if (chosen == reset) {
		if (group) {
			for (int i : std::as_const(multiSel_))
				setSegmentSpeed(i, 1.0);
		} else {
			setSegmentSpeed(index, 1.0);
		}
		// Speed changes output durations — treat it as an edit (updates the info
		// label + records an undo step), then refresh the slider.
		emit segmentsChanged();
		emit selectionChanged(selected_ >= 0 ? selected_ : index);
	}
}

} // namespace harpia
