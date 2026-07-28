// Tutorial zoom — push in on a spot, hold, pull back out. Seamless at both ends.
//
// The move every screen tutorial needs: the clip starts and ends at normal
// framing, so it cuts against neighbouring clips invisibly, and in between it
// magnifies whatever you point it at. Drop it on a clip, set Focus X/Y to the
// thing you're talking about, and adjust Zoom.
//
// Focus X/Y are a position in the PICTURE, 0..1 from its top-left — 0.5,0.5 is
// dead centre. That point ends up in the middle of the frame at full zoom.
//
// Both ramps use smoothstep, so there is no visible kick when the move starts
// or stops. The zoomed picture is kept covering the canvas, so a focus point
// near an edge pans as far as it can instead of exposing black bars.
//
// This builds ON TOP of the clip's own framing rather than replacing it:
// ctx.base is the Zoom / Position the Inspector shows (or the keyframes at this
// instant), so a clip set to 0.8 and zoom 2 ends up at 1.6, and repositioning
// the clip by hand still moves where the move starts from.
//
//   t   seconds into this clip      u   progress 0..1
//   dur clip length in seconds      ctx { clipW, clipH, canvasW, canvasH,
//                                         base:{x,y,scale,rotation,opacity}, ... }
//
//@param zoom    float 1.0 8.0 2.0  Zoom level
//@param focusX  float 0.0 1.0 0.5  Focus X (0 = left, 1 = right)
//@param focusY  float 0.0 1.0 0.5  Focus Y (0 = top, 1 = bottom)
//@param inSec   float 0.0 5.0 0.7  Zoom-in time (s)
//@param outSec  float 0.0 5.0 0.7  Zoom-out time (s)
//@param delaySec float 0.0 10.0 0.0 Wait before zooming (s)

function smoothstep(a, b, x) {
    if (b - a <= 0.0001) return x >= b ? 1 : 0;
    const k = Math.max(0, Math.min(1, (x - a) / (b - a)));
    return k * k * (3 - 2 * k);
}

// 0 at both ends of the clip, 1 while held at full zoom.
function amount(t, dur) {
    const inStart = Math.min(delaySec, dur);
    const rampIn = smoothstep(inStart, inStart + inSec, t);
    // Pull out so the move FINISHES exactly at the clip's end.
    const rampOut = 1 - smoothstep(Math.max(inStart, dur - outSec), dur, t);
    return Math.min(rampIn, rampOut);
}

// The clip's own zoom is the baseline this multiplies into.
function baseScale(ctx) { return ctx.base ? ctx.base.scale : 1; }

function scale(t, u, dur, ctx) {
    return baseScale(ctx) * (1 + (zoom - 1) * amount(t, dur));
}

function position(t, u, dur, ctx) {
    const bx = ctx.base ? ctx.base.x : 0.5;
    const by = ctx.base ? ctx.base.y : 0.5;
    const k = amount(t, dur);
    // Not zoomed yet: sit exactly where the clip is framed.
    if (k <= 0) return { x: bx, y: by };
    const s = baseScale(ctx) * (1 + (zoom - 1) * k);

    // How much of the canvas the picture covers at scale 1. The compositor fits
    // the source inside the canvas preserving aspect, so for a source shaped
    // like the canvas this is 1, and for any other shape one axis is smaller.
    let w1 = 1, h1 = 1;
    if (ctx.clipW > 0 && ctx.clipH > 0 && ctx.canvasW > 0 && ctx.canvasH > 0) {
        const fit = Math.min(ctx.canvasW / ctx.clipW, ctx.canvasH / ctx.clipH);
        w1 = (ctx.clipW * fit) / ctx.canvasW;
        h1 = (ctx.clipH * fit) / ctx.canvasH;
    }

    // A point at fraction f across the picture sits at centre + (f - 0.5) * span.
    // Solve for the centre that puts the focus point in the middle of the frame,
    // then blend in from 0.5 so nothing moves before the zoom starts.
    // Ease from the clip's own framing towards "focus point in the middle of the
    // frame" as the zoom comes in. With the clip centred this is the same move
    // it always was.
    const spanX = w1 * s, spanY = h1 * s;
    let x = bx + k * (0.5 - (focusX - 0.5) * spanX - bx);
    let y = by + k * (0.5 - (focusY - 0.5) * spanY - by);

    // Don't pan past the edge of the picture: once it covers the canvas, keep it
    // covering. (If it doesn't cover, leave it centred rather than jam it to a side.)
    const halfX = spanX / 2, halfY = spanY / 2;
    x = halfX >= 0.5 ? Math.max(1 - halfX, Math.min(halfX, x)) : 0.5;
    y = halfY >= 0.5 ? Math.max(1 - halfY, Math.min(halfY, y)) : 0.5;
    return { x: x, y: y };
}
