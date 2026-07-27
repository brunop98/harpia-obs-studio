// Fade the clip up at the start and out at the end. Handy on captions, logos
// and overlay footage. Leaves position/scale/rotation untouched.
//
//@param inSec  float 0.0 5.0 0.4 Fade in (s)
//@param outSec float 0.0 5.0 0.4 Fade out (s)

function opacity(t, u, dur, ctx) {
    let a = 1.0;
    if (inSec > 0.001)
        a = Math.min(a, t / inSec);
    if (outSec > 0.001)
        a = Math.min(a, (dur - t) / outSec);
    return Math.max(0, Math.min(1, a));
}
