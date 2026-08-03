#include "SpotlightPreview.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace harpia {

SpotlightPreview::SpotlightPreview(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(150);
	setMouseTracking(true); // the patch follows the mouse without a button held
	setCursor(Qt::CrossCursor);
	setToolTip(QStringLiteral("Move the mouse over this to see the spotlight follow it."));
}

void SpotlightPreview::configure(const SpotlightParams &params, int screenWidthPx)
{
	params_ = params;
	screenWidthPx_ = screenWidthPx > 0 ? screenWidthPx : 1920;
	update();
}

QPoint SpotlightPreview::restingPoint() const
{
	// Left of centre and a little high — over the mock window's title bar and
	// text, where there is something for the light to fall on.
	return QPoint(width() * 2 / 5, height() * 2 / 5);
}

void SpotlightPreview::resizeEvent(QResizeEvent *)
{
	if (!hovering_)
		cursor_ = restingPoint();
}

void SpotlightPreview::mouseMoveEvent(QMouseEvent *e)
{
	hovering_ = true;
	cursor_ = e->pos();
	update();
}

void SpotlightPreview::leaveEvent(QEvent *)
{
	hovering_ = false;
	cursor_ = restingPoint();
	update();
}

void SpotlightPreview::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);
	const QRect r = rect();
	if (r.isEmpty())
		return;
	if (cursor_.isNull())
		cursor_ = restingPoint();

	// ---- A mock desktop, so there is something for the light to fall ON. A
	// spotlight over a flat grey rectangle looks identical at every setting.
	p.fillRect(r, QColor(0x1e, 0x22, 0x2b));
	// A few window-ish shapes and a strip of "text", drawn once and always the
	// same, so moving a slider changes only the light and not the scenery.
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x2b, 0x31, 0x3c));
	p.drawRoundedRect(QRect(r.width() / 12, r.height() / 6, r.width() * 3 / 5, r.height() * 3 / 5),
			  6, 6);
	p.setBrush(QColor(0x39, 0x40, 0x4d));
	p.drawRoundedRect(QRect(r.width() / 12, r.height() / 6, r.width() * 3 / 5, r.height() / 9), 6, 6);
	p.setBrush(QColor(0x53, 0x5b, 0x6b));
	for (int i = 0; i < 5; ++i) {
		const int y = r.height() / 6 + r.height() / 5 + i * r.height() / 11;
		const int w = (i % 2 == 0) ? r.width() * 2 / 5 : r.width() / 3;
		p.drawRoundedRect(QRect(r.width() / 9, y, w, std::max(3, r.height() / 30)), 2, 2);
	}
	p.setBrush(QColor(0x2b, 0x31, 0x3c));
	p.drawRoundedRect(QRect(r.width() * 7 / 10, r.height() / 3, r.width() / 4, r.height() / 2), 6, 6);

	// ---- The spotlight, through the same maths the recording uses.
	// The patch is configured in SCREEN pixels; show it at the same fraction of
	// this widget's width that it would take up on the display, so "320 px on a
	// 4K screen" looks as small here as it will there.
	const double scale = double(r.width()) / double(screenWidthPx_);
	const int sizeHere = std::max(8, int(std::lround(params_.sizePx * scale)));
	// spotlightHole clamps to the recording's size limits, which are in screen
	// pixels and far too coarse at preview scale, so the rectangle is built
	// here and only the RADIUS and the ALPHA come from the shared functions.
	const QRect hole(cursor_.x() - sizeHere / 2, cursor_.y() - sizeHere / 2, sizeHere, sizeHere);
	const int radius = spotlightRadius(hole, params_.roundnessPct);
	const int alpha = spotlightAlpha(params_.darkPct, 1.0);

	QPainterPath dark;
	dark.addRect(r);
	QPainterPath lit;
	if (radius > 0)
		lit.addRoundedRect(hole, radius, radius);
	else
		lit.addRect(hole);
	p.setPen(Qt::NoPen);
	p.fillPath(dark.subtracted(lit), QColor(0, 0, 0, alpha));

	// A cursor arrow at the centre of the patch: without it the patch reads as
	// a shape someone placed rather than as something tracking the pointer.
	QPainterPath arrow;
	arrow.moveTo(cursor_);
	arrow.lineTo(cursor_ + QPoint(0, 14));
	arrow.lineTo(cursor_ + QPoint(4, 10));
	arrow.lineTo(cursor_ + QPoint(7, 16));
	arrow.lineTo(cursor_ + QPoint(9, 15));
	arrow.lineTo(cursor_ + QPoint(6, 9));
	arrow.lineTo(cursor_ + QPoint(11, 9));
	arrow.closeSubpath();
	p.setPen(QPen(QColor(0x11, 0x14, 0x1a), 1));
	p.setBrush(Qt::white);
	p.drawPath(arrow);

	// A hairline frame, so the preview reads as a screen rather than as a hole
	// in the dialog.
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(QColor(0x3a, 0x40, 0x4b), 1));
	p.drawRect(r.adjusted(0, 0, -1, -1));
}

} // namespace harpia
