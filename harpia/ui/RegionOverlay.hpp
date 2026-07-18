#pragma once

#include "core/CaptureManager.hpp"

#include <QDialog>
#include <QRect>
#include <QWidget>

class QScreen;

namespace harpia {

// Fullscreen drag-to-select picker that covers a specific screen. Exec it; on
// Accepted, region() holds the chosen rectangle in device pixels relative to
// that screen's top-left — matching the capture source (monitor) coordinate
// space used by crop_filter. The drag/rubber-band interaction is the desktop-
// region analogue of the crop handles in frontend/widgets/OBSBasicPreview.cpp.
class RegionSelectDialog : public QDialog {
	Q_OBJECT
public:
	// `screen` is the display to select on (defaults to the primary screen).
	explicit RegionSelectDialog(QScreen *screen = nullptr, QWidget *parent = nullptr);

	// Selected region in device pixels (enabled == false if nothing picked).
	CaptureRegion region() const { return region_; }

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;

private:
	QRect selectionRectLogical() const;

	QPoint origin_;
	QPoint current_;
	bool selecting_ = false;
	qreal dpr_ = 1.0;
	CaptureRegion region_;
};

// A frameless, click-through, always-on-top window that outlines the region
// currently being recorded so the user can verify the capture area at a glance.
// setRegion() takes device-pixel coordinates (as produced by RegionSelectDialog)
// and positions itself accordingly.
class RegionOverlay : public QWidget {
	Q_OBJECT
public:
	explicit RegionOverlay(QWidget *parent = nullptr);

	// Show the outline for `region` (device pixels, relative to `screen`'s
	// top-left). A disabled region — or a null screen — hides it.
	void setRegion(const CaptureRegion &region, QScreen *screen);

protected:
	void paintEvent(QPaintEvent *event) override;
};

} // namespace harpia
