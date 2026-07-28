// Slow push-in — the classic "focus on this part of the screen" tutorial move.
//
// Define any of position(t, u, dur, ctx), scale(...), rotation(...),
// opacity(...). Channels you leave out keep whatever the clip already has, so
// you can script the zoom and still set position by hand in the Inspector.
//
//   t   seconds into this clip      u   progress 0..1
//   dur clip length in seconds      ctx { canvasW, canvasH, fps, index, ... }
//
//@param zoom   float 1.0 6.0 2.0  Zoom level
//@param focusX float 0.0 1.0 0.5  Focus X
//@param focusY float 0.0 1.0 0.5  Focus Y
//@param holdU  float 0.0 1.0 0.6  Reach full zoom at

function ease(x) {
    x = Math.max(0, Math.min(1, x));
    return x * x * (3 - 2 * x); // smoothstep
}

// Multiplies into the clip's own zoom rather than replacing it, so the
// Inspector's Zoom still means something with this script attached.
function baseScale(ctx) { return ctx.base ? ctx.base.scale : 1; }

function scale(t, u, dur, ctx) {
    return baseScale(ctx) * (1 + (zoom - 1) * ease(u / Math.max(0.001, holdU)));
}

function position(t, u, dur, ctx) {
    const bx = ctx.base ? ctx.base.x : 0.5;
    const by = ctx.base ? ctx.base.y : 0.5;
    const k = ease(u / Math.max(0.001, holdU));
    if (k <= 0) return { x: bx, y: by };
    const s = baseScale(ctx) * (1 + (zoom - 1) * k);

    // Focus X/Y is a point in the PICTURE; to put it in the middle of the frame
    // the picture has to move the OTHER way, by the zoomed distance from centre.
    let w1 = 1, h1 = 1;
    if (ctx.clipW > 0 && ctx.clipH > 0 && ctx.canvasW > 0 && ctx.canvasH > 0) {
        const fit = Math.min(ctx.canvasW / ctx.clipW, ctx.canvasH / ctx.clipH);
        w1 = (ctx.clipW * fit) / ctx.canvasW;
        h1 = (ctx.clipH * fit) / ctx.canvasH;
    }
    const spanX = w1 * s, spanY = h1 * s;
    let x = bx + k * (0.5 - (focusX - 0.5) * spanX - bx);
    let y = by + k * (0.5 - (focusY - 0.5) * spanY - by);

    // Keep the picture covering the canvas rather than panning off its edge.
    const halfX = spanX / 2, halfY = spanY / 2;
    x = halfX >= 0.5 ? Math.max(1 - halfX, Math.min(halfX, x)) : 0.5;
    y = halfY >= 0.5 ? Math.max(1 - halfY, Math.min(halfY, y)) : 0.5;
    return { x: x, y: y };
}
