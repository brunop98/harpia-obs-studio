//@component harpia.pulse
//@name Pulse
//@category Motion
//@stage Transform
//@version 1
//@help Breathe the clip in and out in time with a tempo.
//@param bpm float 30 240 120 Beats per minute
//@param depth float 0 1 0.06 Depth

// A worked example of a custom component, and the shape every one of them takes.
//
// Two things to copy from it.
//
// It is a function of ctx.t and nothing else. No counter, no "last frame"
// variable, nothing that remembers. That is what lets the same frame come out
// the same whether you scrubbed to it, played to it, or the exporter rendered
// it on another core — and it is the one rule that cannot be bent.
//
// It MULTIPLIES io.scale instead of assigning it. io arrives holding what the
// clip would look like without this component: its own zoom, its keyframes,
// and whatever components ran before. Assigning would throw all of that away
// and leave the Inspector's own Scale field looking broken.

function evaluate(ctx, io) {
	const beatsPerSecond = ctx.p.bpm / 60;
	// sin() swings -1..1; this maps it to 0..1 so the clip only ever grows.
	const beat = Math.sin(ctx.t * beatsPerSecond * 2 * Math.PI) * 0.5 + 0.5;
	io.scale *= 1 + ctx.p.depth * beat;
}
