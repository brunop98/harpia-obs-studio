// Exporting part of the timeline: the slice has to RENDER the same.
//
// "Export this clip" is implemented by cutting the model down to the chosen
// window and rebasing it to zero, so that every path downstream -- the
// compositor, the audio mix, the size estimate, the progress arithmetic -- runs
// exactly as it does for a whole-project export. That is only sound if the
// slice is genuinely the same picture, which is a claim about pixels, not about
// field arithmetic. So the core of this file composites BOTH timelines frame by
// frame and compares them.
//
// The frame provider paints the source id and the source time into the pixels,
// so a clip that survives the slice but reads the wrong moment of its source
// shows up as a colour difference rather than passing quietly.
#include "editor/timeline/TimelineCompositor.hpp"
#include "editor/timeline/TimelineSlice.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QImage>
#include <QSignalSpy>

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

constexpr int kCw = 96, kCh = 54;

// A source frame that says which source it is and what moment it is. Any
// mis-sliced source range moves the green channel; any swapped source moves the
// red one.
struct MarkedProvider : TimelineCompositor::FrameProvider {
	QImage frameFor(int sourceId, qint64 srcMs) override
	{
		QImage img(kCw, kCh, QImage::Format_RGBA8888);
		const int r = std::clamp(40 * sourceId, 0, 255);
		const int g = int((srcMs / 20) % 256); // 20 ms per step: visible, unaliased
		const int b = int((srcMs / 5120) % 256);
		img.fill(QColor(r, g, b));
		return img;
	}
};

TlClip videoClip(int sourceId, qint64 srcStart, qint64 srcEnd, qint64 outStart)
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = sourceId;
	c.srcStartMs = srcStart;
	c.srcEndMs = srcEnd;
	c.outStartMs = outStart;
	return c;
}

// Two video tracks, an audio track, keyframes, a component with its own keys,
// and markers -- enough that a slice has something to get wrong on every axis.
TimelineModel buildProject()
{
	TimelineModel m;

	TlTrack over;               // index 0 = drawn on top
	over.kind = TlTrack::Kind::Video;
	over.name = QStringLiteral("Overlay");
	TlClip pip = videoClip(2, 0, 3000, 2000); // 2s..5s
	pip.scale = 0.4;
	pip.posX = 0.75;
	// An animated pose, so a shifted key track shows as a moved picture.
	pip.setKeyframeAt(2000, TlTransform{0.75, 0.25, 0.4, 0.0, 1.0}, TlEase::Linear);
	pip.setKeyframeAt(5000, TlTransform{0.25, 0.75, 0.8, 0.0, 1.0}, TlEase::Linear);
	over.clips.append(pip);
	m.tracks.append(over);

	TlTrack base;
	base.kind = TlTrack::Kind::Video;
	base.name = QStringLiteral("Base");
	base.clips.append(videoClip(1, 1000, 5000, 0));    // 0..4s
	base.clips.append(videoClip(3, 0, 4000, 4000));    // 4s..8s
	m.tracks.append(base);

	TlTrack aud;
	aud.kind = TlTrack::Kind::Audio;
	aud.name = QStringLiteral("Audio");
	TlClip a = videoClip(4, 0, 8000, 0);
	a.fadeInMs = 500;
	a.fadeOutMs = 500;
	aud.clips.append(a);
	m.tracks.append(aud);

	m.markers = {500, 3000, 6500};
	return m;
}

// The one assertion that matters: does the excerpt look like the original did?
// Returns the number of sampled instants whose frames differ.
int renderDiff(const TimelineModel &full, const TimelineModel &slice, qint64 fromMs, int samples)
{
	MarkedProvider fp;
	int bad = 0;
	const qint64 span = slice.durationMs();
	for (int i = 0; i < samples; ++i) {
		// Inclusive of 0, exclusive of the end -- the last instant of a clip
		// belongs to whatever comes next.
		const qint64 t = span * i / samples;
		const QImage a = TimelineCompositor::compose(slice, t, QSize(kCw, kCh), fp,
							     nullptr, 30.0);
		const QImage b = TimelineCompositor::compose(full, fromMs + t, QSize(kCw, kCh), fp,
							     nullptr, 30.0);
		if (a != b)
			++bad;
	}
	return bad;
}

} // namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	const TimelineModel full = buildProject();

	std::printf("\n-- the comparison below can actually fail --\n");
	{
		// Everything in this file rests on comparing composited frames, and a
		// compositor returning the same blank image at every instant would
		// pass every one of those checks. So: the project must render
		// DIFFERENTLY at different times, and a deliberately wrong slice must
		// be caught.
		MarkedProvider fp;
		const QImage a = TimelineCompositor::compose(full, 1000, QSize(kCw, kCh), fp);
		const QImage b = TimelineCompositor::compose(full, 3000, QSize(kCw, kCh), fp);
		ok(!a.isNull() && !b.isNull(), "the compositor produces frames at all");
		ok(a != b, "and different ones at different times");

		// A slice taken from the wrong place: same length, so only the CONTENT
		// can tell them apart.
		const TimelineModel wrong = sliceTimeline(full, 2600, 6600);
		const int bad = renderDiff(full, wrong, 2500, 64);
		std::printf("     a slice 100 ms out of place: %d of 64 frames differ\n", bad);
		ok(bad > 0, "and a slice taken 100 ms off is caught");
	}

	std::printf("\n-- the excerpt renders what the project rendered --\n");
	{
		// A window that cuts INTO clips at both ends and straddles the join
		// between the two base clips, so the slice has to trim heads, trim
		// tails, drop nothing, and keep two tracks composited in order.
		const qint64 from = 2500, to = 6500;
		const TimelineModel sl = sliceTimeline(full, from, to);
		std::printf("     project %lld ms -> excerpt %lld ms\n",
			    (long long)full.durationMs(), (long long)sl.durationMs());
		ok(sl.durationMs() == to - from, "the excerpt is exactly as long as the window");
		const int bad = renderDiff(full, sl, from, 64);
		std::printf("     %d of 64 sampled frames differ\n", bad);
		ok(bad == 0, "every sampled frame is identical to the project's");
	}

	std::printf("\n-- including windows that start and end inside one clip --\n");
	{
		// The "show my friend this bit" case: entirely inside the first base
		// clip, with the overlay not yet on screen.
		const qint64 from = 400, to = 1900;
		const TimelineModel sl = sliceTimeline(full, from, to);
		ok(sl.durationMs() == to - from, "the excerpt is the window's length");
		ok(renderDiff(full, sl, from, 32) == 0, "and renders the same throughout");
	}
	{
		// ...and one that starts mid-animation, which is where a keyframe
		// track that forgot to move would show.
		const qint64 from = 3200, to = 4800;
		const TimelineModel sl = sliceTimeline(full, from, to);
		const int bad = renderDiff(full, sl, from, 32);
		std::printf("     mid-animation window: %d of 32 frames differ\n", bad);
		ok(bad == 0, "an animated overlay keeps its pose across the cut");
	}

	std::printf("\n-- what survives the cut --\n");
	{
		// 6000..7000 leaves the overlay track (2s..5s) with nothing in it, so
		// this is the window where "keep every track" is actually load-bearing.
		const TimelineModel sl = sliceTimeline(full, 6000, 7000);
		ok(sl.tracks[0].clips.isEmpty(), "the window really does empty a track");
		// Kept anyway. Not because the render demands it -- an empty lane
		// composites to nothing wherever it sits, and the negative control for
		// this says so -- but because the slice is a TimelineModel like any
		// other, and the lanes you can see are part of what the thing is. A
		// slice with lanes missing would be a different edit.
		ok(sl.tracks.size() == full.tracks.size(), "every track is kept, empty or not");
		bool kinds = true;
		for (int i = 0; i < sl.tracks.size(); ++i)
			kinds = kinds && sl.tracks[i].kind == full.tracks[i].kind &&
				sl.tracks[i].name == full.tracks[i].name;
		ok(kinds, "in the same order, with their kinds and names");
	}
	{
		const TimelineModel sl = sliceTimeline(full, 4200, 7000);
		// The overlay ends at 5s and the first base clip at 4s: one is partly
		// in, the other entirely out.
		ok(sl.tracks[0].clips.size() == 1, "a clip partly inside the window is kept");
		ok(sl.tracks[1].clips.size() == 1, "and one entirely outside it is dropped");
		std::printf("     overlay now %lld..%lld ms\n",
			    (long long)sl.tracks[0].clips[0].outStartMs,
			    (long long)sl.tracks[0].clips[0].outEndMs());
		ok(sl.tracks[0].clips[0].outStartMs == 0,
		   "a clip already running at the cut starts at zero");
		ok(sl.tracks[0].clips[0].outEndMs() == 800, "and ends when it originally did");
	}

	std::printf("\n-- source time still points at the same footage --\n");
	{
		const TimelineModel sl = sliceTimeline(full, 1500, 3500);
		const TlClip &c = sl.tracks[1].clips[0];
		std::printf("     base clip src %lld..%lld\n", (long long)c.srcStartMs,
			    (long long)c.srcEndMs);
		// The project's base clip is source 1000..5000 laid at 0, so output
		// 1500 is source 2500.
		ok(c.srcStartMs == 2500, "the head trim advances the source range");
		ok(c.srcEndMs == 4500, "and the tail trim pulls its end back");
	}
	{
		// Speed is the part that is easy to get wrong: at 2x, one second of
		// output is two seconds of source.
		TimelineModel m;
		TlTrack t;
		TlClip c = videoClip(1, 0, 8000, 0);
		c.speed = 2.0; // 8s of source in 4s of output
		t.clips.append(c);
		m.tracks.append(t);
		const TimelineModel sl = sliceTimeline(m, 1000, 3000);
		const TlClip &s = sl.tracks[0].clips[0];
		std::printf("     2x clip: src %lld..%lld over %lld ms of output\n",
			    (long long)s.srcStartMs, (long long)s.srcEndMs,
			    (long long)s.outDurationMs());
		ok(s.srcStartMs == 2000 && s.srcEndMs == 6000,
		   "a sped-up clip's source range scales with the speed");
		ok(s.outDurationMs() == 2000, "and it still occupies the window it was cut to");
		ok(renderDiff(m, sl, 1000, 24) == 0, "and renders the same footage");
	}

	std::printf("\n-- markers come along, rebased --\n");
	{
		const TimelineModel sl = sliceTimeline(full, 2000, 6000);
		std::printf("     %d marker(s):", int(sl.markers.size()));
		for (qint64 m : sl.markers)
			std::printf(" %lld", (long long)m);
		std::printf("\n");
		ok(sl.markers.size() == 1, "only the markers inside the window survive");
		ok(!sl.markers.isEmpty() && sl.markers[0] == 1000,
		   "and they move with the window's start");
	}

	std::printf("\n-- audio fades follow the trim --\n");
	{
		// The fade-in lives in the first 500 ms; a window that starts after it
		// must not replay it, or the excerpt opens with a swell that is not in
		// the project.
		const TimelineModel sl = sliceTimeline(full, 3000, 5000);
		const TlClip &a = sl.tracks[2].clips[0];
		std::printf("     fade in %d ms, out %d ms\n", a.fadeInMs, a.fadeOutMs);
		ok(a.fadeInMs == 0, "a fade the cut lands past is gone");
		ok(a.fadeOutMs == 0, "as is one the cut has not reached yet");
	}
	{
		// The documented inexact case, pinned so it stays known: cutting INTO
		// a fade keeps what is left of it, and that remainder restarts from
		// silence rather than resuming partway -- a fade has a length in this
		// model but no offset.
		const TimelineModel sl = sliceTimeline(full, 200, 4000);
		const TlClip &a = sl.tracks[2].clips[0];
		std::printf("     cut 200 ms into a 500 ms fade -> %d ms\n", a.fadeInMs);
		ok(a.fadeInMs == 300, "cutting into a fade keeps the remainder of it");
	}
	{
		// And a fade can never outlast the clip it is on, however short the
		// window is cut.
		const TimelineModel sl = sliceTimeline(full, 0, 300);
		const TlClip &a = sl.tracks[2].clips[0];
		ok(a.fadeInMs <= a.outDurationMs() && a.fadeOutMs <= a.outDurationMs(),
		   "fades are clamped to a window shorter than they are");
	}

	std::printf("\n-- component keyframes move with the clip --\n");
	{
		// Components carry their own key tracks in CLIP time, separately from
		// the transform ones. Three places to remember, so all three are worth
		// checking rather than trusting one to stand for the others.
		TimelineModel m;
		TlTrack t;
		TlClip c = videoClip(1, 0, 4000, 0);
		ComponentInstance ci;
		ci.typeId = QStringLiteral("harpia.blur");
		ci.instanceId = QStringLiteral("fx");
		ci.keys.insert(QStringLiteral("amount"),
			       QVector<PropKey>{PropKey{0, 0.0}, PropKey{4000, 1.0}});
		c.components.append(ci);
		t.clips.append(c);
		m.tracks.append(t);

		const TimelineModel sl = sliceTimeline(m, 1000, 3000);
		const auto &keys = sl.tracks[0].clips[0].components[0].keys[QStringLiteral("amount")];
		std::printf("     keys at %lld and %lld\n", (long long)keys[0].tMs,
			    (long long)keys[1].tMs);
		ok(keys.size() == 2, "the keys outside the window are kept, not dropped");
		ok(keys[0].tMs == -1000 && keys[1].tMs == 3000,
		   "shifted by the head trim, so the value at every instant is unchanged");
	}
	{
		// The same for an effect clip's own parameter keys.
		TimelineModel m;
		TlTrack t;
		t.kind = TlTrack::Kind::Effect;
		TlClip c;
		c.type = TlClip::Type::Effect;
		c.outStartMs = 0;
		c.srcStartMs = 0;
		c.srcEndMs = 4000;
		c.fx.keys.append(FxKey{0, {{QStringLiteral("amount"), 0.0}}, TlEase::Linear, 0.42, 0.58});
		c.fx.keys.append(FxKey{4000, {{QStringLiteral("amount"), 1.0}}, TlEase::Linear, 0.42, 0.58});
		t.clips.append(c);
		m.tracks.append(t);
		const TimelineModel sl = sliceTimeline(m, 1500, 3000);
		const TlClip &s = sl.tracks[0].clips[0];
		ok(s.fx.keys[0].tMs == -1500 && s.fx.keys[1].tMs == 2500,
		   "an effect clip's parameter keys shift too");
		ok(s.outDurationMs() == 1500, "and the effect covers the window it was cut to");
	}

	std::printf("\n-- degenerate windows --\n");
	{
		ok(sliceTimeline(full, 3000, 3000).isEmpty(), "a zero-length window is empty");
		ok(sliceTimeline(full, 4000, 1000).isEmpty(), "so is a backwards one");
		ok(sliceTimeline(full, 99000, 100000).isEmpty(), "and one past the end");
		const TimelineModel whole = sliceTimeline(full, 0, full.durationMs());
		ok(whole.durationMs() == full.durationMs(),
		   "slicing the whole span gives the whole project back");
		ok(renderDiff(full, whole, 0, 32) == 0, "renders identically, of course");
		// A negative start is a clamp, not an offset: the excerpt must not be
		// padded with silence the project never had.
		const TimelineModel neg = sliceTimeline(full, -1000, 2000);
		ok(neg.durationMs() == 2000, "a negative start clamps to zero");
	}

	std::printf("\n-- which stretch \"the selection\" means --\n");
	{
		// The other half of the feature: the window has to be told WHAT to cut
		// out. Selecting two clips on different tracks means the span they
		// jointly occupy -- everything in between comes along, gaps included,
		// because the excerpt is a window of time and not a bag of clips.
		TimelineView v;
		v.setModel(full);
		ok(!v.selectionSpan().isValid(), "nothing selected, nothing to export");

		v.selectClip(1, 0); // base clip, 0..4000
		std::printf("     one clip: %lld..%lld\n", (long long)v.selectionSpan().fromMs,
			    (long long)v.selectionSpan().toMs);
		ok(v.selectionSpan() == TlSpan{0, 4000}, "one clip is its own span");

		v.addToSelection(0, 0); // overlay, 2000..5000 -- overlaps, on another track
		std::printf("     plus an overlapping clip on another track: %lld..%lld\n",
			    (long long)v.selectionSpan().fromMs,
			    (long long)v.selectionSpan().toMs);
		ok(v.selectionSpan() == TlSpan{0, 5000},
		   "two clips are the span they jointly cover");

		v.selectClip(1, 1); // 4000..8000, with the earlier one deselected
		ok(v.selectionSpan() == TlSpan{4000, 8000}, "and it follows the selection");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
