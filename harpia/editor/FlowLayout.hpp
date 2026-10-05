#pragma once

// A layout that wraps: items left to right, onto the next line when the row is
// full -- like words in a paragraph.
//
// For rows that are wider than the space they sometimes get: the Inspector's
// tag buttons and style check boxes, and the editor's toolbar. A QHBoxLayout
// in a narrow panel can only squeeze its buttons until their labels are cut
// ("le ti Ed") or push the panel into a horizontal scroll; this one keeps
// every control whole and adds a line instead. Hidden widgets take no room.
//
// After Qt's own Flow Layout example, with height-for-width so a parent
// layout gives it the lines it needs.

#include <QLayout>
#include <QVector>

namespace harpia {

class FlowLayout : public QLayout {
public:
	explicit FlowLayout(QWidget *parent = nullptr, int hSpacing = 4, int vSpacing = 4);
	~FlowLayout() override;

	void addItem(QLayoutItem *item) override;
	int count() const override;
	QLayoutItem *itemAt(int index) const override;
	QLayoutItem *takeAt(int index) override;

	Qt::Orientations expandingDirections() const override;
	bool hasHeightForWidth() const override;
	int heightForWidth(int width) const override;
	QSize minimumSize() const override;
	QSize sizeHint() const override;
	void setGeometry(const QRect &rect) override;

private:
	int doLayout(const QRect &rect, bool testOnly) const;

	QVector<QLayoutItem *> items_;
	int hSpace_ = 4;
	int vSpace_ = 4;
};

} // namespace harpia
