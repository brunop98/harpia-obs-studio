#pragma once

// "Zoom here": click a point on the preview, get a push-in to it.
//
// A screen recording of a 1080p desktop, played back at 1080p, is unreadable --
// the thing you are pointing at is 30 pixels wide. The fix is to move the
// camera, and doing that by hand means four keyframes with matched easing on
// two channels, positioned so the interesting pixel lands in the middle. Nobody
// does that more than twice.
//
// What comes out is ORDINARY Transform keyframes. Not a "zoom effect" with its
// own settings panel and its own rules -- the keys land in TlClip::keys where
// the keyframe editor can see them, so any single zoom can be dragged, retimed,
// re-eased or deleted afterwards with the tools that already exist. A generator
// that produced something only it could edit would be worse than the four
// keyframes it saves you.
//
// The envelope is: hold the pose you were on, ease in to the zoom just BEFORE
// the moment of interest (so the camera leads the action rather than chasing
// it), hold there, then ease back out.
//
//   scale ─────╮          ╭──────
//              ╰──────────╯
//        lead-in   hold    out
//              ^ the click
//
// Everything here is pure geometry on the model, so zoomkeyframes_test can
// check the part that is actually hard: that the point you clicked really does
// end up in the middle of the frame, and that pushing in near an edge does not
// drag the picture off the canvas and show background through the gap.

#include "TimelineModel.hpp"

#include <QPointF>
#include <QSize>

namespace harpia {

struct ZoomSettings {
	double scale = 1.6;   // how far in; 1.0 = no zoom
	int leadInMs = 400;   // push-in, finishing AT the chosen moment
	int holdMs = 1500;    // time held at full zoom
	int outMs = 500;      // pull-out
	TlEase ease = TlEase::EaseInOut;
};

// The pose that puts `pointInClip` (normalised 0..1 within the clip's own
// picture, which is what a click on the preview gives) at the centre of the
// canvas, at `scale`.
//
// Clamped so a clip that covers the canvas still covers it: at 2x you can move
// the centre half a canvas in each direction and no further, or the frame's
// edge comes into shot with nothing behind it. An axis where the clip does NOT
// already cover the canvas (a 4:3 clip letterboxed on a 16:9 timeline) is left
// alone -- there is a gap there by the user's own choice, and clamping would
// fight it rather than fix it.
TlTransform zoomPoseFor(QSize canvas, QSize srcSize, QPointF pointInClip, double scale);

// A click on the preview gives a point on the CANVAS; the two functions here
// want a point in the CLIP. Those are the same thing only while the clip sits
// at scale 1, centred -- which is exactly the case you test in and exactly not
// the case the second time you zoom. Converting through the pose the clip is
// actually at means "zoom in a bit more on that" targets the pixel under the
// pointer rather than a different one that happened to be there before.
QPointF clipPointFromCanvas(const TlTransform &current, QSize canvas, QSize srcSize,
			    QPointF canvasNorm);

// Write a zoom into `clip` centred on `atOutMs` (an OUTPUT-time position).
//
// Returns false and changes nothing when there is no room: a clip shorter than
// the push-in plus the pull-out cannot hold a zoom that arrives and leaves, and
// half of one would just be a lurch.
bool addZoomAt(TlClip &clip, qint64 atOutMs, QPointF pointInClip, QSize canvas, QSize srcSize,
	       const ZoomSettings &settings);

} // namespace harpia
