#include "FlowLayout.hpp"

#include <QWidget>

#include <algorithm>

namespace harpia {

FlowLayout::FlowLayout(QWidget *parent, int hSpacing, int vSpacing)
	: QLayout(parent), hSpace_(hSpacing), vSpace_(vSpacing)
{
	setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
	while (QLayoutItem *item = takeAt(0))
		delete item;
}

void FlowLayout::addItem(QLayoutItem *item)
{
	items_.append(item);
}

int FlowLayout::count() const
{
	return int(items_.size());
}

QLayoutItem *FlowLayout::itemAt(int index) const
{
	return index >= 0 && index < items_.size() ? items_[index] : nullptr;
}

QLayoutItem *FlowLayout::takeAt(int index)
{
	return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
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

QSize FlowLayout::sizeHint() const
{
	// One line: what it would take to show everything side by side.
	int w = 0, h = 0, n = 0;
	for (QLayoutItem *item : items_) {
		if (item->isEmpty())
			continue;
		const QSize s = item->sizeHint();
		w += s.width() + (n++ ? hSpace_ : 0);
		h = std::max(h, s.height());
	}
	const QMargins m = contentsMargins();
	return QSize(w + m.left() + m.right(), h + m.top() + m.bottom());
}

QSize FlowLayout::minimumSize() const
{
	// The widest single item: anything narrower than that cannot be wrapped
	// into, so it is the floor -- and the reason the row never forces its
	// parent wider than one control.
	QSize size;
	for (QLayoutItem *item : items_)
		if (!item->isEmpty())
			size = size.expandedTo(item->minimumSize());
	const QMargins m = contentsMargins();
	return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

void FlowLayout::setGeometry(const QRect &rect)
{
	QLayout::setGeometry(rect);
	doLayout(rect, false);
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const
{
	const QMargins m = contentsMargins();
	const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
	int x = area.x();
	int y = area.y();
	int lineHeight = 0;
	for (QLayoutItem *item : items_) {
		if (item->isEmpty())
			continue;
		const QSize s = item->sizeHint();
		int nextX = x + s.width() + hSpace_;
		if (nextX - hSpace_ > area.right() + 1 && lineHeight > 0) {
			x = area.x();
			y += lineHeight + vSpace_;
			nextX = x + s.width() + hSpace_;
			lineHeight = 0;
		}
		if (!testOnly)
			item->setGeometry(QRect(QPoint(x, y), QSize(std::min(s.width(), area.width()), s.height())));
		x = nextX;
		lineHeight = std::max(lineHeight, s.height());
	}
	return y + lineHeight - rect.y() + m.bottom();
}

} // namespace harpia
