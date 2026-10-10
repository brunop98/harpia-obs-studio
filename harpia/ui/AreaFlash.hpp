#pragma once

// Outlines an area of the desktop for a moment: "this is what will be
// recorded". Used by the remote control's /area/show, so the Unity package can
// check that the Game view it measured is the rectangle Harpia sees -- the
// two count pixels differently on some setups (display scaling).
//
// One see-through, click-through layer per screen the area touches, drawn in
// that screen's own coordinates and gone after a moment.

#include <QRect>
#include <QRectF>
#include <QString>
#include <QVector>

class QScreen;
class QWidget;

namespace harpia {

class AreaFlash {
public:
	struct ScreenArea {
		QScreen *screen = nullptr;
		QRect physical; // the screen in physical desktop px; null = its logical geometry, scaled
	};

	// Shows the outline (and `caption`) for `ms`. Returns the layers, which
	// delete themselves when the time is up.
	static QVector<QWidget *> show(const QRect &areaPx, const QVector<ScreenArea> &screens, int ms,
				       const QString &caption);

	// The area in a screen's local logical coordinates.
	static QRectF toLocal(const QRect &areaPx, const QRect &screenPhysical, double dpr);
};

} // namespace harpia
