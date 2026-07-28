#include "KeyList.hpp"

#include "TimeText.hpp"
#include "../ui/UiIcons.hpp"

#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace harpia {

namespace {
constexpr int kAtPlayheadMs = 1; // "the key here" means within a millisecond
}

KeyList::KeyList(const QString &what, QWidget *parent) : QWidget(parent)
{
	auto *v = new QVBoxLayout(this);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(4);

	list_ = new QListWidget(this);
	list_->setMaximumHeight(84);
	list_->setToolTip(QStringLiteral("Double-click a time to move the playhead there."));
	v->addWidget(list_);

	auto *row = new QHBoxLayout;
	row->setSpacing(4);
	add_ = new QPushButton(QStringLiteral("Add key"), this);
	add_->setToolTip(QStringLiteral("Record %1 at the playhead. A key already there "
					"is overwritten.")
				 .arg(what));
	del_ = new QPushButton(this);
	del_->setIcon(uiIcon(Glyph::Cross, 12));
	del_->setToolTip(QStringLiteral("Remove the selected key"));
	prev_ = new QPushButton(this);
	prev_->setIcon(uiIcon(Glyph::StepBack, 12));
	prev_->setToolTip(QStringLiteral("Go to the previous key"));
	next_ = new QPushButton(this);
	next_->setIcon(uiIcon(Glyph::StepForward, 12));
	next_->setToolTip(QStringLiteral("Go to the next key"));
	row->addWidget(add_, 1);
	row->addWidget(prev_);
	row->addWidget(next_);
	row->addWidget(del_);
	v->addLayout(row);

	connect(add_, &QPushButton::clicked, this, &KeyList::addRequested);
	connect(del_, &QPushButton::clicked, this, [this]() {
		const int r = list_->currentRow();
		if (r >= 0 && r < times_.size())
			emit removeRequested(times_[r]);
	});
	connect(list_, &QListWidget::itemDoubleClicked, this, [this]() {
		const int r = list_->currentRow();
		if (r >= 0 && r < times_.size())
			emit jumpRequested(times_[r]);
	});
	connect(list_, &QListWidget::currentRowChanged, this, [this](int) { refreshButtons(); });
	connect(prev_, &QPushButton::clicked, this, [this]() {
		// Strictly before the playhead, so repeated presses walk backwards
		// instead of sticking on the key you are already sitting on.
		for (int i = times_.size() - 1; i >= 0; --i)
			if (times_[i] < playhead_ - kAtPlayheadMs) {
				emit jumpRequested(times_[i]);
				return;
			}
	});
	connect(next_, &QPushButton::clicked, this, [this]() {
		for (const qint64 t : times_)
			if (t > playhead_ + kAtPlayheadMs) {
				emit jumpRequested(t);
				return;
			}
	});
}

void KeyList::setTimes(const QVector<qint64> &ms, qint64 playheadMs)
{
	// Keep the selection on the same TIME across a refresh, not the same row:
	// adding an earlier key shifts every row down. Read it BEFORE times_ is
	// replaced — indexing the new list with the old row is how this went wrong
	// the first time, and it looks right until a key is added above.
	const int was = list_->currentRow();
	const qint64 wasTime = (was >= 0 && was < times_.size()) ? times_[was] : -1;

	times_ = ms;
	std::sort(times_.begin(), times_.end());
	playhead_ = playheadMs;

	const QSignalBlocker b(list_);
	list_->clear();
	for (int i = 0; i < times_.size(); ++i) {
		const bool here = std::abs(times_[i] - playhead_) <= kAtPlayheadMs;
		// ASCII for the marker, deliberately: a dingbat here would be the
		// same font-coverage trap the button icons were just moved off.
		list_->addItem(QStringLiteral("%1 %2")
				       .arg(here ? QStringLiteral(">") : QStringLiteral(" "))
				       .arg(timeTextMs(times_[i])));
		if (times_[i] == wasTime)
			list_->setCurrentRow(i);
	}
	if (times_.isEmpty())
		list_->addItem(QStringLiteral("Not animated — press Add key to start."));
	refreshButtons();
}

void KeyList::refreshButtons()
{
	const bool any = !times_.isEmpty();
	const int r = list_->currentRow();
	del_->setEnabled(any && r >= 0 && r < times_.size());
	prev_->setEnabled(any && times_.first() < playhead_ - kAtPlayheadMs);
	next_->setEnabled(any && times_.last() > playhead_ + kAtPlayheadMs);
	// "Add" always works: on a time that already has a key it overwrites it,
	// which is what you want after nudging a value.
}

} // namespace harpia
