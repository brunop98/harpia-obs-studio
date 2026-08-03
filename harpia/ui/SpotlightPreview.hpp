#pragma once

#include "core/SpotlightFx.hpp"

#include <QPoint>
#include <QWidget>

namespace harpia {

// A live preview of the spotlight for the preset editor: a mock desktop with
// the patch cut out of it, following the mouse while it is over the widget.
//
// It draws with the same functions the recording overlay does
// (spotlightHole / spotlightRadius / spotlightAlpha), so what is being tuned
// here and what lands in the file cannot drift apart -- which is the whole
// point of a preview. Sizes are scaled: the patch is set in screen pixels and
// this widget is a few hundred across, so it is shown at the same FRACTION of
// the preview's width that it would occupy on the screen being recorded.
class SpotlightPreview : public QWidget {
	Q_OBJECT
public:
	explicit SpotlightPreview(QWidget *parent = nullptr);

	// screenWidthPx: the display the preset records, so the patch can be shown
	// at its true relative size. Falls back to 1920 when unknown.
	void configure(const SpotlightParams &params, int screenWidthPx);

protected:
	void paintEvent(QPaintEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void leaveEvent(QEvent *) override;
	void resizeEvent(QResizeEvent *) override;

private:
	// Where the patch sits when the mouse is elsewhere: over the mock window,
	// so the preview shows the effect rather than an empty circle.
	QPoint restingPoint() const;

	SpotlightParams params_;
	int screenWidthPx_ = 1920;
	QPoint cursor_;
	bool hovering_ = false;
};

} // namespace harpia
