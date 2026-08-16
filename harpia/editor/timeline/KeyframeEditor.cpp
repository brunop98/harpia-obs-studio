#include "KeyframeEditor.hpp"

#include "../component/ComponentRegistry.hpp"
#include "../../ui/ColorField.hpp"

#include <QCheckBox>
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

} // namespace

// ---- KeyframeLane -----------------------------------------------------------

KeyframeLane::KeyframeLane(int lane, QWidget *parent) : QWidget(parent), lane_(lane)
{
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	setMinimumHeight(lp_.laneMinH);
}

void KeyframeLane::setComponentSource(int componentIndex, const QString &propKey,
				     const PropDef &def)
{
	compIndex_ = componentIndex;
	propKey_ = propKey;
	def_ = def;
	sel_ = -1;
	update();
}

const QVector<PropKey> *KeyframeLane::propKeys() const
{
	if (compIndex_ < 0 || compIndex_ >= clip_.components.size())
		return nullptr;
	const auto it = clip_.components[compIndex_].keys.find(propKey_);
	return it == clip_.components[compIndex_].keys.cend() ? nullptr : &(*it);
}

QVector<PropKey> *KeyframeLane::propKeys()
{
	return const_cast<QVector<PropKey> *>(std::as_const(*this).propKeys());
}

int KeyframeLane::keyCount() const
{
	if (!isComponentLane())
		return clip_.keysOnLane(lane_).size();
	const QVector<PropKey> *k = propKeys();
	return k ? k->size() : 0;
}

qint64 KeyframeLane::keyTimeAt(int i) const
{
	if (!isComponentLane())
		return (i >= 0 && i < clip_.keys.size()) ? clip_.keys[i].tMs : 0;
	const QVector<PropKey> *k = propKeys();
	return (k && i >= 0 && i < k->size()) ? (*k)[i].tMs : 0;
}

double KeyframeLane::keyValueAt(int i) const
{
	if (!isComponentLane()) {
		if (i < 0 || i >= clip_.keys.size())
			return 0.0;
		const TlTransform &tf = clip_.keys[i].tf;
		switch (lane_) {
		case TlLaneScale: return tf.scale;
		case TlLaneRot: return tf.rotation;
		case TlLaneOpacity: return tf.opacity;
		default: return tf.posX; // position draws X; Y rides the same keys
		}
	}
	const QVector<PropKey> *k = propKeys();
	return (k && i >= 0 && i < k->size()) ? (*k)[i].v : 0.0;
}

void KeyframeLane::removeKeyAt(int i)
{
	if (!isComponentLane()) {
		clip_.clearKeyLane(i, lane_);
		return;
	}
	QVector<PropKey> *k = propKeys();
	if (!k || i < 0 || i >= k->size())
		return;
	k->remove(i);
	// An emptied list is erased, not left behind: propAt() reads "has an entry"
	// as "is animated", so an empty vector leaves the property looking keyed
	// with nothing in it -- and its static value permanently overridden.
	if (k->isEmpty())
		clip_.components[compIndex_].keys.remove(propKey_);
}

int KeyframeLane::addKeyAt(qint64 tMs)
{
	if (!isComponentLane()) {
		const qint64 at = clip_.outStartMs + tMs;
		clip_.setKeyframeAt(at, clip_.transformAt(at), TlEase::EaseInOut, 1 << lane_);
		return clip_.keyframeIndexAt(at, 1);
	}
	if (compIndex_ < 0 || compIndex_ >= clip_.components.size())
		return -1;
	QVector<PropKey> &k = clip_.components[compIndex_].keys[propKey_];
	// Seeded from what the property is ALREADY doing at this instant, so adding
	// a key pins the picture rather than changing it -- the same promise the
	// transform lanes make.
	PropKey nk;
	nk.tMs = tMs;
	nk.v = k.isEmpty() ? clip_.components[compIndex_].props.value(propKey_, def_.def).toDouble()
			   : valueFromKeys(def_, k, tMs).toDouble();
	for (int i = 0; i < k.size(); ++i)
		if (k[i].tMs == tMs) { // one key per instant, like every other store
			k[i].v = nk.v;
			return i;
		}
	k.append(nk);
	std::sort(k.begin(), k.end(), [](const PropKey &a, const PropKey &b) { return a.tMs < b.tMs; });
	for (int i = 0; i < k.size(); ++i)
		if (k[i].tMs == tMs)
			return i;
	return -1;
}

QString KeyframeLane::channelName() const
{
	return isComponentLane() ? (def_.label.isEmpty() ? propKey_ : def_.label)
				 : QString::fromLatin1(tlLaneName(lane_));
}

void KeyframeLane::setLayoutParams(const KeyframeLayoutParams &p)
{
	lp_ = p;
	setMinimumHeight(lp_.laneMinH);
	clampView(); // maxZoom may have moved under the current zoom
	update();
}

void KeyframeLane::setClip(const TlClip &c)
{
	clip_ = c;
	if (sel_ >= keyCount() && !isComponentLane())
		sel_ = -1;
	if (isComponentLane() && sel_ >= keyCount())
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
	const int limit = isComponentLane() ? keyCount() : clip_.keys.size();
	const int want = (keyIndex >= 0 && keyIndex < limit) ? keyIndex : -1;
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
	return QRect(lp_.margin, lp_.margin + lp_.rulerH, std::max(1, width() - 2 * lp_.margin),
		     std::max(1, height() - 2 * lp_.margin - lp_.rulerH));
}

void KeyframeLane::clampView()
{
	zoom_ = std::clamp(zoom_, 1.0, lp_.maxZoom);
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

// The indices this lane shows, in time order. For a transform channel that is
// the subset of the clip's keys pinning it; for a component property it is
// simply all of that property's keys.
QVector<int> KeyframeLane::laneKeyIndices() const
{
	if (!isComponentLane())
		return clip_.keysOnLane(lane_);
	QVector<int> out;
	const int n = keyCount();
	out.reserve(n);
	for (int i = 0; i < n; ++i)
		out.append(i);
	return out;
}

int KeyframeLane::keyAtX(int x) const
{
	int best = -1, bestD = lp_.grab + 1;
	for (int i : laneKeyIndices()) {
		const int d = std::abs(msToX(keyTimeAt(i)) - x);
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
		p.drawLine(x, lp_.margin + lp_.rulerH - 4, x, lp_.margin + lp_.rulerH - 1);
		p.drawText(QRect(x - 28, lp_.margin - 1, 56, lp_.rulerH - 4), Qt::AlignHCenter | Qt::AlignTop,
			   QStringLiteral("%1s").arg(t / 1000.0, 0, 'f', stepMs < 1000 ? 1 : 0));
	}

	// The channel's actual curve across the clip, sampled through the same
	// transformAt() the compositor uses — so the shape drawn IS the shape
	// rendered, easing and all.
	const QVector<int> mine = laneKeyIndices();
	// A colour has no curve: it does not live on a number line, and a line
	// drawn through its packed 0xAARRGGBB would wander through bit patterns
	// that are not colours at all. Its diamonds carry the swatch instead.
	const bool drawCurve = mine.size() >= 2 &&
			       !(isComponentLane() && def_.type == PropType::Color);
	if (drawCurve) {
		double lo = 1e30, hi = -1e30;
		for (int i : mine) {
			lo = std::min(lo, keyValueAt(i));
			hi = std::max(hi, keyValueAt(i));
		}
		if (hi - lo < 1e-9) {
			lo -= 0.5;
			hi += 0.5;
		}
		// Sampled through the SAME evaluator the compositor uses -- so the
		// shape drawn is the shape rendered, easing and all.
		const QVector<PropKey> *pk = propKeys();
		auto sample = [&](qint64 t) {
			if (!isComponentLane()) {
				const TlTransform tf = clip_.transformAt(clip_.outStartMs + t);
				switch (lane_) {
				case TlLaneScale: return tf.scale;
				case TlLaneRot: return tf.rotation;
				case TlLaneOpacity: return tf.opacity;
				default: return tf.posX;
				}
			}
			return pk ? valueFromKeys(def_, *pk, t).toDouble() : 0.0;
		};
		const int top = r.y() + 14, bot = r.bottom() - 18;
		QPolygonF curve;
		for (int x = r.x(); x <= r.right(); ++x) {
			const double n = (sample(xToMs(x)) - lo) / (hi - lo);
			curve << QPointF(x, bot - n * (bot - top));
		}
		p.setPen(QPen(kCurve, 2));
		p.setBrush(Qt::NoBrush);
		p.drawPolyline(curve);
	}

	// Keys: a diamond each, the selected one filled white and larger.
	const bool isColour = isComponentLane() && def_.type == PropType::Color;
	for (int i : mine) {
		const int x = msToX(keyTimeAt(i));
		if (x < r.x() - 8 || x > r.right() + 8)
			continue;
		const int y = r.bottom() - 10;
		const int s = (i == sel_) ? lp_.diamond + 2 : lp_.diamond;
		QPainterPath d;
		d.moveTo(x, y - s);
		d.lineTo(x + s, y);
		d.lineTo(x, y + s);
		d.lineTo(x - s, y);
		d.closeSubpath();
		p.setPen(QPen(QColor(0x20, 0x20, 0x20), 1));
		// A colour key is drawn IN its colour: with no curve to read, the
		// diamond is the only thing that can say what it holds.
		p.setBrush(isColour ? QColor::fromRgba(QRgb(quint32(keyValueAt(i))))
				    : (i == sel_ ? kKeySel : kKey));
		p.drawPath(d);
		if (isColour && i == sel_) {
			p.setPen(QPen(kKeySel, 2));
			p.setBrush(Qt::NoBrush);
			p.drawPath(d);
		}
	}

	if (mine.isEmpty()) {
		p.setPen(kCaption);
		p.drawText(r, Qt::AlignCenter,
			   QStringLiteral("No %1 keyframes — double-click to add one.")
				   .arg(channelName().toLower()));
	}

	// Playhead.
	const int px = msToX(playheadMs_);
	if (px >= r.x() - 1 && px <= r.right() + 1) {
		p.setPen(QPen(kPlayhead, 1));
		p.drawLine(px, lp_.margin, px, r.bottom());
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
		dragGrabMs_ = xToMs(e->pos().x()) - keyTimeAt(hit);
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
	if (drag_ == Drag::Key && dragKey_ >= 0 && dragKey_ < keyStoreSize()) {
		moved_ = true;
		const qint64 t = std::clamp<qint64>(xToMs(e->pos().x()) - dragGrabMs_, 0,
						    clip_.outDurationMs());
		// Keep the store sorted as the key passes a neighbour, and follow the
		// key we are HOLDING rather than whatever index it used to be.
		if (!isComponentLane()) {
			clip_.keys[dragKey_].tMs = t;
			const TlKeyframe held = clip_.keys[dragKey_];
			clip_.sortKeys();
			for (int i = 0; i < clip_.keys.size(); ++i)
				if (clip_.keys[i] == held) {
					dragKey_ = sel_ = i;
					break;
				}
		} else if (QVector<PropKey> *pk = propKeys()) {
			(*pk)[dragKey_].tMs = t;
			const PropKey held = (*pk)[dragKey_];
			std::sort(pk->begin(), pk->end(),
				  [](const PropKey &a, const PropKey &b) { return a.tMs < b.tMs; });
			for (int i = 0; i < pk->size(); ++i)
				if ((*pk)[i] == held) {
					dragKey_ = sel_ = i;
					break;
				}
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
		removeKeyAt(hit);
		sel_ = -1;
		emit selectionChanged(-1);
		emit clipEdited();
		update();
		return;
	}
	// Empty space: add a key here, holding whatever the channel is doing now, so
	// adding one never changes the picture.
	sel_ = addKeyAt(xToMs(e->pos().x()));
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
	zoom_ = std::clamp(zoom_ * std::pow(1.2, dy / 120.0), 1.0, lp_.maxZoom);
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
		lane->setLayoutParams(lp_); // whatever the Dev panel last set
		lanes_.append(lane);
		tabs_->addTab(lane, QString::fromLatin1(tlLaneName(l)));
		wireLane(lane);
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
		if (lane && lane->isComponentLane()) {
			PropKey *pk = selectedCompKey();
			if (!pk)
				return;
			pk->tMs = std::clamp<qint64>(v, 0, clip_.outDurationMs());
			const PropKey held = *pk;
			auto &vec = clip_.components[lane->componentIndex()].keys[lane->propKey()];
			std::sort(vec.begin(), vec.end(),
				  [](const PropKey &a, const PropKey &b) { return a.tMs < b.tMs; });
			for (int i = 0; i < vec.size(); ++i)
				if (vec[i] == held) {
					lane->selectKey(i);
					break;
				}
			pushEdit();
			return;
		}
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

	// The component-property value row: a number, a checkbox or a colour field,
	// per the property's type. The same three shapes the Inspector offers for
	// the same properties, so a keyed value is edited the way the static one is.
	compRow_ = new QWidget(this);
	auto *ch2 = new QHBoxLayout(compRow_);
	ch2->setContentsMargins(0, 0, 0, 0);
	ch2->setSpacing(6);
	compSpin_ = mkSpin(-1e6, 1e6, 0.01, 3);
	compCheck_ = new QCheckBox(QStringLiteral("On"), compRow_);
	compSwatch_ = new QPushButton(compRow_);
	compSwatch_->setFixedWidth(120);
	ch2->addWidget(compSpin_, 1);
	ch2->addWidget(compCheck_);
	ch2->addWidget(compSwatch_);
	compLabel_ = new QLabel(QStringLiteral("Value"), this);
	form->addRow(compLabel_, compRow_);
	connect(compSpin_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
		if (!syncing_)
			applyCompValue(v);
	});
	connect(compCheck_, &QCheckBox::toggled, this, [this](bool on) {
		if (!syncing_)
			applyCompValue(on ? 1.0 : 0.0);
	});
	connect(compSwatch_, &QPushButton::clicked, this, [this]() {
		const QColor start = compSwatch_->property("harpiaColor").value<QColor>();
		// Live, like every other colour in the project: the preview follows the
		// picker, and the key it lands on is one undo step.
		pickColorLive(this, QStringLiteral("Keyframe colour"),
			      start.isValid() ? start : QColor(Qt::white), [this](const QColor &c) {
				      paintColorSwatch(compSwatch_, c, /*showHex=*/true);
				      applyCompValue(double(quint32(c.rgba())));
			      },
			      /*allowAlpha=*/true);
	});

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
		if (PropKey *pk = selectedCompKey()) {
			pk->ease = tlEaseFromInt(idx);
			pushEdit();
			syncKeyPanel();
			return;
		}
		KeyframeLane *lane = currentLaneWidget();
		const int k = lane ? lane->selectedKey() : -1;
		if (k < 0 || k >= clip_.keys.size() || lane->isComponentLane())
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
			if (PropKey *pk = selectedCompKey()) {
				pk->bez1 = bez1_->value();
				pk->bez2 = bez2_->value();
				pushEdit();
				return;
			}
			KeyframeLane *lane = currentLaneWidget();
			const int k = lane ? lane->selectedKey() : -1;
			if (k < 0 || k >= clip_.keys.size() || lane->isComponentLane())
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

// The transform channel being shown. Meaningless while a component tab is up,
// which is why every caller asks currentLaneWidget()->isComponentLane() first.
int KeyframeEditor::currentLane() const
{
	return tabs_ ? std::clamp(tabs_->currentIndex(), 0, kTlLaneCount - 1) : 0;
}

KeyframeLane *KeyframeEditor::currentLaneWidget() const
{
	const int i = tabs_ ? tabs_->currentIndex() : -1;
	return (i >= 0 && i < lanes_.size()) ? lanes_[i] : nullptr;
}

void KeyframeEditor::setClip(const TlClip &c, const QString &label)
{
	clip_ = c;
	setWindowTitle(label.isEmpty() ? QStringLiteral("Keyframes")
				       : QStringLiteral("Keyframes — %1").arg(label));
	rebuildTabs();
	rebuildLanes();
	syncKeyPanel();
}

void KeyframeEditor::setLayoutParams(const KeyframeLayoutParams &p)
{
	lp_ = p;
	// Also remembered here, so lanes rebuilt later (rebuildLanes runs whenever
	// the clip changes) come up with the current geometry rather than the
	// struct defaults.
	for (KeyframeLane *l : lanes_)
		if (l)
			l->setLayoutParams(lp_);
}

void KeyframeEditor::setPlayheadOut(qint64 outMs)
{
	const qint64 t = std::clamp<qint64>(outMs - clip_.outStartMs, 0, clip_.outDurationMs());
	for (KeyframeLane *l : lanes_)
		l->setPlayhead(t);
}

int KeyframeEditor::tabCountForTest() const
{
	return tabs_ ? tabs_->count() : 0;
}

QString KeyframeEditor::tabTextForTest(int i) const
{
	return (tabs_ && i >= 0 && i < tabs_->count()) ? tabs_->tabText(i) : QString();
}

void KeyframeEditor::showTabForTest(int i)
{
	if (tabs_ && i >= 0 && i < tabs_->count())
		tabs_->setCurrentIndex(i);
}

void KeyframeEditor::wireLane(KeyframeLane *lane)
{
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

void KeyframeEditor::rebuildTabs()
{
	// What SHOULD be here: every component property that has keys on it. A tab
	// per keyed property, and none for the rest -- an empty component lane
	// would be a lane you cannot add to from here (the key list is created on
	// the component, by the Inspector's diamond), and every keyed property
	// without a tab is animation you can see moving and cannot reach.
	QVector<QPair<int, QString>> want;
	for (int ci = 0; ci < clip_.components.size(); ++ci) {
		const ComponentInstance &inst = clip_.components[ci];
		for (auto it = inst.keys.cbegin(); it != inst.keys.cend(); ++it)
			if (!it->isEmpty())
				want.append({ci, it.key()});
	}
	if (want == compTabs_)
		return; // the same set: leave the tabs (and the current one) alone

	// Drop the old component tabs; the four transform lanes always stay.
	while (tabs_->count() > kTlLaneCount)
		tabs_->removeTab(tabs_->count() - 1);
	while (lanes_.size() > kTlLaneCount) {
		delete lanes_.takeLast();
	}
	compTabs_ = want;

	const ComponentRegistry &reg = ComponentRegistry::instance();
	// How many instances share a type, so a clip with two Blurs does not grow
	// two tabs both called "Blur · Radius".
	QHash<QString, int> ofType;
	for (const ComponentInstance &inst : clip_.components)
		ofType[inst.typeId]++;
	QHash<QString, int> seen;

	for (const auto &w : want) {
		const ComponentInstance &inst = clip_.components[w.first];
		const ComponentType *type = reg.find(inst.typeId);
		PropDef def;
		def.key = w.second;
		def.label = w.second;
		if (type)
			for (const PropDef &d : type->props)
				if (d.key == w.second) {
					def = d;
					break;
				}
		QString owner = type ? type->displayName : inst.typeId;
		if (ofType.value(inst.typeId) > 1)
			owner += QStringLiteral(" %1").arg(++seen[inst.typeId]);

		auto *lane = new KeyframeLane(0, this);
		lane->setLayoutParams(lp_);
		lane->setComponentSource(w.first, w.second, def);
		lanes_.append(lane);
		const int idx = tabs_->addTab(
			lane, QStringLiteral("%1 · %2").arg(owner, def.label.isEmpty() ? w.second
											: def.label));
		if (!def.help.isEmpty())
			tabs_->setTabToolTip(idx, def.help);
		wireLane(lane);
	}
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

// The property panel for a component lane. Same three rows -- Time, Value,
// Interpolation -- reading a PropKey instead of a TlKeyframe channel.
void KeyframeEditor::syncCompPanel(KeyframeLane *lw)
{
	const PropDef &def = lw->propDef();
	const int ci = lw->componentIndex();
	const int k = lw->selectedKey();
	const QVector<PropKey> *keys = nullptr;
	if (ci >= 0 && ci < clip_.components.size()) {
		const auto it = clip_.components[ci].keys.find(lw->propKey());
		if (it != clip_.components[ci].keys.cend())
			keys = &(*it);
	}
	const bool have = keys && k >= 0 && k < keys->size();

	form_->setRowVisible(posRow_, false);
	form_->setRowVisible(valueRow_, false);
	form_->setRowVisible(compRow_, true);
	compLabel_->setText(def.label.isEmpty() ? lw->propKey() : def.label);
	compSpin_->setVisible(def.type == PropType::Float || def.type == PropType::Int);
	compCheck_->setVisible(def.type == PropType::Bool);
	compSwatch_->setVisible(def.type == PropType::Color);
	if (def.type == PropType::Float || def.type == PropType::Int) {
		// The property's own range, so a keyed value cannot be pushed somewhere
		// the Inspector would not let you put it.
		compSpin_->setRange(def.min, def.max);
		compSpin_->setDecimals(def.type == PropType::Int ? 0 : 3);
		compSpin_->setSingleStep(def.type == PropType::Int
						 ? 1.0
						 : std::max(0.001, (def.max - def.min) / 100.0));
	}

	for (QWidget *w : {static_cast<QWidget *>(timeSpin_), static_cast<QWidget *>(easeCombo_),
			   compRow_, static_cast<QWidget *>(delBtn_),
			   static_cast<QWidget *>(copyBtn_)})
		w->setEnabled(have);
	// Only back into the property it came from: a PropKey's value is a bare
	// double whose meaning belongs to its property, so a colour pasted into a
	// radius would type-check and be nonsense.
	pasteBtn_->setEnabled(haveCompClip_ && compClipboardKey_ == lw->propKey());

	if (have) {
		const PropKey &key = (*keys)[k];
		timeSpin_->setMaximum(int(std::min<qint64>(clip_.outDurationMs(), 1 << 30)));
		timeSpin_->setValue(int(key.tMs));
		if (def.type == PropType::Bool)
			compCheck_->setChecked(key.v != 0.0);
		else if (def.type == PropType::Color)
			paintColorSwatch(compSwatch_, QColor::fromRgba(QRgb(quint32(key.v))));
		else
			compSpin_->setValue(key.v);
		easeCombo_->setCurrentIndex(int(key.ease));
		bez1_->setValue(key.bez1);
		bez2_->setValue(key.bez2);
		keyInfo_->setText(QStringLiteral("Key %1 of %2 on %3.")
					  .arg(k + 1)
					  .arg(keys->size())
					  .arg(lw->propDef().label));
	} else {
		const int n = keys ? keys->size() : 0;
		keyInfo_->setText(n == 0
					  ? QStringLiteral("No keyframes on this property yet.")
					  : QStringLiteral("%1 keyframe%2 — click one to edit it.")
						    .arg(n)
						    .arg(n == 1 ? QString() : QStringLiteral("s")));
	}
	form_->setRowVisible(bezRow_, have && easeCombo_->currentIndex() == int(TlEase::Bezier));
}

// The selected key of a component lane, ready to write to -- or null when the
// current tab is a transform channel or nothing is selected. Every action below
// starts here, so none of them has to repeat the four-step walk from lane to
// component to property to index.
PropKey *KeyframeEditor::selectedCompKey()
{
	KeyframeLane *lane = currentLaneWidget();
	if (!lane || !lane->isComponentLane())
		return nullptr;
	const int ci = lane->componentIndex();
	const int k = lane->selectedKey();
	if (ci < 0 || ci >= clip_.components.size() || k < 0)
		return nullptr;
	auto it = clip_.components[ci].keys.find(lane->propKey());
	if (it == clip_.components[ci].keys.end() || k >= it->size())
		return nullptr;
	return &(*it)[k];
}

void KeyframeEditor::applyCompValue(double v)
{
	KeyframeLane *lane = currentLaneWidget();
	if (!lane || !lane->isComponentLane())
		return;
	const int k = lane->selectedKey();
	const int ci = lane->componentIndex();
	if (k < 0 || ci < 0 || ci >= clip_.components.size())
		return;
	auto it = clip_.components[ci].keys.find(lane->propKey());
	if (it == clip_.components[ci].keys.end() || k >= it->size())
		return;
	(*it)[k].v = v;
	pushEdit();
}

void KeyframeEditor::syncKeyPanel()
{
	syncing_ = true;
	KeyframeLane *lw = currentLaneWidget();
	if (lw && lw->isComponentLane()) {
		syncCompPanel(lw);
		syncing_ = false;
		return;
	}
	const int lane = currentLane();
	const int k = lw ? lw->selectedKey() : -1;
	const bool have = (k >= 0 && k < clip_.keys.size() && clip_.keys[k].channel(lane).on);

	// The component row belongs to the other kind of lane.
	form_->setRowVisible(compRow_, false);

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
	if (lane->isComponentLane()) {
		// Through the lane, so a key added from the button and one added by
		// double-clicking the lane are the same operation -- including the
		// seeding rule that makes adding a key never change the picture.
		const int k = lane->addKeyAtForOwner(lane->playhead());
		clip_ = lane->clip();
		pushEdit();
		lane->selectKey(k);
		syncKeyPanel();
		return;
	}
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
	if (k < 0)
		return;
	if (lane->isComponentLane()) {
		lane->removeKeyAtForOwner(k);
		clip_ = lane->clip();
		lane->selectKey(-1);
		pushEdit();
		return;
	}
	if (k >= clip_.keys.size())
		return;
	clip_.clearKeyLane(k, currentLane());
	lane->selectKey(-1);
	pushEdit();
}

void KeyframeEditor::copySelected()
{
	KeyframeLane *lane = currentLaneWidget();
	const int k = lane ? lane->selectedKey() : -1;
	if (k < 0)
		return;
	if (lane->isComponentLane()) {
		if (const PropKey *pk = selectedCompKey()) {
			compClipboard_ = *pk;
			compClipboardKey_ = lane->propKey();
			haveCompClip_ = true;
			syncKeyPanel();
		}
		return;
	}
	if (k >= clip_.keys.size())
		return;
	clipboard_ = clip_.keys[k];
	clipboardLane_ = currentLane();
	haveClip_ = true;
	syncKeyPanel();
}

void KeyframeEditor::pasteHere()
{
	KeyframeLane *lane = currentLaneWidget();
	if (!lane)
		return;
	if (lane->isComponentLane()) {
		if (!haveCompClip_ || compClipboardKey_ != lane->propKey())
			return;
		const int k = lane->addKeyAtForOwner(lane->playhead());
		clip_ = lane->clip();
		auto it = clip_.components[lane->componentIndex()].keys.find(lane->propKey());
		if (k >= 0 && it != clip_.components[lane->componentIndex()].keys.end() &&
		    k < it->size()) {
			// The time is where the playhead is; everything else is the key
			// that was copied.
			const qint64 keep = (*it)[k].tMs;
			(*it)[k] = compClipboard_;
			(*it)[k].tMs = keep;
		}
		pushEdit();
		lane->selectKey(k);
		syncKeyPanel();
		return;
	}
	if (!haveClip_)
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
