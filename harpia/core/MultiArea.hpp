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

class AreaSwitcher {
public:
	// Which area contains `p` (half-open on the far edges, like the crop).
	// `prefer` wins when it contains the point too; -1 when none does.
	static int areaAt(const QVector<QPoint> &tops, QSize size, QPoint p, int prefer = -1)
	{
		auto inside = [&](int i) {
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
	static int startIndex(const QVector<QPoint> &tops, QSize size, QPoint cursor)
	{
		const int i = areaAt(tops, size, cursor);
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
	void arm(const QVector<QPoint> &tops, QSize size, int start, bool enabled = true)
	{
		tops_ = tops;
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
	// True for the one tick() call on which the shown area changed -- the
	// moment to drop a marker.
	bool switchedThisTick() const { return switched_; }
	bool panning() const { return panStart_ >= 0; }

	// Jump (or pan) to area `i` now, skipping the hover delay.
	void switchTo(int i, qint64 nowMs, const MultiAreaParams &p)
	{
		if (!armed_ || i < 0 || i >= tops_.size() || i == current_)
			return;
		current_ = i;
		switched_ = true;
		candidate_ = -1;
		if (p.transition == AreaTransition::Pan && p.panMs > 0) {
			from_ = pos_;
			panStart_ = nowMs;
		} else {
			pos_ = QPointF(tops_[i]);
			panStart_ = -1;
		}
	}

	// One step. Returns true when the frame moved by at least a pixel, in which
	// case region() is the rectangle to apply.
	bool tick(QPoint cursor, qint64 nowMs, const MultiAreaParams &p)
	{
		switched_ = false;
		if (!armed_)
			return false;
		const QPoint before = pos_.toPoint();

		const int under = areaAt(tops_, size_, cursor, current_);
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
		return pos_.toPoint() != before;
	}

	CaptureRegion region() const
	{
		const QPoint t = pos_.toPoint();
		return CaptureRegion{enabled_, t.x(), t.y(), size_.width(), size_.height()};
	}

private:
	QVector<QPoint> tops_;
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
