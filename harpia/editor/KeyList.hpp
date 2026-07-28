#pragma once

// A list of keyframe times, with the four things you need to do to them.
//
// The project has three kinds of keyframe: a clip's transform (TlKeyframe), an
// effect's parameters (FxKey) and a spotlight mask's pose (SpotKey). Only the
// first had any UI. The other two could be CREATED — dragging a keyframed mask
// writes one — and then never seen again: no way to find them, retime them or
// take one back out.
//
// This is the smallest thing that closes that: the times, in order, with the
// one at the playhead highlighted, and Add / Remove / Prev / Next. It is not the
// curve editor KeyframeEditor gives a clip's transform, and does not pretend to
// be; it is the difference between "invisible" and "editable".

#include <QVector>
#include <QWidget>

class QListWidget;
class QPushButton;

namespace harpia {

class KeyList : public QWidget {
	Q_OBJECT
public:
	// `what` names the thing being animated, for the empty-state line — e.g.
	// "this effect's settings".
	explicit KeyList(const QString &what, QWidget *parent = nullptr);

	// Times in ms, and where the playhead is so the current key can be marked.
	// Not required to be sorted; the display sorts.
	void setTimes(const QVector<qint64> &ms, qint64 playheadMs);

signals:
	// Add a key at the playhead — or overwrite the one already there.
	void addRequested();
	// Remove the key at this time.
	void removeRequested(qint64 ms);
	// Move the playhead here (double-click, or Prev/Next).
	void jumpRequested(qint64 ms);

private:
	void refreshButtons();

	QListWidget *list_ = nullptr;
	QPushButton *add_ = nullptr;
	QPushButton *del_ = nullptr;
	QPushButton *prev_ = nullptr;
	QPushButton *next_ = nullptr;
	QVector<qint64> times_; // sorted
	qint64 playhead_ = 0;
};

} // namespace harpia
