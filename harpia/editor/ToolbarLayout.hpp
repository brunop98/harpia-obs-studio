#pragma once

// The editor's one toolbar: three zones on a line when the window is wide
// enough, wrapping onto more lines when it is not.
//
//   [ editing tools ........ ]   [ transport · speed ]   [ ...... preview / audio · info ]
//         Left zone                 Center zone                   Right zone
//
// Wide: Left packs from the left edge, Right from the right edge, Center sits
// in the middle of the window (nudged aside if it would touch either). This
// is where the eye expects play/pause: under the middle of the picture.
//
// Narrow: everything flows left to right in zone order and wraps like words,
// each item kept whole. A QHBoxLayout in the same space squeezed the buttons
// until their labels were cut to "le ti Ed" and "Previ"; this adds a line.
//
// Items are widgets (a cluster of controls that belong together -- the mode
// switch, undo/redo, the transport -- is one widget so it wraps as a unit).
// A hidden widget, or a cluster whose controls are all hidden, takes no room.

#include <QLayout>
#include <QVector>

namespace harpia {

class ToolbarLayout : public QLayout {
public:
	enum class Zone { Left, Center, Right };

	explicit ToolbarLayout(QWidget *parent = nullptr, int spacing = 6, int zoneGap = 18, int lineSpacing = 4);
	~ToolbarLayout() override;

	void addWidget(QWidget *w, Zone zone);

	// QLayout
	void addItem(QLayoutItem *item) override; // goes to the Left zone
	int count() const override;
	QLayoutItem *itemAt(int index) const override;
	QLayoutItem *takeAt(int index) override;
	Qt::Orientations expandingDirections() const override;
	bool hasHeightForWidth() const override;
	int heightForWidth(int width) const override;
	QSize minimumSize() const override;
	QSize sizeHint() const override;
	void setGeometry(const QRect &rect) override;

	// For tests: would everything fit on one line at this width?
	bool fitsOneLine(int width) const;

private:
	struct Entry {
		QLayoutItem *item = nullptr;
		Zone zone = Zone::Left;
	};
	static bool empty(QLayoutItem *item);
	int oneLineWidth() const;
	int doLayout(const QRect &rect, bool testOnly) const;

	QVector<Entry> entries_;
	int spacing_ = 6;
	int zoneGap_ = 18;
	int lineSpacing_ = 4;
};

} // namespace harpia
