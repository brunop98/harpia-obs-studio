#include "ToolbarLayout.hpp"

#include <QWidget>
#include <QWidgetItem>

#include <algorithm>

namespace harpia {

ToolbarLayout::ToolbarLayout(QWidget *parent, int spacing, int zoneGap, int lineSpacing)
	: QLayout(parent), spacing_(spacing), zoneGap_(zoneGap), lineSpacing_(lineSpacing)
{
	setContentsMargins(0, 0, 0, 0);
}

ToolbarLayout::~ToolbarLayout()
{
	while (QLayoutItem *item = takeAt(0))
		delete item;
}

void ToolbarLayout::addWidget(QWidget *w, Zone zone)
{
	addChildWidget(w);
	// Kept in zone order, so iterating entries_ walks Left, Center, Right --
	// and within a zone, in the order the controls were added.
	int at = int(entries_.size());
	for (int i = 0; i < entries_.size(); ++i)
		if (int(entries_[i].zone) > int(zone)) {
			at = i;
			break;
		}
	entries_.insert(at, Entry{new QWidgetItem(w), zone});
	invalidate();
}

void ToolbarLayout::addItem(QLayoutItem *item)
{
	int at = int(entries_.size());
	for (int i = 0; i < entries_.size(); ++i)
		if (entries_[i].zone != Zone::Left) {
			at = i;
			break;
		}
	entries_.insert(at, Entry{item, Zone::Left});
}

int ToolbarLayout::count() const
{
	return int(entries_.size());
}

QLayoutItem *ToolbarLayout::itemAt(int index) const
{
	return index >= 0 && index < entries_.size() ? entries_[index].item : nullptr;
}

QLayoutItem *ToolbarLayout::takeAt(int index)
{
	if (index < 0 || index >= entries_.size())
		return nullptr;
	return entries_.takeAt(index).item;
}

Qt::Orientations ToolbarLayout::expandingDirections() const
{
	return Qt::Horizontal;
}

bool ToolbarLayout::hasHeightForWidth() const
{
	return true;
}

// Hidden -- or a cluster with every control in it hidden, which reports no
// width at all. Either way it takes no room and no spacing.
bool ToolbarLayout::empty(QLayoutItem *item)
{
	return item->isEmpty() || item->sizeHint().width() <= 0;
}

int ToolbarLayout::oneLineWidth() const
{
	int w = 0;
	int n = 0;
	int zones = 0;
	int lastZone = -1;
	for (const Entry &e : entries_) {
		if (empty(e.item))
			continue;
		if (int(e.zone) != lastZone) {
			if (lastZone >= 0)
				w += zoneGap_;
			lastZone = int(e.zone);
			++zones;
			n = 0;
		}
		w += e.item->sizeHint().width() + (n++ ? spacing_ : 0);
	}
	const QMargins m = contentsMargins();
	return w + m.left() + m.right();
}

bool ToolbarLayout::fitsOneLine(int width) const
{
	return oneLineWidth() <= width;
}

int ToolbarLayout::heightForWidth(int width) const
{
	return doLayout(QRect(0, 0, width, 0), true);
}

QSize ToolbarLayout::sizeHint() const
{
	int h = 0;
	for (const Entry &e : entries_)
		if (!empty(e.item))
			h = std::max(h, e.item->sizeHint().height());
	const QMargins m = contentsMargins();
	return QSize(oneLineWidth(), h + m.top() + m.bottom());
}

QSize ToolbarLayout::minimumSize() const
{
	// The widest single item: narrower than that and nothing can wrap into it.
	QSize s;
	for (const Entry &e : entries_)
		if (!empty(e.item))
			s = s.expandedTo(e.item->minimumSize().expandedTo(QSize(e.item->sizeHint().width(), 0)));
	const QMargins m = contentsMargins();
	return s + QSize(m.left() + m.right(), m.top() + m.bottom());
}

void ToolbarLayout::setGeometry(const QRect &rect)
{
	QLayout::setGeometry(rect);
	doLayout(rect, false);
}

int ToolbarLayout::doLayout(const QRect &rect, bool testOnly) const
{
	const QMargins m = contentsMargins();
	const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());

	QVector<const Entry *> shown;
	for (const Entry &e : entries_)
		if (!empty(e.item))
			shown.push_back(&e);
	if (shown.isEmpty())
		return m.top() + m.bottom();

	int lineH = 0;
	for (const Entry *e : shown)
		lineH = std::max(lineH, e->item->sizeHint().height());

	const auto place = [&](const Entry *e, int x, int y, int h) {
		if (testOnly)
			return;
		const QSize s = e->item->sizeHint();
		e->item->setGeometry(QRect(x, y + (h - s.height()) / 2, std::min(s.width(), area.width()), s.height()));
	};
	const auto zoneWidth = [&](Zone z) {
		int w = 0, n = 0;
		for (const Entry *e : shown)
			if (e->zone == z)
				w += e->item->sizeHint().width() + (n++ ? spacing_ : 0);
		return w;
	};

	if (oneLineWidth() <= rect.width()) {
		// One line: Left from the left, Right from the right, Center centred.
		const int lw = zoneWidth(Zone::Left), cw = zoneWidth(Zone::Center), rw = zoneWidth(Zone::Right);
		const int leftEnd = area.x() + lw + (lw ? zoneGap_ : 0);
		const int rightStart = area.right() + 1 - rw - (rw ? zoneGap_ : 0);
		int cx = area.x() + (area.width() - cw) / 2;
		cx = std::max(cx, leftEnd);
		cx = std::min(cx, rightStart - cw);
		int xl = area.x(), xc = cx, xr = area.right() + 1 - rw;
		for (const Entry *e : shown) {
			const int w = e->item->sizeHint().width();
			int &x = e->zone == Zone::Left ? xl : e->zone == Zone::Center ? xc : xr;
			place(e, x, area.y(), lineH);
			x += w + spacing_;
		}
		return lineH + m.top() + m.bottom();
	}

	// Wrapping: in zone order, like words; a zone change is a wider gap.
	int x = area.x(), y = area.y(), curH = 0;
	Zone lastZone = shown.first()->zone;
	bool lineStart = true;
	for (const Entry *e : shown) {
		const QSize s = e->item->sizeHint();
		const int gap = lineStart ? 0 : (e->zone != lastZone ? zoneGap_ : spacing_);
		if (!lineStart && x + gap + s.width() > area.right() + 1) {
			y += curH + lineSpacing_;
			x = area.x();
			curH = 0;
			lineStart = true;
		}
		x += lineStart ? 0 : gap;
		place(e, x, y, s.height());
		x += s.width();
		curH = std::max(curH, s.height());
		lastZone = e->zone;
		lineStart = false;
	}
	return y + curH - rect.y() + m.bottom();
}

} // namespace harpia
