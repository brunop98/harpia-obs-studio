#include "KeyframeEditor.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace harpia {

namespace {
const QColor kBg(0x1a, 0x1c, 0x20);
const QColor kSpan(0x25, 0x2a, 0x31);
const QColor kBorder(0x3a, 0x3e, 0x45);
const QColor kCaption(0x9a, 0x9f, 0xa8);
const QColor kKey(0xff, 0xd4, 0x4f);
const QColor kKeySel(0xff, 0xff, 0xff);
const QColor kCurve(0x00, 0xae, 0xef);
const QColor kPlayhead(0xe5, 0x48, 0x4d);

constexpr int kMargin = 8;
constexpr int kRulerH = 16;
constexpr int kGrab = 7;   // half-width of a key's clickable area
constexpr int kDiamond = 5;
constexpr double kMaxZoom = 64.0;
} // namespace

// ---- KeyframeLane -----------------------------------------------------------

KeyframeLane::KeyframeLane(int lane, QWidget *parent) : QWidget(parent), lane_(lane)
{
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	setMinimumHeight(120);
}

void KeyframeLane::setClip(const TlClip &c)
{
	clip_ = c;
	if (sel_ >= clip_.keys.size())
		sel_ = -1;
	clampView();
	update();
}

void KeyframeLane::setPlayhead(qint64 clipMs)
{
	playheadMs_ = std::clamp<qint64>(clipMs, 0, clip_.outDurationMs());
	update();
}

void KeyframeLane::selectKey(int keyIndex)
{
	const int want = (keyIndex >= 0 && keyIndex < clip_.keys.size()) ? keyIndex : -1;
	if (want == sel_)
		return;
	sel_ = want;
	// Selecting programmatically has to reach the property panel too, or the
	// panel and the highlighted diamond disagree about what is selected.
	emit selectionChanged(sel_);
	update();
}

void KeyframeLane::zoomToFit()
{
	zoom_ = 1.0;
	viewStart_ = 0;
	update();
}

QSize KeyframeLane::minimumSizeHint() const
{
	return QSize(320, 120);
}

QRect KeyframeLane::contentRect() const
{
	return QRect(kMargin, kMargin + kRulerH, std::max(1, width() - 2 * kMargin),
		     std::max(1, height() - 2 * kMargin - kRulerH));
}

void KeyframeLane::clampView()
{
	zoom_ = std::clamp(zoom_, 1.0, kMaxZoom);
	const qint64 dur = std::max<qint64>(1, clip_.outDurationMs());
	const qint64 visible = qint64(double(dur) / zoom_);
	viewStart_ = std::clamp<qint64>(viewStart_, 0, std::max<qint64>(0, dur - visible));
}

int KeyframeLane::msToX(qint64 ms) const
{
	const QRect r = contentRect();
	const qint64 dur = std::max<qint64>(1, clip_.outDurationMs());
	const qint64 visible = std::max<qint64>(1, qint64(double(dur) / zoom_));
	return r.x() + int(double(ms - viewStart_) / double(visible) * r.width());
}

qint64 KeyframeLane::xToMs(int x) const
{
	const QRect r = contentRect();
	const qint64 dur = std::max<qint64>(1, clip_.outDurationMs());
	const qint64 visible = std::max<qint64>(1, qint64(double(dur) / zoom_));
	const double f = double(x - r.x()) / double(std::max(1, r.width()));
	return std::clamp<qint64>(viewStart_ + qint64(f * double(visible)), 0, dur);
}

int KeyframeLane::keyAtX(int x) const
{
	int best = -1, bestD = kGrab + 1;
	for (int i : clip_.keysOnLane(lane_)) {
		const int d = std::abs(msToX(clip_.keys[i].tMs) - x);
		if (d < bestD) {
			bestD = d;
			best = i;
		}
	}
	return best;
}

void KeyframeLane::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.fillRect(rect(), kBg);
	const QRect r = contentRect();
	const qint64 dur = std::max<qint64>(1, clip_.outDurationMs());

	// The clip's span, so a key's place in the clip is readable at a glance.
	p.setPen(Qt::NoPen);
	p.setBrush(kSpan);
	p.drawRoundedRect(r, 3, 3);

	// Ruler: a tick roughly every 80px, on a round number of seconds.
	p.setPen(kCaption);
	QFont f = p.font();
	f.setPixelSize(9);
	p.setFont(f);
	const qint64 visible = std::max<qint64>(1, qint64(double(dur) / zoom_));
	const double perPx = double(visible) / double(std::max(1, r.width()));
	double stepMs = 1000.0;
	for (const double cand : {100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 30000.0}) {
		stepMs = cand;
		if (cand / perPx >= 80.0)
			break;
	}
	for (qint64 t = qint64(std::ceil(viewStart_ / stepMs) * stepMs); t <= viewStart_ + visible;
	     t += qint64(stepMs)) {
		const int x = msToX(t);
		if (x < r.x() - 1 || x > r.right() + 1)
			continue;
		p.drawLine(x, kMargin + kRulerH - 4, x, kMargin + kRulerH - 1);
		p.drawText(QRect(x - 28, kMargin - 1, 56, kRulerH - 4), Qt::AlignHCenter | Qt::AlignTop,
			   QStringLiteral("%1s").arg(t / 1000.0, 0, 'f', stepMs < 1000 ? 1 : 0));
	}

	// The channel's actual curve across the clip, sampled through the same
	// transformAt() the compositor uses — so the shape drawn IS the shape
	// rendered, easing and all.
	const QVector<int> mine = clip_.keysOnLane(lane_);
	if (mine.size() >= 2) {
		double lo = 1e30, hi = -1e30;
		auto valueOf = [&](const TlTransform &tf) {
			switch (lane_) {
			case TlLaneScale: return tf.scale;
			case TlLaneRot: return tf.rotation;
			case TlLaneOpacity: return tf.opacity;
			default: return tf.posX; // position draws X; Y rides the same keys
			}
		};
		for (int i : mine) {
			lo = std::min(lo, valueOf(clip_.keys[i].tf));
			hi = std::max(hi, valueOf(clip_.keys[i].tf));
		}
		if (hi - lo < 1e-9) {
			lo -= 0.5;
			hi += 0.5;
		}
		const int top = r.y() + 14, bot = r.bottom() - 18;
		QPolygonF curve;
		for (int x = r.x(); x <= r.right(); ++x) {
			const qint64 t = xToMs(x);
			const double v = valueOf(clip_.transformAt(clip_.outStartMs + t));
			const double n = (v - lo) / (hi - lo);
			curve << QPointF(x, bot - n * (bot - top));
		}
		p.setPen(QPen(kCurve, 2));
		p.setBrush(Qt::NoBrush);
		p.drawPolyline(curve);
	}

	// Keys: a diamond each, the selected one filled white and larger.
	for (int i : mine) {
		const int x = msToX(clip_.keys[i].tMs);
		if (x < r.x() - 8 || x > r.right() + 8)
			continue;
		const int y = r.bottom() - 10;
		const int s = (i == sel_) ? kDiamond + 2 : kDiamond;
		QPainterPath d;
		d.moveTo(x, y - s);
		d.lineTo(x + s, y);
		d.lineTo(x, y + s);
		d.lineTo(x - s, y);
		d.closeSubpath();
		p.setPen(QPen(QColor(0x20, 0x20, 0x20), 1));
		p.setBrush(i == sel_ ? kKeySel : kKey);
		p.drawPath(d);
	}

	if (mine.isEmpty()) {
		p.setPen(kCaption);
		p.drawText(r, Qt::AlignCenter,
			   QStringLiteral("No %1 keyframes — double-click to add one.")
				   .arg(QString::fromLatin1(tlLaneName(lane_)).toLower()));
	}

	// Playhead.
	const int px = msToX(playheadMs_);
	if (px >= r.x() - 1 && px <= r.right() + 1) {
		p.setPen(QPen(kPlayhead, 1));
		p.drawLine(px, kMargin, px, r.bottom());
	}

	p.setPen(QPen(kBorder, 1));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(r, 3, 3);
}

void KeyframeLane::mousePressEvent(QMouseEvent *e)
{
	pressPos_ = e->pos();
	moved_ = false;
	if (e->button() == Qt::MiddleButton) {
		drag_ = Drag::Pan;
		panStart_ = viewStart_;
		setCursor(Qt::ClosedHandCursor);
		return;
	}
	if (e->button() != Qt::LeftButton)
		return;
	const int hit = keyAtX(e->pos().x());
	if (hit >= 0) {
		sel_ = hit;
		drag_ = Drag::Key;
		dragKey_ = hit;
		dragGrabMs_ = xToMs(e->pos().x()) - clip_.keys[hit].tMs;
		emit selectionChanged(sel_);
		update();
		return;
	}
	// Empty space scrubs, so the preview follows the cursor here too.
	drag_ = Drag::Scrub;
	playheadMs_ = xToMs(e->pos().x());
	emit scrubbed(playheadMs_);
	update();
}

void KeyframeLane::mouseMoveEvent(QMouseEvent *e)
{
	if (drag_ == Drag::Pan) {
		const qint64 dur = std::max<qint64>(1, clip_.outDurationMs());
		const qint64 visible = qint64(double(dur) / zoom_);
		const double perPx = double(visible) / double(std::max(1, contentRect().width()));
		viewStart_ = panStart_ - qint64((e->pos().x() - pressPos_.x()) * perPx);
		clampView();
		update();
		return;
	}
	if (drag_ == Drag::Key && dragKey_ >= 0 && dragKey_ < clip_.keys.size()) {
		moved_ = true;
		const qint64 t = std::clamp<qint64>(xToMs(e->pos().x()) - dragGrabMs_, 0,
						    clip_.outDurationMs());
		clip_.keys[dragKey_].tMs = t;
		// Keep the list sorted as it passes a neighbour, and follow the key we
		// are holding rather than whatever index it used to be.
		const TlKeyframe held = clip_.keys[dragKey_];
		clip_.sortKeys();
		for (int i = 0; i < clip_.keys.size(); ++i)
			if (clip_.keys[i] == held) {
				dragKey_ = sel_ = i;
				break;
			}
		QToolTip::showText(e->globalPosition().toPoint(),
				   QStringLiteral("%1 s").arg(t / 1000.0, 0, 'f', 2), this);
		emit selectionChanged(sel_);
		emit clipEdited();
		update();
		return;
	}
	if (drag_ == Drag::Scrub) {
		playheadMs_ = xToMs(e->pos().x());
		emit scrubbed(playheadMs_);
		update();
		return;
	}
	setCursor(keyAtX(e->pos().x()) >= 0 ? Qt::SizeHorCursor : Qt::ArrowCursor);
}

void KeyframeLane::mouseReleaseEvent(QMouseEvent *e)
{
	Q_UNUSED(e);
	if (drag_ == Drag::Key && moved_)
		emit clipEdited(); // the finished retime is the undo step
	drag_ = Drag::None;
	dragKey_ = -1;
	QToolTip::hideText();
	unsetCursor();
}

void KeyframeLane::mouseDoubleClickEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	const int hit = keyAtX(e->pos().x());
	if (hit >= 0) { // double-clicking a key drops it from THIS channel
		clip_.clearKeyLane(hit, lane_);
		sel_ = -1;
		emit selectionChanged(-1);
		emit clipEdited();
		update();
		return;
	}
	// Empty space: add a key here, holding whatever the channel is doing now, so
	// adding one never changes the picture.
	const qint64 t = xToMs(e->pos().x());
	clip_.setKeyframeAt(clip_.outStartMs + t, clip_.transformAt(clip_.outStartMs + t),
			    TlEase::EaseInOut, 1 << lane_);
	sel_ = clip_.keyframeIndexAt(clip_.outStartMs + t, 1);
	emit selectionChanged(sel_);
	emit clipEdited();
	update();
}

void KeyframeLane::wheelEvent(QWheelEvent *e)
{
	const int dy = e->angleDelta().y();
	if (dy == 0)
		return;
	if (e->modifiers() & Qt::ShiftModifier) { // shift-wheel pans
		const qint64 dur = std::max<qint64>(1, clip_.outDurationMs());
		viewStart_ -= qint64(dy / 120.0 * double(dur) / zoom_ * 0.15);
		clampView();
		update();
		e->accept();
		return;
	}
	// Zoom about the cursor, so the frame under it stays put.
	const qint64 anchor = xToMs(int(e->position().x()));
	const double before = zoom_;
	zoom_ = std::clamp(zoom_ * std::pow(1.2, dy / 120.0), 1.0, kMaxZoom);
	if (std::abs(zoom_ - before) > 1e-9) {
		const QRect r = contentRect();
		const qint64 dur = std::max<qint64>(1, clip_.outDurationMs());
		const qint64 visible = qint64(double(dur) / zoom_);
		const double f = double(int(e->position().x()) - r.x()) / double(std::max(1, r.width()));
		viewStart_ = anchor - qint64(f * double(visible));
		clampView();
		update();
	}
	e->accept();
}

// ---- KeyframeEditor ---------------------------------------------------------

KeyframeEditor::KeyframeEditor(QWidget *parent) : QDialog(parent)
{
	// Floating, resizable, and NOT modal: the point is to keep editing the
	// timeline and watching the preview while this is open.
	setWindowTitle(QStringLiteral("Keyframes"));
	setWindowFlag(Qt::Window, true);
	setModal(false);
	setSizeGripEnabled(true);
	resize(720, 460);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(10, 10, 10, 10);
	outer->setSpacing(8);

	auto *hint = new QLabel(
		QStringLiteral("Double-click a lane to add a keyframe (or to remove one). Drag a "
			       "diamond to retime it, wheel to zoom, Shift-wheel or middle-drag to "
			       "pan. Every change updates the preview and is one undo step."),
		this);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	outer->addWidget(hint);

	tabs_ = new QTabWidget(this);
	tabs_->setDocumentMode(true);
	for (int l = 0; l < kTlLaneCount; ++l) {
		auto *lane = new KeyframeLane(l, this);
		lanes_.append(lane);
		tabs_->addTab(lane, QString::fromLatin1(tlLaneName(l)));
		connect(lane, &KeyframeLane::clipEdited, this, [this, lane]() {
			if (syncing_)
				return;
			clip_ = lane->clip();
			pushEdit();
		});
		connect(lane, &KeyframeLane::selectionChanged, this, [this](int) { syncKeyPanel(); });
		connect(lane, &KeyframeLane::scrubbed, this, [this](qint64 t) {
			emit scrubRequested(clip_.outStartMs + t);
			for (KeyframeLane *l2 : lanes_)
				l2->setPlayhead(t);
		});
	}
	connect(tabs_, &QTabWidget::currentChanged, this, [this](int) { syncKeyPanel(); });
	outer->addWidget(tabs_, 1);

	// ---- selected-key property panel ----
	keyInfo_ = new QLabel(this);
	keyInfo_->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	outer->addWidget(keyInfo_);

	form_ = new QFormLayout;
	QFormLayout *form = form_;
	form->setHorizontalSpacing(10);
	form->setVerticalSpacing(4);

	timeSpin_ = new QSpinBox(this);
	timeSpin_->setRange(0, 1000 * 60 * 60);
	timeSpin_->setSuffix(QStringLiteral(" ms"));
	timeSpin_->setSingleStep(10);
	timeSpin_->setKeyboardTracking(false);
	form->addRow(QStringLiteral("Time"), timeSpin_);
	connect(timeSpin_, &QSpinBox::valueChanged, this, [this](int v) {
		if (syncing_)
			return;
		KeyframeLane *lane = currentLaneWidget();
		const int k = lane ? lane->selectedKey() : -1;
		if (k < 0 || k >= clip_.keys.size())
			return;
		clip_.keys[k].tMs = std::clamp<qint64>(v, 0, clip_.outDurationMs());
		const TlKeyframe held = clip_.keys[k];
		clip_.sortKeys();
		for (int i = 0; i < clip_.keys.size(); ++i)
			if (clip_.keys[i] == held) {
				lane->selectKey(i);
				break;
			}
		pushEdit();
	});

	// Position needs two boxes; the others one. Both rows live in the form and
	// only the relevant one is shown.
	auto mkSpin = [this](double lo, double hi, double step, int dec) {
		auto *s = new QDoubleSpinBox(this);
		s->setRange(lo, hi);
		s->setSingleStep(step);
		s->setDecimals(dec);
		s->setKeyboardTracking(false);
		return s;
	};
	valueSpins_.resize(kTlLaneCount + 1);
	valueSpins_[0] = mkSpin(-2.0, 3.0, 0.01, 3);   // pos X
	valueSpins_[4] = mkSpin(-2.0, 3.0, 0.01, 3);   // pos Y (kept at the end)
	valueSpins_[1] = mkSpin(0.05, 20.0, 0.05, 2);  // scale
	valueSpins_[2] = mkSpin(-3600.0, 3600.0, 1.0, 1); // rotation
	valueSpins_[2]->setSuffix(QStringLiteral("°"));
	valueSpins_[3] = mkSpin(0.0, 1.0, 0.05, 2);    // opacity

	posRow_ = new QWidget(this);
	auto *ph = new QHBoxLayout(posRow_);
	ph->setContentsMargins(0, 0, 0, 0);
	ph->setSpacing(6);
	ph->addWidget(new QLabel(QStringLiteral("X"), posRow_));
	ph->addWidget(valueSpins_[0], 1);
	ph->addWidget(new QLabel(QStringLiteral("Y"), posRow_));
	ph->addWidget(valueSpins_[4], 1);
	form->addRow(QStringLiteral("Value"), posRow_);

	valueRow_ = new QWidget(this);
	auto *vh = new QHBoxLayout(valueRow_);
	vh->setContentsMargins(0, 0, 0, 0);
	vh->addWidget(valueSpins_[1], 1);
	vh->addWidget(valueSpins_[2], 1);
	vh->addWidget(valueSpins_[3], 1);
	valueLabel_ = new QLabel(QStringLiteral("Value"), this);
	form->addRow(valueLabel_, valueRow_);

	auto applyValue = [this]() {
		if (syncing_)
			return;
		KeyframeLane *lane = currentLaneWidget();
		const int k = lane ? lane->selectedKey() : -1;
		if (k < 0 || k >= clip_.keys.size())
			return;
		switch (currentLane()) {
		case TlLaneScale: clip_.keys[k].tf.scale = valueSpins_[1]->value(); break;
		case TlLaneRot: clip_.keys[k].tf.rotation = valueSpins_[2]->value(); break;
		case TlLaneOpacity: clip_.keys[k].tf.opacity = valueSpins_[3]->value(); break;
		default:
			clip_.keys[k].tf.posX = valueSpins_[0]->value();
			clip_.keys[k].tf.posY = valueSpins_[4]->value();
			break;
		}
		pushEdit();
	};
	for (QDoubleSpinBox *s : valueSpins_)
		connect(s, &QDoubleSpinBox::valueChanged, this, [applyValue](double) { applyValue(); });

	easeCombo_ = new QComboBox(this);
	for (int i = 0; i < kTlEaseCount; ++i)
		easeCombo_->addItem(QString::fromLatin1(tlEaseName(TlEase(i))));
	easeCombo_->setToolTip(QStringLiteral(
		"How this keyframe hands over to the next one, for this channel only."));
	form->addRow(QStringLiteral("Interpolation"), easeCombo_);
	connect(easeCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
		if (syncing_)
			return;
		KeyframeLane *lane = currentLaneWidget();
		const int k = lane ? lane->selectedKey() : -1;
		if (k < 0 || k >= clip_.keys.size())
			return;
		clip_.keys[k].channel(currentLane()).ease = tlEaseFromInt(idx);
		pushEdit();
		syncKeyPanel();
	});

	bezRow_ = new QWidget(this);
	auto *bh = new QHBoxLayout(bezRow_);
	bh->setContentsMargins(0, 0, 0, 0);
	bh->setSpacing(6);
	bez1_ = mkSpin(0.0, 1.0, 0.01, 2);
	bez2_ = mkSpin(0.0, 1.0, 0.01, 2);
	bh->addWidget(new QLabel(QStringLiteral("in"), bezRow_));
	bh->addWidget(bez1_, 1);
	bh->addWidget(new QLabel(QStringLiteral("out"), bezRow_));
	bh->addWidget(bez2_, 1);
	form->addRow(QStringLiteral("Bezier"), bezRow_);
	for (QDoubleSpinBox *s : {bez1_, bez2_})
		connect(s, &QDoubleSpinBox::valueChanged, this, [this](double) {
			if (syncing_)
				return;
			KeyframeLane *lane = currentLaneWidget();
			const int k = lane ? lane->selectedKey() : -1;
			if (k < 0 || k >= clip_.keys.size())
				return;
			TlKeyChannel &ch = clip_.keys[k].channel(currentLane());
			ch.bez1 = bez1_->value();
			ch.bez2 = bez2_->value();
			pushEdit();
		});
	outer->addLayout(form);

	auto *btns = new QHBoxLayout;
	auto *addBtn = new QPushButton(QStringLiteral("Add key at playhead"), this);
	connect(addBtn, &QPushButton::clicked, this, &KeyframeEditor::addKeyHere);
	btns->addWidget(addBtn);
	copyBtn_ = new QPushButton(QStringLiteral("Copy"), this);
	connect(copyBtn_, &QPushButton::clicked, this, &KeyframeEditor::copySelected);
	btns->addWidget(copyBtn_);
	pasteBtn_ = new QPushButton(QStringLiteral("Paste at playhead"), this);
	connect(pasteBtn_, &QPushButton::clicked, this, &KeyframeEditor::pasteHere);
	btns->addWidget(pasteBtn_);
	delBtn_ = new QPushButton(QStringLiteral("Delete"), this);
	connect(delBtn_, &QPushButton::clicked, this, &KeyframeEditor::deleteSelected);
	btns->addWidget(delBtn_);
	btns->addStretch(1);
	auto *fitBtn = new QPushButton(QStringLiteral("Fit"), this);
	connect(fitBtn, &QPushButton::clicked, this, [this]() {
		for (KeyframeLane *l : lanes_)
			l->zoomToFit();
	});
	btns->addWidget(fitBtn);
	auto *closeBtn = new QPushButton(QStringLiteral("Close"), this);
	closeBtn->setToolTip(QStringLiteral("Closing keeps every keyframe — they live on the clip."));
	connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
	btns->addWidget(closeBtn);
	outer->addLayout(btns);

	new QShortcut(QKeySequence::Copy, this, this, &KeyframeEditor::copySelected);
	new QShortcut(QKeySequence::Paste, this, this, &KeyframeEditor::pasteHere);
	new QShortcut(QKeySequence(Qt::Key_Delete), this, this, &KeyframeEditor::deleteSelected);
	new QShortcut(QKeySequence(Qt::Key_K), this, this, &KeyframeEditor::addKeyHere);

	syncKeyPanel();
}

int KeyframeEditor::currentLane() const
{
	return tabs_ ? std::clamp(tabs_->currentIndex(), 0, kTlLaneCount - 1) : 0;
}

KeyframeLane *KeyframeEditor::currentLaneWidget() const
{
	const int i = currentLane();
	return (i >= 0 && i < lanes_.size()) ? lanes_[i] : nullptr;
}

void KeyframeEditor::setClip(const TlClip &c, const QString &label)
{
	clip_ = c;
	setWindowTitle(label.isEmpty() ? QStringLiteral("Keyframes")
				       : QStringLiteral("Keyframes — %1").arg(label));
	rebuildLanes();
	syncKeyPanel();
}

void KeyframeEditor::setPlayheadOut(qint64 outMs)
{
	const qint64 t = std::clamp<qint64>(outMs - clip_.outStartMs, 0, clip_.outDurationMs());
	for (KeyframeLane *l : lanes_)
		l->setPlayhead(t);
}

void KeyframeEditor::rebuildLanes()
{
	syncing_ = true;
	for (KeyframeLane *l : lanes_)
		l->setClip(clip_);
	syncing_ = false;
}

void KeyframeEditor::pushEdit()
{
	rebuildLanes();
	syncKeyPanel();
	emit clipChanged(clip_);
}

void KeyframeEditor::syncKeyPanel()
{
	syncing_ = true;
	const int lane = currentLane();
	KeyframeLane *lw = currentLaneWidget();
	const int k = lw ? lw->selectedKey() : -1;
	const bool have = (k >= 0 && k < clip_.keys.size() && clip_.keys[k].channel(lane).on);

	// setRowVisible, not setVisible on the field: hiding only the widget leaves
	// its label behind as a stray word with nothing next to it.
	form_->setRowVisible(posRow_, lane == TlLanePos);
	form_->setRowVisible(valueRow_, lane != TlLanePos);
	valueSpins_[1]->setVisible(lane == TlLaneScale);
	valueSpins_[2]->setVisible(lane == TlLaneRot);
	valueSpins_[3]->setVisible(lane == TlLaneOpacity);
	// The form's "Value" label belongs to whichever row is showing.
	valueLabel_->setText(QString::fromLatin1(tlLaneName(lane)));

	for (QWidget *w : {static_cast<QWidget *>(timeSpin_), static_cast<QWidget *>(easeCombo_),
			   posRow_, valueRow_, static_cast<QWidget *>(delBtn_),
			   static_cast<QWidget *>(copyBtn_)})
		w->setEnabled(have);
	pasteBtn_->setEnabled(haveClip_);

	if (have) {
		const TlKeyframe &key = clip_.keys[k];
		timeSpin_->setMaximum(int(std::min<qint64>(clip_.outDurationMs(), 1 << 30)));
		timeSpin_->setValue(int(key.tMs));
		valueSpins_[0]->setValue(key.tf.posX);
		valueSpins_[4]->setValue(key.tf.posY);
		valueSpins_[1]->setValue(key.tf.scale);
		valueSpins_[2]->setValue(key.tf.rotation);
		valueSpins_[3]->setValue(key.tf.opacity);
		easeCombo_->setCurrentIndex(int(key.channel(lane).ease));
		bez1_->setValue(key.channel(lane).bez1);
		bez2_->setValue(key.channel(lane).bez2);
		const int n = clip_.keysOnLane(lane).size();
		keyInfo_->setText(QStringLiteral("Key %1 of %2 on %3.")
					  .arg(clip_.keysOnLane(lane).indexOf(k) + 1)
					  .arg(n)
					  .arg(QString::fromLatin1(tlLaneName(lane))));
	} else {
		const int n = clip_.keysOnLane(lane).size();
		keyInfo_->setText(n == 0 ? QStringLiteral("No %1 keyframes on this clip yet.")
						   .arg(QString::fromLatin1(tlLaneName(lane)).toLower())
					 : QStringLiteral("%1 %2 keyframe%3 — click one to edit it.")
						   .arg(n)
						   .arg(QString::fromLatin1(tlLaneName(lane)).toLower())
						   .arg(n == 1 ? QString() : QStringLiteral("s")));
	}
	form_->setRowVisible(bezRow_, have && easeCombo_->currentIndex() == int(TlEase::Bezier));
	syncing_ = false;
}

void KeyframeEditor::addKeyHere()
{
	KeyframeLane *lane = currentLaneWidget();
	if (!lane)
		return;
	const qint64 t = lane->playhead();
	const qint64 at = clip_.outStartMs + t;
	// Seeded from what the channel is already doing, so adding a key never
	// changes the picture — it just pins it.
	clip_.setKeyframeAt(at, clip_.transformAt(at), TlEase::EaseInOut, 1 << currentLane());
	pushEdit();
	lane->selectKey(clip_.keyframeIndexAt(at, 1));
	syncKeyPanel();
}

void KeyframeEditor::deleteSelected()
{
	KeyframeLane *lane = currentLaneWidget();
	const int k = lane ? lane->selectedKey() : -1;
	if (k < 0 || k >= clip_.keys.size())
		return;
	clip_.clearKeyLane(k, currentLane());
	lane->selectKey(-1);
	pushEdit();
}

void KeyframeEditor::copySelected()
{
	KeyframeLane *lane = currentLaneWidget();
	const int k = lane ? lane->selectedKey() : -1;
	if (k < 0 || k >= clip_.keys.size())
		return;
	clipboard_ = clip_.keys[k];
	clipboardLane_ = currentLane();
	haveClip_ = true;
	syncKeyPanel();
}

void KeyframeEditor::pasteHere()
{
	if (!haveClip_)
		return;
	KeyframeLane *lane = currentLaneWidget();
	if (!lane)
		return;
	const int dst = currentLane();
	const qint64 at = clip_.outStartMs + lane->playhead();
	// Paste the VALUES the copied key held, into the lane being viewed — so a
	// key can be copied from Scale into Rotation if that is what you want.
	TlTransform tf = clip_.transformAt(at);
	switch (dst) {
	case TlLaneScale: tf.scale = clipboard_.tf.scale; break;
	case TlLaneRot: tf.rotation = clipboard_.tf.rotation; break;
	case TlLaneOpacity: tf.opacity = clipboard_.tf.opacity; break;
	default:
		tf.posX = clipboard_.tf.posX;
		tf.posY = clipboard_.tf.posY;
		break;
	}
	clip_.setKeyframeAt(at, tf, clipboard_.channel(clipboardLane_).ease, 1 << dst);
	const int k = clip_.keyframeIndexAt(at, 1);
	if (k >= 0) {
		clip_.keys[k].channel(dst) = clipboard_.channel(clipboardLane_);
		clip_.keys[k].channel(dst).on = true;
	}
	pushEdit();
	lane->selectKey(k);
	syncKeyPanel();
}

} // namespace harpia
