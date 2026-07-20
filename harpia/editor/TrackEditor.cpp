#include "TrackEditor.hpp"

#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

namespace harpia {

namespace {
constexpr int kMargin = 8;
constexpr int kCaptionH = 16;
constexpr int kSrcH = 34; // tall enough for the filmstrip
constexpr int kTrackGap = 8;
constexpr double kMaxZoom = 32.0;
constexpr int kOutH = 40;
constexpr int kSegGap = 4;
constexpr int kMinSegW = 48;     // "reasonably wide" — easy to click and drag
constexpr int kHardMinSegW = 24; // absolute floor when many segments compete
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
	setToolTip(QStringLiteral("Source track: Ctrl+scroll to zoom, scroll to pan"));
}

QSize TrackEditor::sizeHint() const
{
	return QSize(480, minimumSizeHint().height());
}

QSize TrackEditor::minimumSizeHint() const
{
	return QSize(240, 2 * kMargin + 2 * kCaptionH + kSrcH + kTrackGap + kOutH);
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
	update();
}

qint64 TrackEditor::visibleMs() const
{
	return std::max<qint64>(1, qint64(duration_ / zoom_));
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
	const int delta = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
	if (delta == 0)
		return;
	const double steps = delta / 120.0;
	if (e->modifiers() & Qt::ControlModifier) {
		const int x = int(e->position().x());
		const qint64 anchor = xToMs(x);
		const QRect r = sourceRect();
		const double frac = std::clamp(double(x - r.x()) / std::max(1, r.width()), 0.0, 1.0);
		zoom_ = std::clamp(zoom_ * std::pow(1.3, steps), 1.0, kMaxZoom);
		viewStart_ = anchor - qint64(frac * visibleMs());
	} else {
		viewStart_ -= qint64(steps * visibleMs() * 0.15);
	}
	clampView();
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

void TrackEditor::removeSegment(int index)
{
	if (index < 0 || index >= segs_.size())
		return;
	segs_.remove(index);
	selected_ = -1;
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
	return QRect(kMargin, kMargin + kCaptionH, width() - 2 * kMargin, kSrcH);
}

QRect TrackEditor::outputRect() const
{
	return QRect(kMargin, kMargin + kCaptionH + kSrcH + kTrackGap + kCaptionH,
		     width() - 2 * kMargin, kOutH);
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
	const int avail = std::max(1, r.width() - kSegGap * (n - 1));
	qint64 total = totalOutputMs();
	if (total <= 0)
		total = 1;

	QVector<double> w(n);
	double sum = 0.0;
	for (int i = 0; i < n; ++i) {
		w[i] = std::max<double>(kMinSegW, double(segs_[i].outDurationMs()) / double(total) * avail);
		sum += w[i];
	}
	if (sum > avail) {
		const double k = double(avail) / sum;
		for (int i = 0; i < n; ++i)
			w[i] = std::max<double>(kHardMinSegW, w[i] * k);
	}
	int x = r.x();
	for (int i = 0; i < n; ++i) {
		const int wi = int(w[i]);
		rects.append(QRect(x, r.y(), wi, r.height()));
		x += wi + kSegGap;
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
	capFont.setPixelSize(11);
	p.setFont(capFont);

	const QRect src = sourceRect();
	const QRect out = outputRect();

	// ---- Captions --------------------------------------------------------
	p.setPen(kCaption);
	p.drawText(QRect(src.x(), kMargin, src.width(), kCaptionH), Qt::AlignVCenter | Qt::AlignLeft,
		   QStringLiteral("Source — press and drag to select a section to keep"));
	QString outCaption = QStringLiteral("Output — %1 cut%2, %3s")
				     .arg(segs_.size())
				     .arg(segs_.size() == 1 ? QString() : QStringLiteral("s"))
				     .arg(totalOutputMs() / 1000.0, 0, 'f', 1);
	if (!segs_.isEmpty())
		outCaption += QStringLiteral("   (drag to reorder · Del to delete)");
	p.drawText(QRect(out.x(), out.y() - kCaptionH, out.width(), kCaptionH),
		   Qt::AlignVCenter | Qt::AlignLeft, outCaption);

	// ---- Source track ----------------------------------------------------
	p.setPen(kBarBorder);
	p.setBrush(kBarBg);
	p.drawRoundedRect(src, 5, 5);

	{
		QPainterPath clip;
		clip.addRoundedRect(src, 5, 5);
		p.save();
		p.setClipPath(clip);

		// Filmstrip so each part of the video is easy to recognize.
		if (!thumbs_.isEmpty() && duration_ > 0) {
			const int n = thumbs_.size();
			const double sliceMs = double(duration_) / n;
			const qint64 viewEnd = viewStart_ + visibleMs();
			int i0 = std::clamp(int(viewStart_ / sliceMs), 0, n - 1);
			int i1 = std::clamp(int(viewEnd / sliceMs) + 1, i0 + 1, n);
			for (int i = i0; i < i1; ++i) {
				if (thumbs_[i].isNull())
					continue;
				const int x1 = msToX(qint64(i * sliceMs));
				const int x2 = msToX(qint64((i + 1) * sliceMs));
				if (x2 > x1)
					p.drawImage(QRect(x1, src.y(), x2 - x1, src.height()),
						    thumbs_[i]);
			}
		}

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
	segFont.setPixelSize(10);
	for (int i = 0; i < rects.size(); ++i) {
		const QRect r = rects[i].adjusted(0, 3, 0, -3);
		const bool sel = (i == selected_);
		p.setPen(sel ? QPen(kAccent, 2) : QPen(kBarBorder, 1));
		p.setBrush(sel ? kSegFillSel : kSegFill);
		p.drawRoundedRect(r, 4, 4);
		if (r.width() >= 34) {
			p.setFont(segFont);
			p.setPen(QColor(0xe6, 0xe6, 0xe6));
			const QString l1 = QStringLiteral("#%1 · %2×").arg(i + 1).arg(
				segs_[i].speed, 0, 'g', 3);
			const QString l2 =
				QStringLiteral("%1s").arg(segs_[i].outDurationMs() / 1000.0, 0, 'f', 1);
			p.drawText(r.adjusted(4, 1, -4, -r.height() / 2), Qt::AlignVCenter | Qt::AlignLeft,
				   p.fontMetrics().elidedText(l1, Qt::ElideRight, r.width() - 8));
			p.drawText(r.adjusted(4, r.height() / 2, -4, -1), Qt::AlignVCenter | Qt::AlignLeft,
				   p.fontMetrics().elidedText(l2, Qt::ElideRight, r.width() - 8));
		}
	}

	// Reorder caret while dragging a segment.
	if (mode_ == Mode::DraggingSegment && dragMoved_ && dragInsertSlot_ >= 0) {
		int cx;
		if (rects.isEmpty())
			cx = out.x();
		else if (dragInsertSlot_ >= rects.size())
			cx = rects.last().right() + kSegGap / 2 + 1;
		else
			cx = rects[dragInsertSlot_].left() - kSegGap / 2 - 1;
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
		const int idx = segmentAt(pos);
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
			const QRect r = segmentRects()[idx];
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
				emit scrubSource(seg.srcStartMs);
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

	// Idle: cursor hints.
	if (sourceRect().contains(pos)) {
		setCursor(Qt::CrossCursor);
	} else {
		const int idx = segmentAt(pos);
		if (idx >= 0) {
			const QRect r = segmentRects()[idx];
			const int edgeZone = std::min(7, r.width() / 3);
			if (pos.x() - r.left() <= edgeZone || r.right() - pos.x() <= edgeZone)
				setCursor(Qt::SizeHorCursor);
			else
				setCursor(Qt::PointingHandCursor);
		} else {
			unsetCursor();
		}
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
	if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && selected_ >= 0) {
		removeSegment(selected_);
		return;
	}
	QWidget::keyPressEvent(e);
}

void TrackEditor::showSegmentMenu(int index, const QPoint &globalPos)
{
	QMenu menu(this);
	QAction *del = menu.addAction(QStringLiteral("Delete cut"));
	QAction *reset = menu.addAction(QStringLiteral("Reset speed to 1×"));
	QAction *chosen = menu.exec(globalPos);
	if (chosen == del) {
		removeSegment(index);
	} else if (chosen == reset) {
		setSegmentSpeed(index, 1.0);
		// Re-announce the selection so the editor window refreshes its slider.
		emit selectionChanged(index);
	}
}

} // namespace harpia
