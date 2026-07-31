#include "EditorWidgets.hpp"

#include "CanvasFit.hpp"
#include "Filmstrip.hpp"
#include "TimeText.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace harpia {

// ============================ PreviewCanvas ============================

namespace {
constexpr int kHandle = 8;
}

PreviewCanvas::PreviewCanvas(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(lp_.minW, lp_.minH);
	setMouseTracking(true);
	setStyleSheet(QStringLiteral("background:#0d0e11;"));
}

void PreviewCanvas::setLayoutParams(const PreviewLayoutParams &p)
{
	lp_ = p;
	setMinimumSize(lp_.minW, lp_.minH);
	updateGeometry();
	update();
}

void PreviewCanvas::setVideoSize(int w, int h)
{
	vw_ = w;
	vh_ = h;
	if (cropVideo_.isNull())
		cropVideo_ = QRect(0, 0, w, h);
	update();
}

void PreviewCanvas::setFrame(const QImage &img)
{
	frame_ = img;
	update();
}

void PreviewCanvas::setCropEnabled(bool on)
{
	cropEnabled_ = on;
	if (on && (cropVideo_.isNull() || cropVideo_.width() < 2))
		cropVideo_ = QRect(0, 0, vw_, vh_);
	update();
}

void PreviewCanvas::resetCrop()
{
	cropVideo_ = QRect(0, 0, vw_, vh_);
	emit cropChanged(cropVideo_);
	update();
}

void PreviewCanvas::setCropRectVideo(const QRect &r)
{
	cropVideo_ = r;
	update();
}

QRect PreviewCanvas::displayRect() const
{
	if (vw_ <= 0 || vh_ <= 0)
		return rect();
	const double s = std::min(double(width()) / vw_, double(height()) / vh_);
	const int dw = std::max(1, int(vw_ * s));
	const int dh = std::max(1, int(vh_ * s));
	return QRect((width() - dw) / 2, (height() - dh) / 2, dw, dh);
}

// Where the frame is actually painted. Normally identical to displayRect(): the
// frame is rendered at the canvas shape, so fitting it into a rect of that shape
// changes nothing.
//
// It differs only when the two disagree -- a frame left over from before a
// resolution change, say -- and then this letterboxes rather than distorts.
// Stretching a picture to fit a box it does not belong in is never the right
// answer, and it is the one thing a preview must not do: it is the reference
// for every framing decision made against it.
QRect PreviewCanvas::frameRect() const
{
	const QRect d = displayRect();
	if (frame_.isNull() || d.isEmpty())
		return d;
	const QRect r = fitRectInCanvas(frame_.size(), d.size());
	return r.isEmpty() ? d : r.translated(d.topLeft());
}

QRect PreviewCanvas::videoToWidget(const QRect &r) const
{
	const QRect d = displayRect();
	if (vw_ <= 0 || vh_ <= 0)
		return d;
	const double sx = double(d.width()) / vw_;
	const double sy = double(d.height()) / vh_;
	return QRect(d.x() + int(r.x() * sx), d.y() + int(r.y() * sy), int(r.width() * sx),
		     int(r.height() * sy));
}

QRect PreviewCanvas::widgetCropRect() const
{
	return videoToWidget(cropVideo_);
}

namespace {
// Grab radius and rotate-knob arm for the clip's transform grips. Deliberately
// the same numbers the spotlight masks use: two sets of handles in one widget
// that grabbed at different distances would feel like two different tools.
constexpr int kXfGrab = 9;
constexpr int kXfRotateArm = 26;
} // namespace

void PreviewCanvas::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.fillRect(rect(), QColor(0x0d, 0x0e, 0x11));
	const QRect d = displayRect();
	if (!frame_.isNull())
		p.drawImage(frameRect(), frame_);

	// Full editing: dashed outline of the clip being manipulated (canvas px ->
	// widget px), so it's obvious what scroll/drag will move.
	if (transformMode_ && !transformRect_.isEmpty() && vw_ > 0 && vh_ > 0) {
		const QRectF r = transformWidgetRect();
		const QColor accent(0x00, 0xae, 0xef);
		p.save();
		p.setRenderHint(QPainter::Antialiasing, true);
		// Turn the painter rather than the rect: a rotated QRectF is still
		// axis-aligned, so drawing it turned is the only way the outline sits on
		// the clip instead of around it.
		p.translate(r.center());
		p.rotate(transformRotation_);
		p.translate(-r.center());
		p.setPen(QPen(accent, 1.5, Qt::DashLine));
		p.setBrush(Qt::NoBrush);
		p.drawRect(r);
		// The stalk up to the rotate knob, drawn in the same turned frame so it
		// stays perpendicular to the top edge.
		p.setPen(QPen(accent, 1.2));
		p.drawLine(QPointF(r.center().x(), r.top()),
			   QPointF(r.center().x(), r.top() - kXfRotateArm));
		p.restore();

		// The grips themselves come back already turned, so they are drawn
		// square-on -- a rotated square grip is just harder to hit.
		p.setPen(QPen(accent, 1.2));
		p.setBrush(QColor(0x10, 0x12, 0x14));
		const QVector<QPointF> h = transformHandlePoints();
		for (int k = 0; k < h.size(); ++k) {
			if (k == h.size() - 1) { // the rotate knob is a circle, not a square
				p.setBrush(accent);
				p.drawEllipse(h[k], 5.0, 5.0);
				p.setBrush(QColor(0x10, 0x12, 0x14));
			} else {
				p.drawRect(QRectF(h[k].x() - 3.5, h[k].y() - 3.5, 7, 7));
			}
		}
	}

	// Centre guides. Only while a drag is actually snapped, so the line means
	// "this is on the centre now" rather than being permanent furniture.
	if ((guideX_ || guideY_) && !d.isEmpty()) {
		p.save();
		p.setRenderHint(QPainter::Antialiasing, false);
		p.setPen(QPen(QColor(0xff, 0xc0, 0x40, 200), 1, Qt::DashLine));
		if (guideX_) {
			const int cx = d.center().x();
			p.drawLine(cx, d.top(), cx, d.bottom());
		}
		if (guideY_) {
			const int cy = d.center().y();
			p.drawLine(d.left(), cy, d.right(), cy);
		}
		p.restore();
	}

	// The Mask component's shape, with the same grips as everything else here.
	if (mask_.on) {
		const QVector<QPointF> h = maskHandlePoints();
		if (h.size() == 9) {
			const QColor accent(0xff, 0xc0, 0x40); // amber: not the clip's blue
			p.save();
			p.setRenderHint(QPainter::Antialiasing, true);
			// The outline is drawn as a polygon through the corner grips
			// rather than a rotated rect, because it has already been through
			// both rotations and there is no single angle left to give a
			// painter. TL, TR, BR, BL -- the four corners of that list.
			QPolygonF outline;
			outline << h[0] << h[2] << h[7] << h[5];
			p.setPen(QPen(accent, 1.5, Qt::DashLine));
			p.setBrush(Qt::NoBrush);
			p.drawPolygon(outline);
			p.setPen(QPen(accent, 1.2));
			p.drawLine(h[1], h[8]); // the stalk to the rotate knob
			p.setBrush(QColor(0x10, 0x12, 0x14));
			for (int k = 0; k < 8; ++k)
				p.drawRect(QRectF(h[k].x() - 3.5, h[k].y() - 3.5, 7, 7));
			p.setBrush(accent);
			p.drawEllipse(h[8], 5.0, 5.0);
			p.restore();
		}
	}

	if (spotMode_)
		drawSpotlight(p);

	if (!cropEnabled_)
		return;

	const QRect c = widgetCropRect();
	// Dim everything outside the crop.
	QRegion outside(d);
	outside -= QRegion(c);
	p.setClipRegion(outside);
	p.fillRect(d, QColor(0, 0, 0, 130));
	p.setClipping(false);

	p.setPen(QPen(QColor(0x00, 0xae, 0xef), 2));
	p.setBrush(Qt::NoBrush);
	p.drawRect(c);
	p.setBrush(QColor(0x00, 0xae, 0xef));
	p.setPen(Qt::NoPen);
	const QPoint pts[8] = {c.topLeft(),
			       {c.center().x(), c.top()},
			       c.topRight(),
			       {c.left(), c.center().y()},
			       {c.right(), c.center().y()},
			       c.bottomLeft(),
			       {c.center().x(), c.bottom()},
			       c.bottomRight()};
	for (const QPoint &h : pts)
		p.drawRect(QRect(h.x() - kHandle / 2, h.y() - kHandle / 2, kHandle, kHandle));
}

PreviewCanvas::Zone PreviewCanvas::zoneAt(const QPoint &pos) const
{
	const QRect c = widgetCropRect();
	const int m = kHandle;
	const bool nl = std::abs(pos.x() - c.left()) <= m;
	const bool nr = std::abs(pos.x() - c.right()) <= m;
	const bool nt = std::abs(pos.y() - c.top()) <= m;
	const bool nb = std::abs(pos.y() - c.bottom()) <= m;
	if (nt && nl)
		return Zone::TL;
	if (nt && nr)
		return Zone::TR;
	if (nb && nl)
		return Zone::BL;
	if (nb && nr)
		return Zone::BR;
	if (nl)
		return Zone::L;
	if (nr)
		return Zone::R;
	if (nt)
		return Zone::T;
	if (nb)
		return Zone::B;
	if (c.contains(pos))
		return Zone::Move;
	return Zone::None;
}

void PreviewCanvas::setTransformMode(bool on)
{
	if (transformMode_ == on)
		return;
	transformMode_ = on;
	transformDragging_ = false;
	if (!on)
		transformRect_ = QRectF();
	setCursor(on ? Qt::OpenHandCursor : Qt::ArrowCursor);
	update();
}

void PreviewCanvas::setCentreGuides(bool showX, bool showY)
{
	if (guideX_ == showX && guideY_ == showY)
		return;
	guideX_ = showX;
	guideY_ = showY;
	update();
}

void PreviewCanvas::setTransformBox(const QRectF &canvasRect, double rotationDeg)
{
	if (transformRect_ == canvasRect && qFuzzyCompare(transformRotation_ + 1.0, rotationDeg + 1.0))
		return;
	transformRect_ = canvasRect;
	transformRotation_ = rotationDeg;
	update();
}

// The clip's unrotated box in widget pixels. Rotation is applied on top of this
// by whoever draws or hit-tests, never baked in -- the box is what the pose
// says, and turning it is a separate fact.
QRectF PreviewCanvas::transformWidgetRect() const
{
	if (transformRect_.isEmpty() || vw_ <= 0 || vh_ <= 0)
		return QRectF();
	const QRect d = displayRect();
	const double sx = double(d.width()) / vw_;
	const double sy = double(d.height()) / vh_;
	return QRectF(d.x() + transformRect_.x() * sx, d.y() + transformRect_.y() * sy,
		      transformRect_.width() * sx, transformRect_.height() * sy);
}

// Eight grips plus the rotation knob, in widget coordinates, already turned by
// the clip's rotation. Order matches XfZone below: TL, T, TR, L, R, BL, B, BR,
// Rotate.
QVector<QPointF> PreviewCanvas::transformHandlePoints() const
{
	QVector<QPointF> out;
	const QRectF r = transformWidgetRect();
	if (r.isEmpty())
		return out;
	const double hw = r.width() / 2.0, hh = r.height() / 2.0;
	const QPointF local[9] = {{-hw, -hh}, {0, -hh}, {hw, -hh},
				  {-hw, 0},   {hw, 0},  {-hw, hh},
				  {0, hh},    {hw, hh}, {0, -hh - kXfRotateArm}};
	const QPointF c = r.center();
	const double a = transformRotation_ * M_PI / 180.0;
	for (const QPointF &l : local)
		out.append(c + QPointF(l.x() * std::cos(a) - l.y() * std::sin(a),
				       l.x() * std::sin(a) + l.y() * std::cos(a)));
	return out;
}

void PreviewCanvas::setMaskEdit(const MaskEdit &m)
{
	mask_ = m;
	if (!m.on)
		maskDrag_ = MaskZone::None;
	update();
}

// Where the mask's grips land on screen.
//
// Two transforms, in this order: the shape sits in the CLIP's normalised space
// and is turned by its own rotation there; the clip is then placed on the
// canvas and turned by the CLIP's rotation. Applying them the other way round
// puts the grips somewhere plausible-looking and wrong the moment either angle
// is non-zero.
QVector<QPointF> PreviewCanvas::maskHandlePoints() const
{
	QVector<QPointF> out;
	const QRectF clip = transformWidgetRect();
	if (!mask_.on || clip.isEmpty())
		return out;

	// Half-extents in widget pixels. A Circle takes its height from its width,
	// exactly as maskPath does, or the grips would sit off the shape.
	const double hw = mask_.w * clip.width() / 2.0;
	const double hh = (mask_.shape == SpotShape::Circle) ? hw : mask_.h * clip.height() / 2.0;
	const QPointF local[9] = {{-hw, -hh}, {0, -hh}, {hw, -hh},
				  {-hw, 0},   {hw, 0},  {-hw, hh},
				  {0, hh},    {hw, hh}, {0, -hh - kXfRotateArm}};

	// The shape's centre, inside the clip's rect, before the clip is turned.
	const QPointF centreInClip(clip.x() + mask_.cx * clip.width(),
				   clip.y() + mask_.cy * clip.height());
	const double ma = mask_.rotation * M_PI / 180.0;
	const double ca = transformRotation_ * M_PI / 180.0;
	const QPointF clipCentre = clip.center();
	for (const QPointF &l : local) {
		// The shape's own rotation, about the shape's centre.
		const QPointF turned(l.x() * std::cos(ma) - l.y() * std::sin(ma),
				     l.x() * std::sin(ma) + l.y() * std::cos(ma));
		const QPointF inClip = centreInClip + turned;
		// Then the clip's rotation, about the clip's centre.
		const QPointF v = inClip - clipCentre;
		out.append(clipCentre + QPointF(v.x() * std::cos(ca) - v.y() * std::sin(ca),
						v.x() * std::sin(ca) + v.y() * std::cos(ca)));
	}
	return out;
}

QPointF PreviewCanvas::widgetToClipNorm(const QPointF &p) const
{
	const QRectF clip = transformWidgetRect();
	if (clip.isEmpty())
		return {};
	// Undo the clip's rotation first, then express the result as a fraction of
	// the clip's own rect -- the inverse of maskHandlePoints' second step.
	const QPointF centre = clip.center();
	const QPointF v = p - centre;
	const double a = -transformRotation_ * M_PI / 180.0;
	const QPointF un(v.x() * std::cos(a) - v.y() * std::sin(a),
			 v.x() * std::sin(a) + v.y() * std::cos(a));
	const QPointF inClip = centre + un;
	return QPointF((inClip.x() - clip.x()) / clip.width(),
		       (inClip.y() - clip.y()) / clip.height());
}

PreviewCanvas::MaskZone PreviewCanvas::maskZoneAt(const QPoint &pos) const
{
	if (!mask_.on)
		return MaskZone::None;
	const QVector<QPointF> h = maskHandlePoints();
	if (h.size() != 9)
		return MaskZone::None;
	static const MaskZone zones[9] = {MaskZone::TL, MaskZone::T,  MaskZone::TR,
					  MaskZone::L,  MaskZone::R,  MaskZone::BL,
					  MaskZone::B,  MaskZone::BR, MaskZone::Rotate};
	for (int k = 0; k < h.size(); ++k) {
		const QPointF v = h[k] - QPointF(pos);
		if (std::abs(v.x()) <= kXfGrab && std::abs(v.y()) <= kXfGrab)
			return zones[k];
	}
	// Inside the shape moves it. Tested in the shape's own axes so a turned
	// mask is grabbed where it looks like it is.
	const QPointF n = widgetToClipNorm(QPointF(pos));
	const double dx = n.x() - mask_.cx, dy = n.y() - mask_.cy;
	const double a = -mask_.rotation * M_PI / 180.0;
	const double lx = dx * std::cos(a) - dy * std::sin(a);
	const double ly = dx * std::sin(a) + dy * std::cos(a);
	if (std::abs(lx) <= mask_.w / 2.0 && std::abs(ly) <= mask_.h / 2.0)
		return MaskZone::Move;
	return MaskZone::None;
}

PreviewCanvas::XfZone PreviewCanvas::transformZoneAt(const QPoint &pos) const
{
	const QRectF r = transformWidgetRect();
	if (r.isEmpty())
		return XfZone::None;
	static const XfZone zones[9] = {XfZone::TL, XfZone::T,  XfZone::TR,
					XfZone::L,  XfZone::R,  XfZone::BL,
					XfZone::B,  XfZone::BR, XfZone::Rotate};
	const QVector<QPointF> h = transformHandlePoints();
	for (int k = 0; k < h.size(); ++k) {
		const QPointF v = h[k] - QPointF(pos);
		if (std::abs(v.x()) <= kXfGrab && std::abs(v.y()) <= kXfGrab)
			return zones[k];
	}
	// Inside the box (in ITS axes, so a tilted clip is grabbed where it looks
	// like it is) moves the clip -- which is what the whole canvas used to do.
	const QPointF c = r.center();
	const QPointF v = QPointF(pos) - c;
	const double a = -transformRotation_ * M_PI / 180.0;
	const QPointF local(v.x() * std::cos(a) - v.y() * std::sin(a),
			    v.x() * std::sin(a) + v.y() * std::cos(a));
	if (std::abs(local.x()) <= r.width() / 2.0 && std::abs(local.y()) <= r.height() / 2.0)
		return XfZone::Move;
	return XfZone::None;
}

// ---- Spotlight handles ------------------------------------------------------
//
// The Spotlight model has always been keyframable and pixel-exact; what it did
// not have was a way to place a mask by looking at the picture. Everything here
// is that: the shape drawn where it really is, eight grips, a rotation arm, and
// snapping to the canvas's own landmarks.
//
// All coordinates are normalised to the canvas (0..1) because that is what the
// model stores — the same numbers render identically at preview size and at
// export size, which is the guarantee the whole compositor is built on.

namespace {
constexpr int kSpotGrip = 5;       // half-size of a grip square, widget px
constexpr int kSpotGrab = 9;       // how close the cursor has to be to grab one
constexpr int kSpotRotateArm = 26; // distance from the top edge to the rotate knob
constexpr double kSnapTolPx = 7.0; // snapping reach, in widget pixels
} // namespace

void PreviewCanvas::setSpotlightMode(bool on)
{
	if (spotMode_ == on)
		return;
	spotMode_ = on;
	spotDrag_ = SpotZone::None;
	spotGuideHOn_ = spotGuideVOn_ = false;
	setMouseTracking(true); // so the cursor can change over a grip
	setCursor(Qt::ArrowCursor);
	update();
}

void PreviewCanvas::setSpotlightMasks(const QVector<SpotDraw> &masks, int selected)
{
	// A drag owns the pose it is editing; letting the window push a new one
	// mid-gesture would fight the mouse.
	if (spotDrag_ != SpotZone::None)
		return;
	spotMasks_ = masks;
	spotSel_ = selected;
	update();
}

QPointF PreviewCanvas::canvasToWidgetF(double nx, double ny) const
{
	const QRect d = displayRect();
	return QPointF(d.x() + nx * d.width(), d.y() + ny * d.height());
}

QPointF PreviewCanvas::widgetToCanvasF(const QPointF &p) const
{
	const QRect d = displayRect();
	if (d.width() <= 0 || d.height() <= 0)
		return QPointF();
	return QPointF((p.x() - d.x()) / d.width(), (p.y() - d.y()) / d.height());
}

// Widget pixels -> the mask's own axes, in widget-pixel units with the origin at
// the mask's centre. Un-rotating here is what makes a grip on a turned mask
// behave the way it looks: dragging the right edge widens it along ITS width.
QPointF PreviewCanvas::maskLocal(int i, const QPointF &widgetPt) const
{
	if (i < 0 || i >= spotMasks_.size())
		return QPointF();
	const SpotPose &po = spotMasks_[i].pose;
	const QPointF c = canvasToWidgetF(po.cx, po.cy);
	const QPointF v = widgetPt - c;
	const double a = -po.rotation * M_PI / 180.0;
	return QPointF(v.x() * std::cos(a) - v.y() * std::sin(a),
		       v.x() * std::sin(a) + v.y() * std::cos(a));
}

// The eight grips plus the rotation knob, in WIDGET coordinates, already turned
// by the mask's rotation. Order: TL, T, TR, L, R, BL, B, BR, Rotate.
QVector<QPointF> PreviewCanvas::spotHandlePoints(int i) const
{
	QVector<QPointF> out;
	if (i < 0 || i >= spotMasks_.size())
		return out;
	const QRect d = displayRect();
	const SpotPose &po = spotMasks_[i].pose;
	// A Circle takes its height from its width in PIXELS, exactly as the
	// renderer does, or the handles would sit off the shape on a non-square
	// canvas.
	const double wpx = po.w * d.width();
	const double hpx = (spotMasks_[i].shape == SpotShape::Circle) ? wpx : po.h * d.height();
	const double hw = wpx / 2.0, hh = hpx / 2.0;
	const QPointF local[9] = {{-hw, -hh}, {0, -hh},  {hw, -hh},
				  {-hw, 0},   {hw, 0},   {-hw, hh},
				  {0, hh},    {hw, hh},  {0, -hh - kSpotRotateArm}};
	const QPointF c = canvasToWidgetF(po.cx, po.cy);
	const double a = po.rotation * M_PI / 180.0;
	for (const QPointF &l : local)
		out.append(c + QPointF(l.x() * std::cos(a) - l.y() * std::sin(a),
				       l.x() * std::sin(a) + l.y() * std::cos(a)));
	return out;
}

PreviewCanvas::SpotZone PreviewCanvas::spotZoneAt(const QPoint &pos, int *maskOut) const
{
	// The selected mask is tested first and on its own: its grips must win over
	// another mask's body, or a mask sitting under a grip would steal the drag.
	auto testGrips = [&](int i) -> SpotZone {
		const QVector<QPointF> h = spotHandlePoints(i);
		static const SpotZone zones[9] = {SpotZone::TL, SpotZone::T,  SpotZone::TR,
						  SpotZone::L,  SpotZone::R,  SpotZone::BL,
						  SpotZone::B,  SpotZone::BR, SpotZone::Rotate};
		for (int k = 0; k < h.size(); ++k) {
			const QPointF v = h[k] - QPointF(pos);
			if (std::abs(v.x()) <= kSpotGrab && std::abs(v.y()) <= kSpotGrab)
				return zones[k];
		}
		return SpotZone::None;
	};
	if (spotSel_ >= 0 && spotSel_ < spotMasks_.size()) {
		const SpotZone z = testGrips(spotSel_);
		if (z != SpotZone::None) {
			if (maskOut)
				*maskOut = spotSel_;
			return z;
		}
	}
	// Then bodies, front to back — later masks are drawn over earlier ones, so
	// they should be picked first.
	for (int i = spotMasks_.size() - 1; i >= 0; --i) {
		SpotMask m;
		m.shape = spotMasks_[i].shape;
		if (Spotlight::maskPath(m, spotMasks_[i].pose, displayRect().size())
			    .translated(displayRect().topLeft())
			    .contains(QPointF(pos))) {
			if (maskOut)
				*maskOut = i;
			return SpotZone::Move;
		}
	}
	if (maskOut)
		*maskOut = -1;
	return SpotZone::None;
}

// Snap to the canvas's own landmarks: the edges, the middle, and the thirds
// (which is where a spotlight usually wants to sit). The tolerance is in widget
// pixels so it feels the same however far the preview is zoomed.
double PreviewCanvas::snapNorm(double v, bool horizontal) const
{
	if (!spotSnap_)
		return v;
	const QRect d = displayRect();
	const double px = horizontal ? d.width() : d.height();
	if (px <= 0)
		return v;
	const double tol = kSnapTolPx / px;
	static const double marks[] = {0.0, 1.0 / 3.0, 0.5, 2.0 / 3.0, 1.0};
	for (double m : marks)
		if (std::abs(v - m) <= tol)
			return m;
	return v;
}

void PreviewCanvas::drawSpotlight(QPainter &p) const
{
	const QRect d = displayRect();
	if (d.width() <= 0 || spotMasks_.isEmpty())
		return;
	p.save();
	p.setRenderHint(QPainter::Antialiasing, true);
	for (int i = 0; i < spotMasks_.size(); ++i) {
		SpotMask m;
		m.shape = spotMasks_[i].shape;
		const QPainterPath path =
			Spotlight::maskPath(m, spotMasks_[i].pose, d.size()).translated(d.topLeft());
		const bool sel = (i == spotSel_);
		const bool off = !spotMasks_[i].enabled;
		// A disabled mask is still shown, dashed and grey, because "it is there
		// but doing nothing" is otherwise indistinguishable from "it is gone".
		QColor c = off ? QColor(0x7f, 0x85, 0x8e)
			       : (sel ? QColor(0x00, 0xae, 0xef) : QColor(0xff, 0xd4, 0x4f));
		p.setBrush(Qt::NoBrush);
		// A dark line under the bright one, so the outline reads on a light
		// picture as well as a dark one.
		p.setPen(QPen(QColor(0, 0, 0, 140), sel ? 3.5 : 2.5));
		p.drawPath(path);
		p.setPen(QPen(c, sel ? 2.0 : 1.2, off ? Qt::DashLine : Qt::SolidLine));
		p.drawPath(path);

		if (!sel)
			continue;
		const QVector<QPointF> h = spotHandlePoints(i);
		if (h.size() < 9)
			continue;
		// The rotation arm, then the knob at the end of it.
		p.setPen(QPen(c, 1.2));
		p.drawLine(h[1], h[8]);
		p.setBrush(c);
		p.setPen(QPen(QColor(0x10, 0x12, 0x16), 1.2));
		p.drawEllipse(h[8], kSpotGrip, kSpotGrip);
		for (int k = 0; k < 8; ++k)
			p.drawRect(QRectF(h[k].x() - kSpotGrip, h[k].y() - kSpotGrip,
					  kSpotGrip * 2, kSpotGrip * 2));
	}
	// Snap guides, drawn only while a snap is actually holding.
	if (spotDrag_ != SpotZone::None && (spotGuideHOn_ || spotGuideVOn_)) {
		p.setPen(QPen(QColor(0x3d, 0xdc, 0x97), 1, Qt::DashLine));
		if (spotGuideVOn_)
			p.drawLine(QPointF(spotGuideV_.x(), d.top()),
				   QPointF(spotGuideV_.x(), d.bottom()));
		if (spotGuideHOn_)
			p.drawLine(QPointF(d.left(), spotGuideH_.y()),
				   QPointF(d.right(), spotGuideH_.y()));
	}
	p.restore();
}

void PreviewCanvas::wheelEvent(QWheelEvent *e)
{
	// Wheel zooms the clip under the cursor (Full editing). Anything else keeps
	// the default behaviour so the event can propagate.
	if (!transformMode_ || cropEnabled_) {
		QWidget::wheelEvent(e);
		return;
	}
	const QRect d = displayRect();
	if (d.width() <= 0 || d.height() <= 0) {
		QWidget::wheelEvent(e);
		return;
	}
	const int dy = e->angleDelta().y();
	if (dy == 0) {
		QWidget::wheelEvent(e);
		return;
	}
	const double factor = std::pow(1.15, dy / 120.0);
	const QPointF pos = e->position();
	const double cx = std::clamp(double(pos.x() - d.x()) / d.width(), 0.0, 1.0);
	const double cy = std::clamp(double(pos.y() - d.y()) / d.height(), 0.0, 1.0);
	emit transformZoomed(factor, cx, cy);
	e->accept();
}

void PreviewCanvas::mousePressEvent(QMouseEvent *e)
{
	// Right-click anywhere on the drawn picture. Reported before anything else
	// looks at the event: none of the drag modes below use the right button, and
	// a menu that only appears when no tool happens to be armed is a menu people
	// conclude does not exist.
	if (e->button() == Qt::RightButton) {
		const QRect d = displayRect();
		if (d.width() > 0 && d.height() > 0 && d.contains(e->pos())) {
			emit contextRequested(double(e->pos().x() - d.x()) / d.width(),
					      double(e->pos().y() - d.y()) / d.height(),
					      e->globalPosition().toPoint());
			return;
		}
	}
	// Spotlight editing takes precedence over clip transform: both drag with the
	// left button, and while the Spotlight panel is open the masks are what you
	// mean to move.
	if (spotMode_ && !cropEnabled_ && e->button() == Qt::LeftButton) {
		int mask = -1;
		const SpotZone z = spotZoneAt(e->pos(), &mask);
		if (mask != spotSel_)
			emit spotlightSelected(mask);
		if (z == SpotZone::None || mask < 0) {
			spotSel_ = mask;
			update();
			return;
		}
		spotSel_ = mask;
		spotDrag_ = z;
		spotDragMask_ = mask;
		spotStartPose_ = spotMasks_[mask].pose;
		spotStartLocal_ = maskLocal(mask, QPointF(e->pos()));
		spotStartCanvas_ = widgetToCanvasF(QPointF(e->pos()));
		spotSnap_ = !(e->modifiers() & Qt::ShiftModifier);
		update();
		return;
	}
	// The mask is tested BEFORE the clip's own transform: when a mask is being
	// edited its grips sit on top of the clip's, and the one under the pointer
	// has to be the one the user can see.
	if (mask_.on && !cropEnabled_ && e->button() == Qt::LeftButton) {
		const MaskZone z = maskZoneAt(e->pos());
		if (z != MaskZone::None) {
			maskDrag_ = z;
			maskStart_ = mask_;
			maskStartNorm_ = widgetToClipNorm(QPointF(e->pos()));
			const QVector<QPointF> h = maskHandlePoints();
			if (h.size() == 9) {
				const QPointF c = (h[0] + h[7]) / 2.0;
				const QPointF v = QPointF(e->pos()) - c;
				maskStartAngle_ = std::atan2(v.y(), v.x()) * 180.0 / M_PI;
			}
			setCursor(z == MaskZone::Move ? Qt::ClosedHandCursor : Qt::CrossCursor);
			return;
		}
	}
	if (transformMode_ && !cropEnabled_ && e->button() == Qt::LeftButton) {
		const XfZone z = transformZoneAt(e->pos());
		// Outside the clip entirely: let the press fall through rather than
		// starting a drag the user cannot see the effect of. Dragging empty
		// canvas used to move the selected clip from anywhere, which made it
		// very easy to nudge something you were not looking at.
		if (z == XfZone::None)
			return;
		xfDrag_ = z;
		transformLast_ = e->pos();
		const QRectF r = transformWidgetRect();
		xfPressRect_ = r;
		xfPressRotation_ = transformRotation_;
		const QPointF v = QPointF(e->pos()) - r.center();
		xfPressAngle_ = std::atan2(v.y(), v.x()) * 180.0 / M_PI;
		const double a = -transformRotation_ * M_PI / 180.0;
		xfPressLocal_ = QPointF(v.x() * std::cos(a) - v.y() * std::sin(a),
					v.x() * std::sin(a) + v.y() * std::cos(a));
		transformDragging_ = z == XfZone::Move;
		setCursor(z == XfZone::Move ? Qt::ClosedHandCursor : Qt::CrossCursor);
		return;
	}
	if (!cropEnabled_ || e->button() != Qt::LeftButton)
		return;
	drag_ = zoneAt(e->pos());
	dragStart_ = e->pos();
	dragStartCrop_ = widgetCropRect();
}

void PreviewCanvas::mouseMoveEvent(QMouseEvent *e)
{
	if (spotMode_ && !cropEnabled_) {
		if (spotDrag_ == SpotZone::None) {
			// Not dragging: just tell the cursor what it is over.
			int mask = -1;
			const SpotZone z = spotZoneAt(e->pos(), &mask);
			switch (z) {
			case SpotZone::Move: setCursor(Qt::SizeAllCursor); break;
			case SpotZone::L:
			case SpotZone::R: setCursor(Qt::SizeHorCursor); break;
			case SpotZone::T:
			case SpotZone::B: setCursor(Qt::SizeVerCursor); break;
			case SpotZone::TL:
			case SpotZone::BR: setCursor(Qt::SizeFDiagCursor); break;
			case SpotZone::TR:
			case SpotZone::BL: setCursor(Qt::SizeBDiagCursor); break;
			case SpotZone::Rotate: setCursor(Qt::CrossCursor); break;
			case SpotZone::None: setCursor(Qt::ArrowCursor); break;
			}
			return;
		}
		if (!(e->buttons() & Qt::LeftButton))
			return;
		spotSnap_ = !(e->modifiers() & Qt::ShiftModifier);
		const QRect d = displayRect();
		if (d.width() <= 0 || d.height() <= 0 || spotDragMask_ < 0)
			return;
		SpotPose po = spotStartPose_;
		spotGuideHOn_ = spotGuideVOn_ = false;

		if (spotDrag_ == SpotZone::Move) {
			const QPointF now = widgetToCanvasF(QPointF(e->pos()));
			double cx = spotStartPose_.cx + (now.x() - spotStartCanvas_.x());
			double cy = spotStartPose_.cy + (now.y() - spotStartCanvas_.y());
			// Snap the CENTRE and both edges: lining a spotlight up with the
			// middle of the frame and lining its edge up with the frame's edge
			// are both things people do.
			const double halfW = po.w / 2.0, halfH = po.h / 2.0;
			const double sc = snapNorm(cx, true);
			const double sl = snapNorm(cx - halfW, true) + halfW;
			const double sr = snapNorm(cx + halfW, true) - halfW;
			for (double cand : {sc, sl, sr})
				if (cand != cx) {
					cx = cand;
					spotGuideVOn_ = true;
					break;
				}
			const double scv = snapNorm(cy, false);
			const double st = snapNorm(cy - halfH, false) + halfH;
			const double sb = snapNorm(cy + halfH, false) - halfH;
			for (double cand : {scv, st, sb})
				if (cand != cy) {
					cy = cand;
					spotGuideHOn_ = true;
					break;
				}
			po.cx = cx;
			po.cy = cy;
			spotGuideV_ = canvasToWidgetF(po.cx, po.cy);
			spotGuideH_ = spotGuideV_;
		} else if (spotDrag_ == SpotZone::Rotate) {
			const QPointF c = canvasToWidgetF(spotStartPose_.cx, spotStartPose_.cy);
			const QPointF v = QPointF(e->pos()) - c;
			// The knob sits ABOVE the shape, so zero degrees is straight up.
			double deg = std::atan2(v.x(), -v.y()) * 180.0 / M_PI;
			if (spotSnap_) {
				// Every 15 degrees, the same increment every editor uses.
				const double step = 15.0;
				const double snapped = std::round(deg / step) * step;
				if (std::abs(deg - snapped) < 4.0)
					deg = snapped;
			}
			po.rotation = deg;
		} else {
			// A resize works in the mask's own axes: how far the grip moved
			// from where it was grabbed, measured after un-rotating.
			const QPointF now = maskLocal(spotDragMask_, QPointF(e->pos()));
			const QPointF delta = now - spotStartLocal_;
			const double startWpx = spotStartPose_.w * d.width();
			const double startHpx = (spotMasks_[spotDragMask_].shape == SpotShape::Circle)
							? startWpx
							: spotStartPose_.h * d.height();
			double left = -startWpx / 2.0, right = startWpx / 2.0;
			double top = -startHpx / 2.0, bottom = startHpx / 2.0;
			const bool wl = spotDrag_ == SpotZone::L || spotDrag_ == SpotZone::TL ||
					spotDrag_ == SpotZone::BL;
			const bool wr = spotDrag_ == SpotZone::R || spotDrag_ == SpotZone::TR ||
					spotDrag_ == SpotZone::BR;
			const bool wt = spotDrag_ == SpotZone::T || spotDrag_ == SpotZone::TL ||
					spotDrag_ == SpotZone::TR;
			const bool wb = spotDrag_ == SpotZone::B || spotDrag_ == SpotZone::BL ||
					spotDrag_ == SpotZone::BR;
			if (wl)
				left += delta.x();
			if (wr)
				right += delta.x();
			if (wt)
				top += delta.y();
			if (wb)
				bottom += delta.y();
			// Never let an edge cross its opposite: a mask with negative size
			// is not a thing the renderer can draw.
			const double minPx = 8.0;
			if (right - left < minPx) {
				if (wl)
					left = right - minPx;
				else
					right = left + minPx;
			}
			if (bottom - top < minPx) {
				if (wt)
					top = bottom - minPx;
				else
					bottom = top + minPx;
			}
			const double newWpx = right - left, newHpx = bottom - top;
			// The centre moves by half of whatever the edge moved, in the
			// mask's axes, then back into canvas terms.
			const QPointF shiftLocal((left + right) / 2.0, (top + bottom) / 2.0);
			const double a = spotStartPose_.rotation * M_PI / 180.0;
			const QPointF shiftWidget(
				shiftLocal.x() * std::cos(a) - shiftLocal.y() * std::sin(a),
				shiftLocal.x() * std::sin(a) + shiftLocal.y() * std::cos(a));
			po.cx = spotStartPose_.cx + shiftWidget.x() / d.width();
			po.cy = spotStartPose_.cy + shiftWidget.y() / d.height();
			po.w = newWpx / d.width();
			po.h = (spotMasks_[spotDragMask_].shape == SpotShape::Circle)
					? po.w
					: newHpx / d.height();
		}
		// Keep the model's own bounds: a pose outside them would render, but it
		// would also come back from a project file clamped and appear to jump.
		po.w = std::clamp(po.w, 0.01, 4.0);
		po.h = std::clamp(po.h, 0.01, 4.0);
		spotMasks_[spotDragMask_].pose = po;
		emit spotlightPoseChanged(spotDragMask_, po);
		update();
		return;
	}
	if (maskDrag_ != MaskZone::None && (e->buttons() & Qt::LeftButton)) {
		MaskEdit m = maskStart_;
		const QPointF now = widgetToClipNorm(QPointF(e->pos()));
		if (maskDrag_ == MaskZone::Move) {
			m.cx = maskStart_.cx + (now.x() - maskStartNorm_.x());
			m.cy = maskStart_.cy + (now.y() - maskStartNorm_.y());
		} else if (maskDrag_ == MaskZone::Rotate) {
			const QVector<QPointF> h = maskHandlePoints();
			if (h.size() == 9) {
				const QPointF c = (h[0] + h[7]) / 2.0;
				const QPointF v = QPointF(e->pos()) - c;
				double deg = maskStart_.rotation +
					     (std::atan2(v.y(), v.x()) * 180.0 / M_PI - maskStartAngle_);
				if (e->modifiers() & Qt::ShiftModifier)
					deg = std::round(deg / 15.0) * 15.0;
				while (deg > 180.0)
					deg -= 360.0;
				while (deg < -180.0)
					deg += 360.0;
				m.rotation = deg;
			}
		} else {
			// A size grip. Measured in the SHAPE's own axes, so dragging the
			// right edge of a turned mask widens it along its own width rather
			// than along the screen's.
			const double a = -maskStart_.rotation * M_PI / 180.0;
			const auto toLocal = [&](const QPointF &n) {
				const double dx = n.x() - maskStart_.cx, dy = n.y() - maskStart_.cy;
				return QPointF(dx * std::cos(a) - dy * std::sin(a),
					       dx * std::sin(a) + dy * std::cos(a));
			};
			const QPointF l0 = toLocal(maskStartNorm_), l1 = toLocal(now);
			const bool right = maskDrag_ == MaskZone::R || maskDrag_ == MaskZone::TR ||
					   maskDrag_ == MaskZone::BR;
			const bool left = maskDrag_ == MaskZone::L || maskDrag_ == MaskZone::TL ||
					  maskDrag_ == MaskZone::BL;
			const bool bottom = maskDrag_ == MaskZone::B || maskDrag_ == MaskZone::BL ||
					    maskDrag_ == MaskZone::BR;
			const bool top = maskDrag_ == MaskZone::T || maskDrag_ == MaskZone::TL ||
					 maskDrag_ == MaskZone::TR;
			const double dx = l1.x() - l0.x(), dy = l1.y() - l0.y();
			// The opposite edge stays put: the size changes by the drag and the
			// centre by half of it, which is what "drag this edge" means.
			if (right) {
				m.w = std::max(0.01, maskStart_.w + dx);
			} else if (left) {
				m.w = std::max(0.01, maskStart_.w - dx);
			}
			if (bottom) {
				m.h = std::max(0.01, maskStart_.h + dy);
			} else if (top) {
				m.h = std::max(0.01, maskStart_.h - dy);
			}
			// Move the centre by half the size change, along the shape's axes,
			// so the grip follows the pointer and the far side does not walk.
			const double sw = (right ? (m.w - maskStart_.w) : 0) -
					  (left ? (m.w - maskStart_.w) : 0);
			const double sh = (bottom ? (m.h - maskStart_.h) : 0) -
					  (top ? (m.h - maskStart_.h) : 0);
			const double fa = maskStart_.rotation * M_PI / 180.0;
			m.cx = maskStart_.cx + (sw / 2.0) * std::cos(fa) - (sh / 2.0) * std::sin(fa);
			m.cy = maskStart_.cy + (sw / 2.0) * std::sin(fa) + (sh / 2.0) * std::cos(fa);
		}
		mask_ = m;
		update();
		emit maskPoseChanged(m.cx, m.cy, m.w, m.h, m.rotation);
		return;
	}
	if (xfDrag_ != XfZone::None && (e->buttons() & Qt::LeftButton)) {
		const QRect d = displayRect();
		if (d.width() <= 0 || d.height() <= 0)
			return;
		if (xfDrag_ == XfZone::Move) {
			const QPoint delta = e->pos() - transformLast_;
			transformLast_ = e->pos();
			emit transformDragged(double(delta.x()) / d.width(),
					      double(delta.y()) / d.height());
			return;
		}
		if (xfDrag_ == XfZone::Rotate) {
			const QPointF v = QPointF(e->pos()) - xfPressRect_.center();
			double deg = xfPressRotation_ +
				     (std::atan2(v.y(), v.x()) * 180.0 / M_PI - xfPressAngle_);
			// Shift snaps to 15 degrees. Free rotation is the default because
			// the snap is the special case people ask for, not the other way
			// round -- and it matches the masks, which snap on Shift too.
			if (e->modifiers() & Qt::ShiftModifier)
				deg = std::round(deg / 15.0) * 15.0;
			while (deg > 180.0)
				deg -= 360.0;
			while (deg < -180.0)
				deg += 360.0;
			emit transformRotated(deg);
			return;
		}

		// A resize grip. Only uniform scale exists in a clip's pose, so the
		// factor is how much further from the centre the pointer is along the
		// axis this grip owns -- which makes a side grip do something honest
		// instead of nothing.
		const QPointF v = QPointF(e->pos()) - xfPressRect_.center();
		const double a = -xfPressRotation_ * M_PI / 180.0;
		const QPointF local(v.x() * std::cos(a) - v.y() * std::sin(a),
				    v.x() * std::sin(a) + v.y() * std::cos(a));
		double f = 1.0;
		const bool useX = xfDrag_ == XfZone::L || xfDrag_ == XfZone::R;
		const bool useY = xfDrag_ == XfZone::T || xfDrag_ == XfZone::B;
		if (useX) {
			if (std::abs(xfPressLocal_.x()) > 1.0)
				f = local.x() / xfPressLocal_.x();
		} else if (useY) {
			if (std::abs(xfPressLocal_.y()) > 1.0)
				f = local.y() / xfPressLocal_.y();
		} else {
			// A corner: the ratio of the distances from the centre, so the
			// grip tracks the pointer diagonally.
			const double d0 = std::hypot(xfPressLocal_.x(), xfPressLocal_.y());
			const double d1 = std::hypot(local.x(), local.y());
			if (d0 > 1.0)
				f = d1 / d0;
		}
		// Dragging a grip PAST the centre would flip the factor negative and
		// turn the clip inside out; a clip has no negative scale, so the drag
		// stops at very small instead.
		f = std::clamp(f, 0.02, 50.0);
		emit transformScaled(f, 0.0, 0.0);
		return;
	}
	if (drag_ == Zone::None || !(e->buttons() & Qt::LeftButton))
		return;
	const QPoint delta = e->pos() - dragStart_;
	QRect g = dragStartCrop_;
	switch (drag_) {
	case Zone::Move:
		g.translate(delta);
		break;
	case Zone::L:
		g.setLeft(g.left() + delta.x());
		break;
	case Zone::R:
		g.setRight(g.right() + delta.x());
		break;
	case Zone::T:
		g.setTop(g.top() + delta.y());
		break;
	case Zone::B:
		g.setBottom(g.bottom() + delta.y());
		break;
	case Zone::TL:
		g.setTopLeft(g.topLeft() + delta);
		break;
	case Zone::TR:
		g.setTopRight(g.topRight() + delta);
		break;
	case Zone::BL:
		g.setBottomLeft(g.bottomLeft() + delta);
		break;
	case Zone::BR:
		g.setBottomRight(g.bottomRight() + delta);
		break;
	default:
		break;
	}
	applyWidgetCrop(g.normalized());
}

void PreviewCanvas::mouseReleaseEvent(QMouseEvent *)
{
	if (spotDrag_ != SpotZone::None) {
		spotDrag_ = SpotZone::None;
		spotDragMask_ = -1;
		spotGuideHOn_ = spotGuideVOn_ = false;
		emit spotlightEditFinished(); // one undo step for the whole gesture
		update();
		return;
	}
	drag_ = Zone::None;
	if (maskDrag_ != MaskZone::None) {
		maskDrag_ = MaskZone::None;
		setCursor(transformMode_ ? Qt::OpenHandCursor : Qt::ArrowCursor);
		emit maskEditFinished();
		return;
	}
	if (xfDrag_ != XfZone::None) {
		xfDrag_ = XfZone::None;
		transformDragging_ = false;
		setCursor(transformMode_ ? Qt::OpenHandCursor : Qt::ArrowCursor);
		emit transformEditFinished();
	}
}

void PreviewCanvas::applyWidgetCrop(const QRect &widgetRect)
{
	const QRect d = displayRect();
	if (vw_ <= 0 || vh_ <= 0 || d.width() <= 0)
		return;
	// Clamp to the displayed frame, then map widget px -> source px.
	QRect w = widgetRect.intersected(d);
	if (w.width() < 8 || w.height() < 8)
		return;
	const double sx = double(vw_) / d.width();
	const double sy = double(vh_) / d.height();
	QRect v(int((w.x() - d.x()) * sx), int((w.y() - d.y()) * sy), int(w.width() * sx),
		int(w.height() * sy));
	v = v.intersected(QRect(0, 0, vw_, vh_));
	cropVideo_ = v;
	emit cropChanged(cropVideo_);
	update();
}

// ============================== Timeline ==============================

Timeline::Timeline(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(lp_.barTop + lp_.barH + 22);
	setMouseTracking(true);
	setToolTip(QStringLiteral("Scroll to zoom, Shift+scroll to pan"));
}

void Timeline::setLayoutParams(const TimelineLayoutParams &p)
{
	lp_ = p;
	zoom_ = std::clamp(zoom_, 1.0, lp_.maxZoom);
	if (duration_ > 0)
		clampView();
	setMinimumHeight(lp_.barTop + lp_.barH + 22);
	stripCache_ = QPixmap(); // geometry changed — rebuild the filmstrip
	updateGeometry();
	update();
}

void Timeline::setDuration(qint64 ms)
{
	duration_ = std::max<qint64>(1, ms);
	start_ = 0;
	end_ = duration_;
	playhead_ = 0;
	zoom_ = 1.0;
	viewStart_ = 0;
	update();
}

void Timeline::setThumbs(const QVector<QImage> &thumbs)
{
	thumbs_ = thumbs;
	++thumbsRev_; // invalidates the strip cache
	update();
}

void Timeline::ensureStripCache(const QRect &bar)
{
	const qreal dpr = devicePixelRatioF();
	if (!stripCache_.isNull() && stripCacheSize_ == bar.size() && stripCacheZoom_ == zoom_ &&
	    stripCacheView_ == viewStart_ && stripCacheRev_ == thumbsRev_ && stripCacheDpr_ == dpr)
		return;
	stripCacheSize_ = bar.size();
	stripCacheZoom_ = zoom_;
	stripCacheView_ = viewStart_;
	stripCacheRev_ = thumbsRev_;
	stripCacheDpr_ = dpr;

	stripCache_ = QPixmap(bar.size() * dpr);
	stripCache_.setDevicePixelRatio(dpr);
	stripCache_.fill(Qt::transparent);
	QPainter cp(&stripCache_);
	cp.setRenderHint(QPainter::Antialiasing);
	const QRect local(0, 0, bar.width(), bar.height());
	cp.setPen(Qt::NoPen);
	cp.setBrush(QColor(0x2b, 0x2d, 0x31));
	cp.drawRoundedRect(local, 4, 4);

	if (!thumbs_.isEmpty()) {
		QPainterPath clip;
		clip.addRoundedRect(local, 4, 4);
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
		// The bar shows [viewStart, viewStart+visible] of the source across its
		// whole width. filmstripTiles turns that into one tile per DISTINCT
		// frame, so zooming in spaces the thumbnails out instead of repeating
		// them (see Filmstrip.hpp).
		const qint64 fromMs = xToMs(bar.x());
		const qint64 spanMs = std::max<qint64>(1, xToMs(bar.x() + local.width()) - fromMs);
		for (const StripTile &t : filmstripTiles(n, duration_, fromMs, spanMs, 0, local.width(),
							 tileW, tileGap, 0, local.width())) {
			if (!thumbs_[t.index].isNull())
				cp.drawImage(QRect(t.x, 0, tileW, tileH), thumbs_[t.index]);
		}
	}
}

qint64 Timeline::visibleMs() const
{
	return std::max<qint64>(1, qint64(duration_ / zoom_));
}

void Timeline::clampView()
{
	viewStart_ = std::clamp<qint64>(viewStart_, 0, duration_ - visibleMs());
}

void Timeline::wheelEvent(QWheelEvent *e)
{
	if (duration_ <= 0)
		return;
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
		const double frac = std::clamp(double(x - lp_.pad) / std::max(1, width() - 2 * lp_.pad),
					       0.0, 1.0);
		zoom_ = std::clamp(zoom_ * std::pow(1.3, steps), 1.0, lp_.maxZoom);
		viewStart_ = anchor - qint64(frac * visibleMs());
	} else {
		viewStart_ -= qint64(steps * visibleMs() * 0.15);
	}
	clampView();
	update();
	e->accept();
}

void Timeline::setStart(qint64 ms)
{
	// std::max on the upper bound, not for tidiness: std::clamp with hi < lo is
	// undefined behaviour, and `end_` is 0 until setDuration() has run. Same
	// below for setEnd(), where start_ + 1 can pass duration_.
	start_ = std::clamp<qint64>(ms, 0, std::max<qint64>(0, end_ - 1));
	update();
}

void Timeline::setEnd(qint64 ms)
{
	end_ = std::clamp<qint64>(ms, start_ + 1, std::max<qint64>(start_ + 1, duration_));
	update();
}

void Timeline::setPlayhead(qint64 ms)
{
	playhead_ = std::clamp<qint64>(ms, 0, duration_);
	update();
}

int Timeline::msToX(qint64 ms) const
{
	const int w = width() - 2 * lp_.pad;
	return lp_.pad + int(double(ms - viewStart_) / visibleMs() * w);
}

qint64 Timeline::xToMs(int x) const
{
	const int w = std::max(1, width() - 2 * lp_.pad);
	return std::clamp<qint64>(viewStart_ + qint64(double(x - lp_.pad) / w * visibleMs()), 0,
				  duration_);
}

void Timeline::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	const QRect bar(lp_.pad, lp_.barTop, width() - 2 * lp_.pad, lp_.barH);
	// Background + filmstrip come from the render cache (rebuilt only when the
	// view/zoom/thumbs change) — repaints are a blit, not 20+ image rescales.
	ensureStripCache(bar);
	p.drawPixmap(bar.topLeft(), stripCache_);

	// Overlays live inside the rounded bar.
	{
		QPainterPath clip;
		clip.addRoundedRect(bar, 4, 4);
		p.save();
		p.setClipPath(clip);

		// Selected [start,end] span (translucent, over the filmstrip) and the
		// dimmed outside regions so the kept part reads instantly.
		const int xs = msToX(start_), xe = msToX(end_);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 110));
		p.drawRect(QRect(bar.left(), bar.top(), xs - bar.left(), bar.height()));
		p.drawRect(QRect(xe, bar.top(), bar.right() + 1 - xe, bar.height()));
		p.setBrush(QColor(0x00, 0xae, 0xef, 55));
		p.drawRect(QRect(xs, bar.top(), xe - xs, bar.height()));
		p.restore();
	}
	const int xs = msToX(start_), xe = msToX(end_);

	// Zoom indicator: which part of the clip is visible.
	if (zoom_ > 1.001) {
		const int y = bar.bottom() + 3;
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0x2b, 0x2d, 0x31));
		p.drawRect(QRect(bar.left(), y, bar.width(), 2));
		p.setBrush(QColor(0x9a, 0x9f, 0xa8));
		const int ix = bar.left() + int(double(viewStart_) / duration_ * bar.width());
		const int iw = std::max(8, int(double(visibleMs()) / duration_ * bar.width()));
		p.drawRect(QRect(ix, y, iw, 2));
	}

	// Handles.
	auto handle = [&](int x, const QColor &col) {
		p.setBrush(col);
		p.drawRoundedRect(QRect(x - lp_.handleW / 2, bar.top() - 4, lp_.handleW, bar.height() + 8),
				  2, 2);
	};
	handle(xs, QColor(0x3f, 0xb9, 0x50));
	handle(xe, QColor(0xe5, 0x48, 0x4d));

	// Playhead.
	const int xp = msToX(playhead_);
	p.setPen(QPen(QColor(0xff, 0xff, 0xff), 1));
	p.drawLine(xp, bar.top() - 6, xp, bar.bottom() + 6);

	// Hover marker — the previewed frame's position (no click needed).
	if (hoverMs_ >= 0 && grab_ == Grab::None) {
		const int hx = msToX(hoverMs_);
		p.setPen(QPen(QColor(0xff, 0xff, 0xff, 150), 1));
		p.drawLine(hx, bar.top() + 1, hx, bar.bottom() - 1);
	}

	// Times.
	p.setPen(QColor(0x9a, 0x9f, 0xa8));
	{
		QFont tf = font();
		tf.setPixelSize(std::max(6, lp_.fontPx));
		p.setFont(tf);
	}
	const auto t = timeTextTenths;
	p.drawText(rect().adjusted(lp_.pad, bar.bottom() + 6, -lp_.pad, 0), Qt::AlignLeft,
		   QStringLiteral("Start %1").arg(t(start_)));
	p.drawText(rect().adjusted(lp_.pad, bar.bottom() + 6, -lp_.pad, 0), Qt::AlignRight,
		   QStringLiteral("End %1").arg(t(end_)));
}

void Timeline::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	const int x = e->pos().x();
	const int xs = msToX(start_), xe = msToX(end_);
	if (std::abs(x - xs) <= lp_.handleW)
		grab_ = Grab::Start;
	else if (std::abs(x - xe) <= lp_.handleW)
		grab_ = Grab::End;
	else
		grab_ = Grab::Playhead;
	mouseMoveEvent(e);
}

void Timeline::mouseMoveEvent(QMouseEvent *e)
{
	if (grab_ == Grab::None || !(e->buttons() & Qt::LeftButton)) {
		// Hover: preview the frame under the cursor without clicking. Only the
		// old + new marker columns repaint, not the whole timeline.
		if (e->buttons() == Qt::NoButton && duration_ > 0) {
			const qint64 nh = xToMs(e->pos().x());
			if (nh != hoverMs_) {
				if (hoverMs_ >= 0)
					update(QRect(msToX(hoverMs_) - 2, 0, 5, height()));
				hoverMs_ = nh;
				emit hoverScrub(hoverMs_);
				update(QRect(msToX(hoverMs_) - 2, 0, 5, height()));
			}
		}
		return;
	}
	hoverMs_ = -1; // a drag owns the preview
	const qint64 ms = xToMs(e->pos().x());
	if (grab_ == Grab::Start) {
		start_ = std::clamp<qint64>(ms, 0, std::max<qint64>(0, end_ - 1));
		playhead_ = start_;
		emit startChanged(start_);
		emit scrub(start_);
	} else if (grab_ == Grab::End) {
		end_ = std::clamp<qint64>(ms, start_ + 1, std::max<qint64>(start_ + 1, duration_));
		playhead_ = end_;
		emit endChanged(end_);
		emit scrub(end_);
	} else {
		playhead_ = ms;
		emit scrub(ms);
	}
	update();
}

void Timeline::mouseReleaseEvent(QMouseEvent *)
{
	grab_ = Grab::None;
}

void Timeline::leaveEvent(QEvent *)
{
	if (hoverMs_ >= 0) {
		hoverMs_ = -1;
		update();
	}
}

} // namespace harpia
