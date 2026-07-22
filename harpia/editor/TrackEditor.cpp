#include "TrackEditor.hpp"

#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <utility>

namespace harpia {

namespace {
// Layout values live in TrackLayoutParams (lp_) so the Developer Panel can
// tweak them at runtime; only non-layout constants remain here.
constexpr qint64 kMinCutMs = 150;

const QColor kBarBg(0x20, 0x22, 0x25);
const QColor kBarBorder(0x30, 0x33, 0x38);
const QColor kCaption(0x9a, 0x9f, 0xa8);
const QColor kAccent(0x00, 0xae, 0xef);
const QColor kSegFill(0x2e, 0x4d, 0x6e);
const QColor kSegFillSel(0x3a, 0x6e, 0xa5);
const QColor kPlayhead(0xe5, 0x48, 0x4d);
} // namespace

TrackEditor::TrackEditor(QWidget *parent) : QWidget(parent)
{
	setFocusPolicy(Qt::ClickFocus); // so Delete works after clicking a segment
	setMouseTracking(true);         // cursor hints over the tracks
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setToolTip(QStringLiteral("Source track: scroll to zoom, Shift+scroll to pan"));
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
	stripCache_ = QPixmap(); // geometry changed — rebuild the filmstrip
	updateGeometry();        // min-size hint depends on the track heights
	update();
}

void TrackEditor::setDuration(qint64 ms)
{
	duration_ = std::max<qint64>(0, ms);
	zoom_ = 1.0;
	viewStart_ = 0;
	update();
}

void TrackEditor::setThumbs(const QVector<QImage> &thumbs)
{
	thumbs_ = thumbs;
	++thumbsRev_; // invalidates the strip cache
	update();
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
	cp.setPen(kBarBorder);
	cp.setBrush(kBarBg);
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

void TrackEditor::drawTimeRuler(QPainter &p, const QRect &src) const
{
	if (duration_ <= 0 || src.width() <= 0)
		return;
	// Pick a "nice" tick interval so labels are ~70px apart at the current zoom.
	static const qint64 kSteps[] = {200,   500,    1000,   2000,   5000,  10000,
					15000, 30000,  60000,  120000, 300000, 600000,
					900000, 1800000, 3600000};
	const double msPerPx = double(visibleMs()) / std::max(1, src.width());
	const qint64 want = qint64(msPerPx * 70.0);
	qint64 step = kSteps[sizeof(kSteps) / sizeof(kSteps[0]) - 1];
	for (qint64 s : kSteps) {
		if (s >= want) {
			step = s;
			break;
		}
	}

	auto label = [](qint64 ms) {
		const qint64 m = ms / 60000, s = (ms / 1000) % 60;
		if (ms % 1000 && ms < 60000)
			return QStringLiteral("%1.%2s").arg(ms / 1000).arg((ms % 1000) / 100);
		return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
	};

	// A subtle dark band across the top of the source strip keeps labels legible.
	const int bandH = 12;
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0, 0, 0, 90));
	p.drawRect(QRect(src.left(), src.top(), src.width(), bandH));

	QFont rf = p.font();
	rf.setPixelSize(9);
	p.setFont(rf);

	const qint64 first = (viewStart_ / step) * step;
	for (qint64 t = first; t <= viewStart_ + visibleMs(); t += step) {
		if (t < 0)
			continue;
		const int x = msToX(t);
		if (x < src.left() - 1 || x > src.right() + 1)
			continue;
		p.setPen(QColor(0xff, 0xff, 0xff, 60));
		p.drawLine(x, src.top(), x, src.top() + bandH); // tick
		p.setPen(QColor(0xc8, 0xcc, 0xd2));
		p.drawText(x + 2, src.top() + bandH - 2, label(t)); // label
	}
}

void TrackEditor::clampView()
{
	viewStart_ = std::clamp<qint64>(viewStart_, 0, std::max<qint64>(0, duration_ - visibleMs()));
}

void TrackEditor::wheelEvent(QWheelEvent *e)
{
	// Zoom/pan applies to the source track (the output track has its own scale).
	if (duration_ <= 0 || !sourceRect().contains(e->position().toPoint())) {
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
	if (!pan) {
		// Zoom around the time under the cursor — no modifier needed.
		const int x = int(e->position().x());
		const qint64 anchor = xToMs(x);
		const QRect r = sourceRect();
		const double frac = std::clamp(double(x - r.x()) / std::max(1, r.width()), 0.0, 1.0);
		zoom_ = std::clamp(zoom_ * std::pow(1.3, steps), 1.0, lp_.maxZoom);
		viewStart_ = anchor - qint64(frac * visibleMs());
	} else {
		viewStart_ -= qint64(steps * visibleMs() * 0.15);
	}
	clampView();
	// Preview the frame under the cursor as the view scrolls, mirroring hover.
	if (mode_ == Mode::None) {
		hoverMs_ = xToMs(int(e->position().x()));
		emit hoverScrub(hoverMs_);
	}
	update();
	e->accept();
}

void TrackEditor::setSegmentSpeed(int index, double speed)
{
	if (index < 0 || index >= segs_.size())
		return;
	segs_[index].speed = std::clamp(speed, 0.25, 4.0);
	update();
}

QList<int> TrackEditor::selectedIndices() const
{
	QList<int> list(multiSel_.begin(), multiSel_.end());
	std::sort(list.begin(), list.end());
	return list;
}

void TrackEditor::removeSegment(int index)
{
	if (index < 0 || index >= segs_.size())
		return;
	segs_.remove(index);
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
	selected_ = -1;
	multiSel_.clear();
	emit segmentsChanged();
	emit selectionChanged(-1);
	update();
}

qint64 TrackEditor::totalOutputMs() const
{
	qint64 total = 0;
	for (const CutSegment &s : segs_)
		total += s.outDurationMs();
	return total;
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

QVector<QRect> TrackEditor::segmentRects() const
{
	QVector<QRect> rects;
	const int n = segs_.size();
	if (!n)
		return rects;
	const QRect r = outputRect();
	const int avail = std::max(1, r.width() - lp_.segGap * (n - 1));
	qint64 total = totalOutputMs();
	if (total <= 0)
		total = 1;

	QVector<double> w(n);
	double sum = 0.0;
	for (int i = 0; i < n; ++i) {
		w[i] = std::max<double>(lp_.minSegW,
					double(segs_[i].outDurationMs()) / double(total) * avail);
		sum += w[i];
	}
	if (sum > avail) {
		const double k = double(avail) / sum;
		for (int i = 0; i < n; ++i)
			w[i] = std::max<double>(lp_.hardMinSegW, w[i] * k);
	}
	int x = r.x();
	for (int i = 0; i < n; ++i) {
		const int wi = int(w[i]);
		rects.append(QRect(x, r.y(), wi, r.height()));
		x += wi + lp_.segGap;
	}
	return rects;
}

int TrackEditor::segmentAt(const QPoint &p) const
{
	const QVector<QRect> rects = segmentRects();
	for (int i = 0; i < rects.size(); ++i) {
		if (rects[i].contains(p))
			return i;
	}
	return -1;
}

int TrackEditor::insertSlotAt(int x) const
{
	const QVector<QRect> rects = segmentRects();
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

	const QRect src = sourceRect();
	const QRect out = outputRect();

	// ---- Captions --------------------------------------------------------
	p.setPen(kCaption);
	p.drawText(QRect(src.x(), lp_.margin, src.width(), lp_.captionH),
		   Qt::AlignVCenter | Qt::AlignLeft,
		   QStringLiteral("Source — press and drag to select a section to keep"));
	QString outCaption = QStringLiteral("Output — %1 cut%2, %3s")
				     .arg(segs_.size())
				     .arg(segs_.size() == 1 ? QString() : QStringLiteral("s"))
				     .arg(totalOutputMs() / 1000.0, 0, 'f', 1);
	if (!segs_.isEmpty())
		outCaption += QStringLiteral("   (drag to reorder · Ctrl+click multi-select · Del to delete)");
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

		// Existing cuts shown as translucent regions on the source.
		for (int i = 0; i < segs_.size(); ++i) {
			const int x1 = msToX(segs_[i].srcStartMs);
			const int x2 = msToX(segs_[i].srcEndMs);
			QColor fill = kAccent;
			fill.setAlpha(i == selected_ ? 110 : 60);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			p.drawRect(QRect(QPoint(x1, src.y() + 2), QPoint(x2, src.bottom() - 2)));
		}
		// The in-progress drag selection.
		if (mode_ == Mode::CreatingCut) {
			const int x1 = msToX(std::min(dragStartMs_, dragCurMs_));
			const int x2 = msToX(std::max(dragStartMs_, dragCurMs_));
			QColor fill = kAccent;
			fill.setAlpha(140);
			p.setPen(QPen(kAccent, 1));
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
		drawTimeRuler(p, src);
		p.restore();
	}

	// Zoom indicator: which part of the clip the source track is showing.
	if (zoom_ > 1.001 && duration_ > 0) {
		const int y = src.bottom() + 2;
		p.setPen(Qt::NoPen);
		p.setBrush(kBarBg);
		p.drawRect(QRect(src.left(), y, src.width(), 2));
		p.setBrush(kCaption);
		const int ix = src.left() + int(double(viewStart_) / duration_ * src.width());
		const int iw = std::max(8, int(double(visibleMs()) / duration_ * src.width()));
		p.drawRect(QRect(ix, y, iw, 2));
	}

	// ---- Output track ----------------------------------------------------
	p.setPen(kBarBorder);
	p.setBrush(kBarBg);
	p.drawRoundedRect(out, 5, 5);

	if (segs_.isEmpty()) {
		p.setPen(kCaption);
		p.drawText(out, Qt::AlignCenter,
			   QStringLiteral("Cuts you select above appear here in order"));
	}

	p.save();
	p.setClipRect(out.adjusted(1, 0, -1, 0));
	const QVector<QRect> rects = segmentRects();
	QFont segFont = font();
	segFont.setPixelSize(std::max(6, lp_.segFontPx));
	for (int i = 0; i < rects.size(); ++i) {
		const QRect r = rects[i].adjusted(0, 3, 0, -3);
		const bool sel = multiSel_.contains(i);
		p.setPen(Qt::NoPen);
		p.setBrush(sel ? kSegFillSel : kSegFill);
		p.drawRoundedRect(r, 4, 4);

		// One thumbnail per cut (its first frame) — quick visual identification.
		int thumbW = 0;
		if (!thumbs_.isEmpty() && duration_ > 0) {
			const int ti = std::clamp(int(double(segs_[i].srcStartMs) / duration_ *
						      thumbs_.size()),
						  0, int(thumbs_.size()) - 1);
			const QImage &t = thumbs_[ti];
			if (!t.isNull() && t.height() > 0) {
				const int th = r.height() - 2;
				thumbW = std::min(th * t.width() / t.height(), r.width() - 2);
				QPainterPath segClip;
				segClip.addRoundedRect(r, 4, 4);
				p.save();
				p.setClipPath(segClip);
				p.drawImage(QRect(r.x() + 1, r.y() + 1, thumbW, th), t);
				p.restore();
			}
		}

		// Labels: beside the thumbnail when there is room, else compact overlay.
		const QString l1 =
			QStringLiteral("#%1 · %2×").arg(i + 1).arg(segs_[i].speed, 0, 'g', 3);
		const QString l2 =
			QStringLiteral("%1s").arg(segs_[i].outDurationMs() / 1000.0, 0, 'f', 1);
		p.setFont(segFont);
		const int textX = r.x() + thumbW + 5;
		const int room = r.right() - textX;
		if (room >= 30) {
			p.setPen(QColor(0xe6, 0xe6, 0xe6));
			const QRect top(textX, r.y() + 1, room - 2, r.height() / 2 - 1);
			const QRect bot(textX, r.y() + r.height() / 2, room - 2, r.height() / 2 - 1);
			p.drawText(top, Qt::AlignVCenter | Qt::AlignLeft,
				   p.fontMetrics().elidedText(l1, Qt::ElideRight, room - 2));
			p.drawText(bot, Qt::AlignVCenter | Qt::AlignLeft,
				   p.fontMetrics().elidedText(l2, Qt::ElideRight, room - 2));
		} else if (r.width() >= 26) {
			QPainterPath segClip;
			segClip.addRoundedRect(r, 4, 4);
			p.save();
			p.setClipPath(segClip);
			p.fillRect(QRect(r.x(), r.bottom() - 12, r.width(), 13), QColor(0, 0, 0, 150));
			p.setPen(QColor(0xe6, 0xe6, 0xe6));
			p.drawText(QRect(r.x() + 2, r.bottom() - 12, r.width() - 4, 13),
				   Qt::AlignVCenter | Qt::AlignLeft,
				   p.fontMetrics().elidedText(l1, Qt::ElideRight, r.width() - 4));
			p.restore();
		}

		// Selection border on top so it stays visible over the thumbnail.
		p.setPen(sel ? QPen(kAccent, 2) : QPen(kBarBorder, 1));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(r, 4, 4);
	}

	// Reorder caret while dragging a segment.
	if (mode_ == Mode::DraggingSegment && dragMoved_ && dragInsertSlot_ >= 0) {
		int cx;
		if (rects.isEmpty())
			cx = out.x();
		else if (dragInsertSlot_ >= rects.size())
			cx = rects.last().right() + lp_.segGap / 2 + 1;
		else
			cx = rects[dragInsertSlot_].left() - lp_.segGap / 2 - 1;
		p.setPen(QPen(kAccent, 2));
		p.drawLine(cx, out.y() + 2, cx, out.bottom() - 2);
	}

	// Output playhead.
	if (playheadOutMs_ >= 0 && !rects.isEmpty()) {
		qint64 acc = 0;
		for (int i = 0; i < segs_.size(); ++i) {
			const qint64 d = segs_[i].outDurationMs();
			if (playheadOutMs_ < acc + d || i == segs_.size() - 1) {
				const double f =
					std::clamp(double(playheadOutMs_ - acc) / double(d), 0.0, 1.0);
				const int px = rects[i].x() + int(f * rects[i].width());
				p.setPen(QPen(kPlayhead, 2));
				p.drawLine(px, out.y() + 1, px, out.bottom() - 1);
				break;
			}
			acc += d;
		}
	}

	// Hover marker on the output track — mirrors the source-track marker so
	// it's obvious which frame is being previewed.
	if (hoverOutSeg_ >= 0 && hoverOutSeg_ < rects.size() && mode_ == Mode::None) {
		const QRect hr = rects[hoverOutSeg_];
		const int hx = std::clamp(hoverOutX_, hr.left() + 1, hr.right() - 1);
		p.setPen(QPen(QColor(0xff, 0xff, 0xff, 170), 1));
		p.drawLine(hx, hr.top() + 4, hx, hr.bottom() - 4);
	}
	p.restore();
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
			showSegmentMenu(idx, e->globalPosition().toPoint());
		}
		return;
	}
	if (e->button() != Qt::LeftButton)
		return;

	if (duration_ > 0 && sourceRect().contains(pos)) {
		mode_ = Mode::CreatingCut;
		dragStartMs_ = dragCurMs_ = xToMs(pos.x());
		emit scrubSource(dragCurMs_);
		update();
		return;
	}

	if (outputRect().contains(pos)) {
		const QVector<QRect> segRects = segmentRects(); // computed once per press
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
				emit scrubSource(segs_[idx].srcStartMs);
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
			emit scrubSource(segs_[idx].srcStartMs);
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
				emit scrubSource(onLeft ? seg.srcStartMs : seg.srcEndMs);
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
				emit scrubSource(srcMs);
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
		emit scrubSource(dragCurMs_);
		update();
		return;
	}
	if (mode_ == Mode::DraggingSegment) {
		if (!dragMoved_ && (pos - pressPos_).manhattanLength() > 6)
			dragMoved_ = true;
		if (dragMoved_) {
			dragInsertSlot_ = insertSlotAt(pos.x());
			update();
		}
		return;
	}
	if ((mode_ == Mode::ResizingLeft || mode_ == Mode::ResizingRight) && selected_ >= 0) {
		// Live edge trim: convert the pixel delta into source ms with the scale
		// captured at press, clamp, and preview the frame at the moving edge.
		CutSegment &seg = segs_[selected_];
		const qint64 deltaMs = qint64(std::llround((pos.x() - pressPos_.x()) * resizeSrcPerPx_));
		if (mode_ == Mode::ResizingLeft) {
			seg.srcStartMs = std::clamp<qint64>(resizeOrigStart_ + deltaMs, 0,
							    resizeOrigEnd_ - kMinCutMs);
			emit scrubSource(seg.srcStartMs);
		} else {
			seg.srcEndMs = std::clamp<qint64>(resizeOrigEnd_ + deltaMs,
							  resizeOrigStart_ + kMinCutMs, duration_);
			emit scrubSource(seg.srcEndMs);
		}
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
	const QVector<QRect> rects = segmentRects();
	if (sourceRect().contains(pos)) {
		setCursor(Qt::CrossCursor);
		if (e->buttons() == Qt::NoButton && duration_ > 0) {
			newHover = xToMs(pos.x());
			emit hoverScrub(newHover);
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
				emit hoverScrub(newHover);
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
			segs_.append(seg);
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
				selected_ = to;
				// Indices shifted — collapse the selection to the moved cut.
				multiSel_ = QSet<int>{selected_};
				emit segmentsChanged();
				emit selectionChanged(selected_);
			}
		}
		dragInsertSlot_ = -1;
		dragMoved_ = false;
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

void TrackEditor::showSegmentMenu(int index, const QPoint &globalPos)
{
	// Acting on a member of a multi-selection applies to the whole selection.
	const bool group = multiSel_.size() > 1 && multiSel_.contains(index);
	const int n = group ? multiSel_.size() : 1;

	QMenu menu(this);
	QAction *del = menu.addAction(group ? QStringLiteral("Delete %1 cuts").arg(n)
					    : QStringLiteral("Delete cut"));
	QAction *reset = menu.addAction(group ? QStringLiteral("Reset speed to 1× (%1 cuts)").arg(n)
					      : QStringLiteral("Reset speed to 1×"));
	QAction *chosen = menu.exec(globalPos);
	if (chosen == del) {
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
		// Re-announce the selection so the editor window refreshes its slider.
		emit selectionChanged(selected_ >= 0 ? selected_ : index);
	}
}

} // namespace harpia
