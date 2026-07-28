// Handheld / impact shake. Deterministic (a hash of the frame time, not
// Math.random), so the preview and the exported file agree exactly.
//
//@param amount float 0.0 0.2 0.02 Shake amount
//@param speed  float 0.5 40.0 14.0 Shake speed
//@param spin   float 0.0 10.0 0.8  Rotation wobble (deg)

function noise(x) {
    // Cheap deterministic pseudo-noise in -1..1.
    const s = Math.sin(x * 127.1) * 43758.5453;
    return (s - Math.floor(s)) * 2 - 1;
}

// The shake is an OFFSET from wherever the clip is framed, not an absolute
// position — so it layers over a reposition, a keyframed move, or a zoom script
// earlier in the stack instead of throwing it away.
function position(t, u, dur, ctx) {
    const bx = ctx.base ? ctx.base.x : 0.5;
    const by = ctx.base ? ctx.base.y : 0.5;
    return {
        x: bx + noise(t * speed) * amount,
        y: by + noise(t * speed + 19.7) * amount,
    };
}

function rotation(t, u, dur, ctx) {
    const br = ctx.base ? ctx.base.rotation : 0;
    return br + noise(t * speed + 5.3) * spin;
}
