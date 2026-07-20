#pragma once

#include <QLayout>
#include <QRect>
#include <QStyle>
#include <QVector>

namespace harpia {

// A layout that arranges child widgets left-to-right and wraps them onto the
// next line when they don't fit — so rows of cards reflow gracefully as the
// window is resized instead of overlapping or clipping. (Modeled on Qt's
// canonical FlowLayout example.)
class FlowLayout : public QLayout {
public:
	explicit FlowLayout(QWidget *parent = nullptr, int margin = 0, int hSpacing = 12, int vSpacing = 12);
	~FlowLayout() override;

	void addItem(QLayoutItem *item) override;
	int horizontalSpacing() const;
	int verticalSpacing() const;
	Qt::Orientations expandingDirections() const override;
	bool hasHeightForWidth() const override;
	int heightForWidth(int width) const override;
	int count() const override;
	QLayoutItem *itemAt(int index) const override;
	QSize minimumSize() const override;
	void setGeometry(const QRect &rect) override;
	QSize sizeHint() const override;
	QLayoutItem *takeAt(int index) override;

private:
	int doLayout(const QRect &rect, bool testOnly) const;
	int smartSpacing(QStyle::PixelMetric pm) const;

	QVector<QLayoutItem *> items_;
	int hSpace_;
	int vSpace_;
};

} // namespace harpia
