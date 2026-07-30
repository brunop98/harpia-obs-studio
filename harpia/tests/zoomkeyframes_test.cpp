// "Zoom here": does the point you clicked end up in the middle of the frame?
//
// That is the whole promise, and it is geometry, so it is checkable. The test
// asks the COMPOSITOR where the clip lands (clipRectOnCanvas -- the same call
// that draws it) rather than re-deriving the layout, because a generator that
// agrees with a second copy of the maths and disagrees with the renderer would
// pass a test and still put the wrong pixel in the middle.
//
// The other half is the failure everyone hits when they write this by hand:
// zoom toward something near an edge and the frame slides off the canvas,
// showing background through the gap. The pose has to be clamped, and clamped
// only on the axes where the clip was covering the canvas to begin with.
#include "editor/timeline/TimelineCompositor.hpp"
#include "editor/timeline/ZoomKeyframes.hpp"

#include <QGuiApplication>

#include <cmath>
#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

namespace {

const QSize kCanvas(1920, 1080);
const QSize kSrc(1920, 1080); // a screen recording: exactly fills the canvas

TlClip clipOf(qint64 outStart, qint64 lenMs)
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = 1;
	c.srcStartMs = 0;
	c.srcEndMs = lenMs;
	c.outStartMs = outStart;
	return c;
}

// Where does the clip's own point (u,v) actually land on the canvas, according
// to the compositor?
QPointF pointOnCanvas(const TlTransform &tf, QPointF inClip, QSize canvas = kCanvas,
		      QSize src = kSrc)
{
	const QRectF r = TimelineCompositor::clipRectOnCanvas(tf, canvas, src);
	return QPointF(r.x() + inClip.x() * r.width(), r.y() + inClip.y() * r.height());
}

// Does the clip still cover every pixel of the canvas?
bool coversCanvas(const TlTransform &tf, QSize canvas = kCanvas, QSize src = kSrc)
{
	const QRectF r = TimelineCompositor::clipRectOnCanvas(tf, canvas, src);
	return r.left() <= 0.001 && r.top() <= 0.001 && r.right() >= canvas.width() - 0.001 &&
	       r.bottom() >= canvas.height() - 0.001;
}

} // namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);

	std::printf("\n-- the point you clicked lands in the middle --\n");
	{
		// Asked of the compositor, not of a second copy of the arithmetic.
		// Inside the reachable range for 1.6x -- see the section below for what
		// that range is and why points outside it cannot be centred at all.
		const QPointF target(0.35, 0.65);
		const TlTransform tf = zoomPoseFor(kCanvas, kSrc, target, 1.6);
		const QPointF landed = pointOnCanvas(tf, target);
		std::printf("     clicked (%.2f,%.2f) -> canvas (%.1f,%.1f), centre is (%d,%d)\n",
			    target.x(), target.y(), landed.x(), landed.y(), kCanvas.width() / 2,
			    kCanvas.height() / 2);
		ok(std::abs(landed.x() - kCanvas.width() / 2.0) < 1.0, "horizontally centred");
		ok(std::abs(landed.y() - kCanvas.height() / 2.0) < 1.0, "vertically centred");
		ok(std::abs(tf.scale - 1.6) < 1e-9, "at the zoom that was asked for");
	}
	{
		// Several points, including the exact centre (which must not move).
		bool allCentred = true;
		for (QPointF p : {QPointF(0.5, 0.5), QPointF(0.35, 0.35), QPointF(0.62, 0.44),
				  QPointF(0.5, 0.28)}) {
			const TlTransform tf = zoomPoseFor(kCanvas, kSrc, p, 1.8);
			const QPointF got = pointOnCanvas(tf, p);
			allCentred = allCentred && std::abs(got.x() - 960.0) < 1.0 &&
				     std::abs(got.y() - 540.0) < 1.0;
		}
		ok(allCentred, "and so does every other point that fits");
		const TlTransform mid = zoomPoseFor(kCanvas, kSrc, QPointF(0.5, 0.5), 1.8);
		ok(std::abs(mid.posX - 0.5) < 1e-9 && std::abs(mid.posY - 0.5) < 1e-9,
		   "zooming to the centre does not move the picture sideways");
	}

	std::printf("\n-- zooming near an edge does not show background --\n");
	{
		// The failure this is really guarding: at 1.6x the frame is only 60%
		// wider than the canvas, so a click in the corner would drag its edge
		// into shot if the pose were taken literally.
		const QPointF corner(0.02, 0.02);
		const TlTransform tf = zoomPoseFor(kCanvas, kSrc, corner, 1.6);
		const QRectF r = TimelineCompositor::clipRectOnCanvas(tf, kCanvas, kSrc);
		std::printf("     corner click at 1.6x -> clip rect (%.0f,%.0f %.0fx%.0f)\n", r.x(),
			    r.y(), r.width(), r.height());
		ok(coversCanvas(tf), "the frame still covers the canvas");
		// Clamped, so the corner does NOT reach the middle -- it gets as close
		// as it can. Saying so here keeps the first section's claim honest:
		// it holds for points that fit, not for all of them.
		const QPointF landed = pointOnCanvas(tf, corner);
		std::printf("     it gets as close as it can: (%.0f,%.0f)\n", landed.x(), landed.y());
		ok(landed.x() < kCanvas.width() / 2.0 && landed.y() < kCanvas.height() / 2.0,
		   "and moves toward the corner as far as it can without one");

		bool allCover = true;
		for (double u : {0.0, 0.05, 0.5, 0.95, 1.0})
			for (double v : {0.0, 0.05, 0.5, 0.95, 1.0})
				for (double s : {1.05, 1.6, 2.5, 6.0})
					allCover = allCover &&
						   coversCanvas(zoomPoseFor(kCanvas, kSrc,
									    QPointF(u, v), s));
		ok(allCover, "no click anywhere, at any zoom, opens a gap");
	}
	{
		// A clip that does NOT cover the canvas (4:3 letterboxed on 16:9) has
		// bars by the user's own choice. Clamping the horizontal axis there
		// would be fighting the layout, not protecting it -- so that axis is
		// left alone and only the covering one is held.
		const QSize src43(1440, 1080);
		const TlTransform tf = zoomPoseFor(kCanvas, src43, QPointF(0.1, 0.5), 1.1);
		const QRectF r = TimelineCompositor::clipRectOnCanvas(tf, kCanvas, src43);
		std::printf("     4:3 at 1.1x -> %.0f wide on a %d canvas\n", r.width(),
			    kCanvas.width());
		ok(r.width() < kCanvas.width(), "the clip genuinely does not span the canvas");
		ok(r.top() <= 0.001 && r.bottom() >= kCanvas.height() - 0.001,
		   "the axis that does cover it is still held");
		// And the axis that does NOT is left free, so the click is honoured
		// there in full. Clamping it would mean an inverted range -- lo above
		// hi -- which is undefined behaviour in std::clamp as well as wrong;
		// the `w >= canvas.width()` guard is what rules both out, since it is
		// exactly the condition under which W - w/2 <= w/2.
		const QPointF landed = pointOnCanvas(tf, QPointF(0.1, 0.5), kCanvas, src43);
		std::printf("     the clicked point lands at x=%.0f (centre %d)\n", landed.x(),
			    kCanvas.width() / 2);
		ok(std::abs(landed.x() - kCanvas.width() / 2.0) < 1.0,
		   "so the click is honoured in full on the free axis");
	}

	std::printf("\n-- how far off-centre you can go, and why --\n");
	{
		// Not a shortcoming of the implementation, a property of the geometry,
		// and worth pinning because it decides what the feature can promise.
		//
		// At scale s the frame is s times the canvas, so it can slide by half
		// the overhang before its edge comes into shot:
		//
		//     |u - 0.5|  <=  (1 - 1/s) / 2
		//
		// At 1.6x that is 0.1875 -- only the middle 37% of the picture can be
		// brought to the centre. At 3x it is 0.333, and at 6x, 0.417. Clicking
		// a corner at a low zoom therefore moves the picture only a little,
		// and the honest fix for a user who wants more is more zoom, not a
		// generator that quietly overrides the zoom they asked for.
		auto reach = [](double s) { return (1.0 - 1.0 / s) / 2.0; };
		for (double s : {1.6, 2.0, 3.0, 6.0}) {
			const double r = reach(s);
			// Just inside the limit: centred.
			const QPointF in(0.5 + r - 0.002, 0.5);
			const QPointF gotIn = pointOnCanvas(zoomPoseFor(kCanvas, kSrc, in, s), in);
			// Just outside: cannot be, and is clamped instead.
			const QPointF out(0.5 + r + 0.02, 0.5);
			const QPointF gotOut = pointOnCanvas(zoomPoseFor(kCanvas, kSrc, out, s), out);
			std::printf("     %.1fx: reach %.3f | at the limit x=%.0f | past it x=%.0f\n",
				    s, r, gotIn.x(), gotOut.x());
			ok(std::abs(gotIn.x() - 960.0) < 2.0, "a point at the limit is centred");
			ok(gotOut.x() > 960.0, "one past it is not, and lands short of centre");
			ok(coversCanvas(zoomPoseFor(kCanvas, kSrc, out, s)),
			   "but still without opening a gap");
		}
	}

	std::printf("\n-- what it writes is four ordinary keyframes --\n");
	{
		TlClip c = clipOf(0, 10000);
		ZoomSettings zs; // 400 lead, 1500 hold, 500 out, 1.6x
		ok(addZoomAt(c, 5000, QPointF(0.3, 0.3), kCanvas, kSrc, zs), "a zoom is added");
		std::printf("     %d keys at:", int(c.keys.size()));
		for (const TlKeyframe &k : c.keys)
			std::printf(" %lldms", (long long)k.tMs);
		std::printf("\n");
		ok(c.keys.size() == 4, "four of them: in, peak, hold, out");
		ok(c.keys[0].tMs == 4600 && c.keys[1].tMs == 5000, "the push-in LEADS the moment");
		ok(c.keys[2].tMs == 6500 && c.keys[3].tMs == 7000, "then holds, then leaves");

		// Only position and scale. Keying rotation or opacity here would freeze
		// whatever they happened to be and quietly override a fade or a spin.
		bool lanesRight = true;
		for (const TlKeyframe &k : c.keys)
			lanesRight = lanesRight && k.pos.on && k.scale.on && !k.rot.on && !k.opacity.on;
		ok(lanesRight, "pinning position and scale only, not rotation or opacity");
	}
	{
		// The poses those keys resolve to, read back the way the compositor
		// reads them.
		TlClip c = clipOf(0, 10000);
		ZoomSettings zs;
		addZoomAt(c, 5000, QPointF(0.3, 0.3), kCanvas, kSrc, zs);
		std::printf("     scale: %.2f at 0s, %.2f at 5s, %.2f at 6.5s, %.2f at 8s\n",
			    c.transformAt(0).scale, c.transformAt(5000).scale,
			    c.transformAt(6500).scale, c.transformAt(8000).scale);
		ok(std::abs(c.transformAt(4600).scale - 1.0) < 1e-6, "it starts from where it was");
		ok(std::abs(c.transformAt(5000).scale - 1.6) < 1e-6, "full zoom at the moment itself");
		ok(std::abs(c.transformAt(6500).scale - 1.6) < 1e-6, "still there through the hold");
		ok(std::abs(c.transformAt(7000).scale - 1.0) < 1e-6, "and back out afterwards");
		// Before and after the envelope the clip is untouched, so an unzoomed
		// stretch really is unzoomed.
		ok(std::abs(c.transformAt(0).scale - 1.0) < 1e-6, "before it, nothing has changed");
		ok(std::abs(c.transformAt(9500).scale - 1.0) < 1e-6, "after it, likewise");
		// Mid-push it is on the way, not snapped.
		const double mid = c.transformAt(4800).scale;
		std::printf("     halfway through the push-in: %.3f\n", mid);
		ok(mid > 1.0 && mid < 1.6, "the push-in is a movement, not a cut");
	}

	std::printf("\n-- it fits itself into the clip --\n");
	{
		// Clicking near the very start: the envelope slides inwards rather
		// than being cut off, because a zoom truncated by the clip's edge ends
		// mid-push and reads as a jump back.
		TlClip c = clipOf(0, 10000);
		ZoomSettings zs;
		ok(addZoomAt(c, 100, QPointF(0.5, 0.5), kCanvas, kSrc, zs),
		   "a zoom near the start still lands");
		ok(!c.keys.isEmpty() && c.keys.first().tMs >= 0, "with no key before the clip begins");
		bool inRange = true;
		for (const TlKeyframe &k : c.keys)
			inRange = inRange && k.tMs >= 0 && k.tMs <= c.outDurationMs();
		ok(inRange, "and none past its end");
	}
	{
		TlClip c = clipOf(0, 10000);
		ZoomSettings zs;
		addZoomAt(c, 9950, QPointF(0.5, 0.5), kCanvas, kSrc, zs);
		bool inRange = true;
		for (const TlKeyframe &k : c.keys)
			inRange = inRange && k.tMs >= 0 && k.tMs <= c.outDurationMs();
		std::printf("     clicked at 9.95s of a 10s clip, last key at %lldms\n",
			    c.keys.isEmpty() ? -1LL : (long long)c.keys.last().tMs);
		ok(inRange, "the same at the other end");
		ok(std::abs(c.transformAt(10000).scale - 1.0) < 1e-6,
		   "and it has finished pulling out by the time the clip ends");
	}
	{
		// Too short to arrive and leave: refuse, rather than write half a zoom.
		TlClip c = clipOf(0, 300);
		ZoomSettings zs;
		ok(!addZoomAt(c, 150, QPointF(0.5, 0.5), kCanvas, kSrc, zs),
		   "a clip with no room refuses the zoom");
		ok(c.keys.isEmpty(), "and is left completely alone");
	}

	std::printf("\n-- a clip that is already animated is joined, not overruled --\n");
	{
		// Dropping a zoom onto a clip that already moves must not yank it back
		// to the base pose at the edges of the envelope.
		TlClip c = clipOf(0, 10000);
		c.setKeyframeAt(0, TlTransform{0.5, 0.5, 1.0, 0.0, 1.0}, TlEase::Linear);
		c.setKeyframeAt(10000, TlTransform{0.5, 0.5, 1.3, 0.0, 1.0}, TlEase::Linear);
		const double atStartBefore = c.transformAt(4600).scale;
		const double atEndBefore = c.transformAt(7000).scale;
		std::printf("     existing animation reads %.3f at 4.6s, %.3f at 7s\n",
			    atStartBefore, atEndBefore);
		ok(addZoomAt(c, 5000, QPointF(0.4, 0.4), kCanvas, kSrc, ZoomSettings{}),
		   "the zoom is added on top");
		ok(std::abs(c.transformAt(4600).scale - atStartBefore) < 1e-6,
		   "it leaves from where the clip already was");
		ok(std::abs(c.transformAt(7000).scale - atEndBefore) < 1e-6,
		   "and returns to where the clip was going");
		ok(std::abs(c.transformAt(5000).scale - 1.6) < 1e-6, "with the zoom in between");
	}

	std::printf("\n-- a clip that does not start at zero --\n");
	{
		// The keys are clip-relative but the click is an output time, and that
		// conversion is the sort of thing that works right up until a clip is
		// dragged along the timeline.
		TlClip c = clipOf(30000, 10000);
		ok(addZoomAt(c, 35000, QPointF(0.3, 0.3), kCanvas, kSrc, ZoomSettings{}),
		   "a zoom lands on a clip that starts at 30s");
		ok(std::abs(c.transformAt(35000).scale - 1.6) < 1e-6,
		   "full zoom at the output time that was clicked");
		ok(std::abs(c.transformAt(30000).scale - 1.0) < 1e-6,
		   "and not at the same number of ms into the timeline");
	}

	std::printf("\n-- a click is a point on the CANVAS, not in the clip --\n");
	{
		// While the clip is untransformed the two are the same, which is why
		// this is easy to get wrong and then not notice.
		const TlTransform rest; // 0.5/0.5, scale 1
		const QPointF got = clipPointFromCanvas(rest, kCanvas, kSrc, QPointF(0.25, 0.75));
		ok(std::abs(got.x() - 0.25) < 1e-9 && std::abs(got.y() - 0.75) < 1e-9,
		   "at rest they are the same point");
	}
	{
		// Zoomed in 2x on the top-left quadrant: the middle of the SCREEN is
		// now showing a different part of the source, so clicking the middle
		// must resolve to that part and not to the source's middle.
		const TlTransform zoomed = zoomPoseFor(kCanvas, kSrc, QPointF(0.3, 0.3), 2.0);
		const QPointF centreClick = clipPointFromCanvas(zoomed, kCanvas, kSrc,
								QPointF(0.5, 0.5));
		std::printf("     zoomed on (0.30,0.30): the screen centre is source (%.3f,%.3f)\n",
			    centreClick.x(), centreClick.y());
		ok(std::abs(centreClick.x() - 0.3) < 0.01 && std::abs(centreClick.y() - 0.3) < 0.01,
		   "the screen centre resolves to what is being shown there");

		// The round trip that makes "zoom in a bit more on that" work: take a
		// screen point, convert, re-zoom, and the same screen point still
		// shows the same pixel.
		const QPointF screen(0.4, 0.6);
		const QPointF inClip = clipPointFromCanvas(zoomed, kCanvas, kSrc, screen);
		const TlTransform again = zoomPoseFor(kCanvas, kSrc, inClip, 3.0);
		const QPointF landed = pointOnCanvas(again, inClip);
		std::printf("     re-zooming on it puts it at (%.0f,%.0f)\n", landed.x(), landed.y());
		ok(std::abs(landed.x() - 960.0) < 2.0 && std::abs(landed.y() - 540.0) < 2.0,
		   "zooming again on a screen point centres the pixel that was there");
	}
	{
		// A click on the letterbox bars of a 4:3 clip is outside the picture.
		const QSize src43(1440, 1080);
		const TlTransform rest;
		const QPointF onBar = clipPointFromCanvas(rest, kCanvas, src43, QPointF(0.02, 0.5));
		ok(onBar.x() >= 0.0 && onBar.x() <= 1.0,
		   "a click on the letterbox bar clamps into the picture");
	}

	std::printf("\n-- Snap pulls a drag onto the centre --\n");
	{
		// Dragging to EXACTLY centred by hand is a game of one-pixel
		// corrections you lose: 0.4997 reads as centred and is not.
		bool sx = false, sy = false;
		TlTransform near_;
		near_.posX = 0.4970;
		near_.posY = 0.5040;
		const TlTransform snapped = snapPoseToCentre(near_, 0.01, &sx, &sy);
		ok(snapped.posX == 0.5 && snapped.posY == 0.5, "a near-centre pose lands on centre");
		ok(sx && sy, "and reports both axes, so the guides can be shown");

		// Per axis: sliding down the middle keeps its horizontal centring
		// instead of needing both held at once.
		TlTransform oneAxis;
		oneAxis.posX = 0.5002;
		oneAxis.posY = 0.2000;
		const TlTransform half = snapPoseToCentre(oneAxis, 0.01, &sx, &sy);
		std::printf("     (0.5002, 0.2000) -> (%.4f, %.4f)  snapX=%d snapY=%d\n", half.posX,
			    half.posY, int(sx), int(sy));
		ok(half.posX == 0.5, "the axis that is near centre snaps");
		ok(half.posY == 0.2, "the one that is not is left exactly alone");
		ok(sx && !sy, "and only the snapped axis reports");

		// Far away, nothing happens -- a snap that reaches too far is a drag
		// you cannot place.
		TlTransform far_;
		far_.posX = 0.44;
		far_.posY = 0.62;
		const TlTransform untouched = snapPoseToCentre(far_, 0.01, &sx, &sy);
		ok(untouched.posX == 0.44 && untouched.posY == 0.62, "a pose well off centre is not moved");
		ok(!sx && !sy, "and reports no snap");

		// Threshold 0 is how "Snap off" is expressed.
		bool zx = true, zy = true;
		const TlTransform off = snapPoseToCentre(near_, 0.0, &zx, &zy);
		ok(off.posX == near_.posX && off.posY == near_.posY, "a zero threshold snaps nothing");
		ok(!zx && !zy, "and says so");
	}

	std::printf("\n-- Prev/Next keyframe wraps around --\n");
	{
		// With three keys, pressing Next on the third used to do nothing, which
		// reads as a broken button rather than as the end of the list.
		QVector<TlKeyframe> keys;
		for (qint64 t : {0LL, 1000LL, 2000LL}) {
			TlKeyframe k;
			k.tMs = t;
			keys.append(k);
		}
		ok(stepKeyIndex(keys, 0, +1) == 1, "Next from the first goes to the second");
		ok(stepKeyIndex(keys, 1000, +1) == 2, "and on to the third");
		std::printf("     Next from the last -> index %d\n", stepKeyIndex(keys, 2000, +1));
		ok(stepKeyIndex(keys, 2000, +1) == 0, "Next from the LAST wraps to the first");
		ok(stepKeyIndex(keys, 2000, -1) == 1, "Prev walks back");
		std::printf("     Prev from the first -> index %d\n", stepKeyIndex(keys, 0, -1));
		ok(stepKeyIndex(keys, 0, -1) == 2, "and Prev from the first wraps to the last");

		// Between keys, it goes to the neighbour rather than wrapping.
		ok(stepKeyIndex(keys, 1500, +1) == 2, "from between two keys, Next takes the later");
		ok(stepKeyIndex(keys, 1500, -1) == 1, "and Prev the earlier");

		// A single key has nowhere to go: wrapping onto itself would look like
		// the button doing nothing, which is what this is fixing.
		QVector<TlKeyframe> one{keys[0]};
		ok(stepKeyIndex(one, 0, +1) == -1, "a lone key has nowhere to step");
		ok(stepKeyIndex({}, 0, +1) == -1, "and neither does an empty track");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
