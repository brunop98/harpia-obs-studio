#pragma once

// Multi-Area: several same-size spots on one screen, and the recording cuts
// (or pans) to whichever one the mouse is in.
//
// For tutorials that move between a few fixed places -- a toolbar, a canvas,
// a properties panel -- without the frame swimming the way Follow Mouse does.
// Each area is framed once, on purpose, and the recording only ever shows one
// of those framings.
//
// Pure logic, like FollowMouse: MainWindow cannot be built without libobs and
// a display, so the hit test, the hover delay and the pan arithmetic live
// here and multiarea_test pins them. MainWindow's tick is plumbing: read the
// cursor, call tick(), push region() to the crop filter and the frame.
//
// Rules:
//   * Every area is the SAME size -- the encoder is committed to one frame
//     size per file. Only the top-left differs.
//   * The cursor in no area: stay where we are. Gaps between areas are for
//     travelling through, not a reason to change the shot.
//   * The cursor in another area switches only after it has STAYED there for
//     hoverMs, so crossing an area on the way to a third does not flash it.
//   * Overlapping areas: the current one wins while the cursor is in it;
//     otherwise the lowest-numbered one containing the cursor.
//   * Cut: jump. Pan: ease-in-out from wherever the frame is now over panMs;
//     a new switch mid-pan starts from the in-between position, never snaps.
//
// Coordinates: device pixels relative to the screen's top-left -- the space
// CaptureRegion and crop_filter use (see RegionWatch::toRegionSpace).

#include "CaptureManager.hpp" // CaptureRegion

#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace harpia {

enum class AreaTransition {
	Cut = 0,
	Pan = 1,
};

struct MultiAreaParams {
	AreaTransition transition = AreaTransition::Cut;
	int panMs = 400;   // pan length, 50..3000
	int hoverMs = 300; // how long the cursor stays before switching, 0..2000
};

// Most areas a layout can hold. Plenty for a screen, and it keeps the overlay's
// numbers to one digit.
inline constexpr int kMaxAreas = 9;

// An area in a layout that may span monitors: which (OBS) monitor, and its
// top-left in that monitor's device pixels. Layout order is area numbering.
struct AreaRef {
	int monitor = 0;
	QPoint top;
	bool operator==(const AreaRef &o) const { return monitor == o.monitor && top == o.top; }
};

// Arranging happens on one overlay per screen, each editing only its own
// areas; these fold that back into the one numbered layout.
namespace area_layout {

// Monitor `m`'s areas, in layout order.
inline QVector<QPoint> topsOn(const QVector<AreaRef> &layout, int m)
{
	QVector<QPoint> out;
	for (const AreaRef &a : layout)
		if (a.monitor == m)
			out.push_back(a.top);
	return out;
}

// Their numbers as shown (1-based, global).
inline QVector<int> numbersOn(const QVector<AreaRef> &layout, int m)
{
	QVector<int> out;
	for (int i = 0; i < layout.size(); ++i)
		if (layout[i].monitor == m)
			out.push_back(i + 1);
	return out;
}

// Monitor `m`'s share replaced by `tops` -- that screen's areas after an edit,
// in its own order. Its existing slots are refilled in order (so a move keeps
// the area's number), surplus slots are dropped (a removal; later areas close
// up), and extra tops are appended at the end (a new area gets the next
// number). Other monitors' areas keep their places.
inline QVector<AreaRef> mergeEdit(const QVector<AreaRef> &layout, int m, const QVector<QPoint> &tops)
{
	QVector<AreaRef> out;
	int k = 0;
	for (const AreaRef &a : layout) {
		if (a.monitor != m) {
			out.push_back(a);
		} else if (k < tops.size()) {
			out.push_back({m, tops[k++]});
		}
	}
	for (; k < tops.size(); ++k)
		out.push_back({m, tops[k]});
	return out;
}

inline QVector<QPoint> tops(const QVector<AreaRef> &layout)
{
	QVector<QPoint> out;
	for (const AreaRef &a : layout)
		out.push_back(a.top);
	return out;
}

inline QVector<int> monitors(const QVector<AreaRef> &layout)
{
	QVector<int> out;
	for (const AreaRef &a : layout)
		out.push_back(a.monitor);
	return out;
}

} // namespace area_layout

class AreaSwitcher {
public:
	// Which area contains `p` (half-open on the far edges, like the crop).
	// `prefer` wins when it contains the point too; -1 when none does.
	static int areaAt(const QVector<QPoint> &tops, QSize size, QPoint p, int prefer = -1)
	{
		return areaAtOn(tops, {}, size, 0, p, prefer);
	}

	// The same across monitors. `monitors[i]` is area i's monitor and `p` is
	// in `monitor`'s device pixels -- only areas on that monitor can contain it.
	// An empty `monitors` means everything is on one screen.
	static int areaAtOn(const QVector<QPoint> &tops, const QVector<int> &monitors, QSize size, int monitor,
			    QPoint p, int prefer = -1)
	{
		auto inside = [&](int i) {
			if (!monitors.isEmpty() && (i >= monitors.size() || monitors[i] != monitor))
				return false;
			const QPoint t = tops[i];
			return p.x() >= t.x() && p.y() >= t.y() && p.x() < t.x() + size.width() &&
			       p.y() < t.y() + size.height();
		};
		if (prefer >= 0 && prefer < tops.size() && inside(prefer))
			return prefer;
		for (int i = 0; i < tops.size(); ++i)
			if (inside(i))
				return i;
		return -1;
	}

	// Where a recording starts: the area under the cursor, else area 1.
	static int startIndex(const QVector<QPoint> &tops, QSize size, QPoint cursor,
			      const QVector<int> &monitors = {}, int cursorMonitor = 0)
	{
		const int i = areaAtOn(tops, monitors, size, cursorMonitor, cursor);
		return i >= 0 ? i : 0;
	}

	// An area's top-left kept on the screen (the crop cannot leave it).
	static QPoint clampTop(QPoint t, QSize size, QSize screen)
	{
		if (screen.isEmpty())
			return t;
		return QPoint(std::clamp(t.x(), 0, std::max(0, screen.width() - size.width())),
			      std::clamp(t.y(), 0, std::max(0, screen.height() - size.height())));
	}

	// Smoothstep: eases in and out, 0 -> 0, 1 -> 1.
	static double ease(double u)
	{
		u = std::clamp(u, 0.0, 1.0);
		return u * u * (3.0 - 2.0 * u);
	}

	// Start switching between `tops` (area 1 first), every one `size` big,
	// showing area `start`.
	// `monitors` (optional, one per area) spreads the areas over several
	// screens; a switch to another screen is always a cut -- there is nothing
	// to pan across between two monitors.
	void arm(const QVector<QPoint> &tops, QSize size, int start, bool enabled = true,
		 const QVector<int> &monitors = {})
	{
		tops_ = tops;
		monitors_ = monitors.size() == tops.size() ? monitors : QVector<int>();
		size_ = size;
		enabled_ = enabled;
		armed_ = !tops_.isEmpty() && !size_.isEmpty();
		current_ = armed_ ? std::clamp(start, 0, int(tops_.size()) - 1) : -1;
		pos_ = armed_ ? QPointF(tops_[current_]) : QPointF();
		from_ = pos_;
		panStart_ = -1;
		candidate_ = -1;
		candidateSince_ = -1;
		switched_ = false;
	}
	void disarm() { armed_ = false; }

	// The user moved the frame by hand: the area being shown is now there
	// (for the rest of this take). Cancels a pan in progress.
	void rebaseCurrent(QPoint top)
	{
		if (!armed_ || current_ < 0)
			return;
		tops_[current_] = top;
		pos_ = QPointF(top);
		panStart_ = -1;
	}
	bool armed() const { return armed_; }

	int current() const { return current_; }
	int count() const { return tops_.size(); }
	const QVector<QPoint> &tops() const { return tops_; }
	const QVector<int> &monitors() const { return monitors_; }
	int monitorOf(int i) const { return (i >= 0 && i < monitors_.size()) ? monitors_[i] : 0; }
	int currentMonitor() const { return monitorOf(current_); }
	// True for the one tick() call on which the shown area changed -- the
	// moment to drop a marker.
	bool switchedThisTick() const { return switched_; }
	bool panning() const { return panStart_ >= 0; }

	// Jump (or pan) to area `i` now, skipping the hover delay.
	void switchTo(int i, qint64 nowMs, const MultiAreaParams &p)
	{
		if (!armed_ || i < 0 || i >= tops_.size() || i == current_)
			return;
		const bool sameScreen = monitorOf(i) == monitorOf(current_);
		current_ = i;
		switched_ = true;
		candidate_ = -1;
		if (sameScreen && p.transition == AreaTransition::Pan && p.panMs > 0) {
			from_ = pos_;
			panStart_ = nowMs;
		} else {
			pos_ = QPointF(tops_[i]);
			panStart_ = -1;
		}
	}

	// One step. Returns true when the frame moved by at least a pixel, in which
	// case region() is the rectangle to apply.
	// `cursorMonitor` is the monitor the cursor is on (cursor in its device
	// pixels); -1 when it is on none of them, which counts as a gap.
	bool tick(QPoint cursor, qint64 nowMs, const MultiAreaParams &p, int cursorMonitor = 0)
	{
		switched_ = false;
		if (!armed_)
			return false;
		const QPoint before = pos_.toPoint();
		const int monitorBefore = currentMonitor();

		const int under = cursorMonitor < 0 && !monitors_.isEmpty()
					  ? -1
					  : areaAtOn(tops_, monitors_, size_, cursorMonitor, cursor, current_);
		if (under < 0 || under == current_) {
			// Nowhere, or already there: nothing pending.
			candidate_ = -1;
			candidateSince_ = -1;
		} else if (under != candidate_) {
			candidate_ = under;
			candidateSince_ = nowMs;
		}
		if (candidate_ >= 0 && nowMs - candidateSince_ >= std::max(0, p.hoverMs))
			switchTo(candidate_, nowMs, p);

		if (panStart_ >= 0) {
			const double u = double(nowMs - panStart_) / std::max(1, p.panMs);
			const QPointF to(tops_[current_]);
			if (u >= 1.0) {
				pos_ = to;
				panStart_ = -1;
			} else {
				const double k = ease(u);
				pos_ = from_ + (to - from_) * k;
			}
		}
		// A cut to the same spot on another monitor moved nothing in pixels
		// but everything on screen.
		return pos_.toPoint() != before || currentMonitor() != monitorBefore;
	}

	CaptureRegion region() const
	{
		const QPoint t = pos_.toPoint();
		return CaptureRegion{enabled_, t.x(), t.y(), size_.width(), size_.height()};
	}

private:
	QVector<QPoint> tops_;
	QVector<int> monitors_; // empty: one screen
	QSize size_;
	bool enabled_ = true;
	bool armed_ = false;
	int current_ = -1;
	QPointF pos_;
	QPointF from_;
	qint64 panStart_ = -1;
	int candidate_ = -1;
	qint64 candidateSince_ = -1;
	bool switched_ = false;
};

} // namespace harpia
