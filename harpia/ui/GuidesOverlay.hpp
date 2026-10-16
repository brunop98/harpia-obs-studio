#pragma once

#include "core/CompositionGuides.hpp"

#include <QColor>
#include <QRect>
#include <QWidget>

class QPainter;

namespace harpia {

// How the guides look: which ones, and the line they are drawn with.
struct GuideStyle {
	int kinds = GuideThirds;
	QColor color = QColor(255, 255, 255);
	int opacityPct = 60; // 10..100
	int thickness = 1;   // px, 1..5

	bool operator==(const GuideStyle &o) const
	{
		return kinds == o.kinds && color == o.color && opacityPct == o.opacityPct &&
		       thickness == o.thickness;
	}
	bool operator!=(const GuideStyle &o) const { return !(*this == o); }
};

// The guides' lines for one frame, in device pixels, ready to stroke. Building
// them (the spiral's arcs above all) is the costly half of drawing them, and
// they depend only on the frame's size, the kinds, the thickness and the
// display -- not on the colour -- so the overlay keeps them between paints.
struct GuideGeometry {
	GuidePaths paths;  // device pixels
	int lineWidth = 1; // device pixels
	double unit = 1.0; // device pixels per painter unit

	bool isEmpty() const { return paths.solid.isEmpty() && paths.dashed.isEmpty(); }
};
GuideGeometry guideGeometry(const QRectF &frame, const GuideStyle &style, double pxPerUnit = 1.0);
// Stroke `g` in the style's colour and opacity. See paintGuides.
void strokeGuides(QPainter &p, const GuideGeometry &g, const GuideStyle &style);

// Draw the guides inside `frame`, in the painter's current units. `pxPerUnit`
// is how many device pixels one of those units is, so the line comes out as
// thick as the style says on any display. Each line gets a faint edge in the
// opposite shade, so a white line still reads over snow and a black one over
// night. Writes its pixels outright (Source) rather than blending them, so
// paint it onto something transparent: the overlay, or a layer.
void paintGuides(QPainter &p, const QRectF &frame, const GuideStyle &style, double pxPerUnit = 1.0);

// The guides over the part of the desktop being recorded.
//
// A frameless, click-through, always-on-top window exactly the size of the
// recorded area -- the whole monitor, the region, or the zoomed-in part of
// either. On Windows 10 version 2004 and newer it is excluded from screen
// capture (WDA_EXCLUDEFROMCAPTURE), like the region frame and the recording
// border: on your screen, never in the file. Elsewhere hiddenFromCapture() is
// false and the owner hides it while recording (see guidesVisible).
class GuidesOverlay : public QWidget {
public:
	explicit GuidesOverlay(QWidget *parent = nullptr);

	// Cover `rectLogical` (Qt's logical desktop coordinates, the space
	// QScreen::geometry() lives in). A no-op when nothing changed, because the
	// zoom calls it on every frame of its animation.
	void showGuides(const QRect &rectLogical, const GuideStyle &style);
	void hideGuides();

	QRect shownRect() const { return rect_; }
	const GuideStyle &style() const { return style_; }

	// Whether the window is kept out of screen capture. False until proven
	// otherwise where the system cannot do it, and false if asking failed.
	bool hiddenFromCapture() const { return hidden_; }
	// Whether this system can keep a window out of capture at all.
	static bool systemCanHideFromCapture();

protected:
	void paintEvent(QPaintEvent *) override;

private:
	void excludeFromCapture();

	GuideStyle style_;
	QRect rect_;
	bool hidden_ = false;
	// The lines last drawn, and what they were built for: a moved region or
	// a new colour repaints without rebuilding them.
	GuideGeometry geometry_;
	QSize geometrySize_;
	double geometryDpr_ = 0.0;
	int geometryKinds_ = -1;
	int geometryThickness_ = -1;
};

// The guides over a small 16:9 picture: what the settings page shows, so the
// spiral and the safe boxes can be seen before switching them on.
class GuidesPreview : public QWidget {
public:
	explicit GuidesPreview(QWidget *parent = nullptr);
	void setStyle(const GuideStyle &style);
	QSize sizeHint() const override { return QSize(192, 108); }

protected:
	void paintEvent(QPaintEvent *) override;

private:
	GuideStyle style_;
};

} // namespace harpia
