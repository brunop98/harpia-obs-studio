#include "FlowLayout.hpp"

#include <QWidget>

namespace harpia {

FlowLayout::FlowLayout(QWidget *parent, int margin, int hSpacing, int vSpacing)
	: QLayout(parent), hSpace_(hSpacing), vSpace_(vSpacing)
{
	setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout()
{
	QLayoutItem *item;
	while ((item = takeAt(0)))
		delete item;
}

void FlowLayout::addItem(QLayoutItem *item)
{
	items_.append(item);
}

int FlowLayout::horizontalSpacing() const
{
	return hSpace_ >= 0 ? hSpace_ : smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
}

int FlowLayout::verticalSpacing() const
{
	return vSpace_ >= 0 ? vSpace_ : smartSpacing(QStyle::PM_LayoutVerticalSpacing);
}

int FlowLayout::count() const
{
	return items_.size();
}

QLayoutItem *FlowLayout::itemAt(int index) const
{
	return items_.value(index);
}

QLayoutItem *FlowLayout::takeAt(int index)
{
	if (index >= 0 && index < items_.size())
		return items_.takeAt(index);
	return nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const
{
	return {};
}

bool FlowLayout::hasHeightForWidth() const
{
	return true;
}

int FlowLayout::heightForWidth(int width) const
{
	return doLayout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect &rect)
{
	QLayout::setGeometry(rect);
	doLayout(rect, false);
}

QSize FlowLayout::sizeHint() const
{
	return minimumSize();
}

QSize FlowLayout::minimumSize() const
{
	QSize size;
	for (QLayoutItem *item : items_)
		size = size.expandedTo(item->minimumSize());
	const QMargins m = contentsMargins();
	size += QSize(m.left() + m.right(), m.top() + m.bottom());
	return size;
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const
{
	int left, top, right, bottom;
	getContentsMargins(&left, &top, &right, &bottom);
	const QRect effective = rect.adjusted(left, top, -right, -bottom);
	int x = effective.x();
	int y = effective.y();
	int lineHeight = 0;

	for (QLayoutItem *item : items_) {
		const QSize sz = item->sizeHint();
		int nextX = x + sz.width() + horizontalSpacing();
		if (nextX - horizontalSpacing() > effective.right() + 1 && lineHeight > 0) {
			x = effective.x();
			y = y + lineHeight + verticalSpacing();
			nextX = x + sz.width() + horizontalSpacing();
			lineHeight = 0;
		}
		if (!testOnly)
			item->setGeometry(QRect(QPoint(x, y), sz));
		x = nextX;
		lineHeight = qMax(lineHeight, sz.height());
	}
	return y + lineHeight - rect.y() + bottom;
}

int FlowLayout::smartSpacing(QStyle::PixelMetric pm) const
{
	QObject *parent = this->parent();
	if (!parent)
		return -1;
	if (parent->isWidgetType()) {
		QWidget *pw = static_cast<QWidget *>(parent);
		return pw->style()->pixelMetric(pm, nullptr, pw);
	}
	return static_cast<QLayout *>(parent)->spacing();
}

} // namespace harpia
