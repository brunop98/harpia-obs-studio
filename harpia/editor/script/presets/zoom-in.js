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

function scale(t, u, dur, ctx) {
    return 1 + (zoom - 1) * ease(u / Math.max(0.001, holdU));
}

function position(t, u, dur, ctx) {
    const k = ease(u / Math.max(0.001, holdU));
    // Drift from centre toward the point of interest as we zoom in.
    return { x: 0.5 + (focusX - 0.5) * k, y: 0.5 + (focusY - 0.5) * k };
}
