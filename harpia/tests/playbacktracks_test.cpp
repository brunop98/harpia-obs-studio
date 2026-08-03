// Playback: every track's picture has to keep moving, not just the top one.
//
// The editor decodes SEQUENTIALLY while playing -- seek once, then roll the
// decoder forward -- because per-frame random seeking is what made a 4K
// timeline unwatchable. That is a good trade with one sharp edge: a
// roll-forward decoder can only be in one place at a time, and a timeline can
// put the SAME source on screen from several tracks at once at DIFFERENT source
// timestamps. Stack a clip over itself for a picture-in-picture, split a
// recording and layer the halves, or just use one file twice, and two clips are
// asking one decoder for two different moments on every single frame.
//
// Keyed by source alone, whichever track asks second drags the decoder past the
// first; the first then hits the "already past it, reuse what you have" branch
// forever and shows the other track's frame, frozen, for the rest of the
// playback. Keyed by (source, TRACK), each stack gets its own position.
//
// This file pins both halves:
//   * compose() reports which track is asking, stably, for every clip it draws;
//   * a sequential provider keyed by (source, track) serves both tracks their
//     own timestamps -- with the OLD source-only keying kept as a live control
//     that must still exhibit the freeze, so the test is demonstrably able to
//     tell the two apart.
//
// The fake decoder below reproduces the only two FrameSeeker behaviours that
// matter here: it cannot move backwards without a seek, and asking it to catch
// up drags it forward.
#include "editor/timeline/TimelineCompositor.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QHash>
#include <QImage>

#include <cstdio>
#include <vector>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

namespace {

constexpr int kCw = 64, kCh = 36;

// A stand-in for FrameSeeker's sequential path: position only ever increases
// unless someone seeks, and a request behind the position cannot be served.
struct FakeSeeker {
	qint64 pos = -1;
	int seeks = 0;
	// Roll forward to at least targetMs. Returns the timestamp actually
	// reached, or -1 when the target is behind and no seek was done.
	qint64 rollTo(qint64 targetMs)
	{
		if (pos >= 0 && targetMs < pos)
			return -1; // cannot rewind; the real one cannot either
		pos = std::max(pos, targetMs);
		return pos;
	}
	void seekTo(qint64 ms)
	{
		++seeks;
		pos = ms;
	}
};

// Mirrors the editor's playback branch. `perTrack` selects the fix; false is
// the old source-only keying, kept as the control. `holdMs` is how far past a
// request the decoder may be and still have its held frame reused -- the old
// code had no bound at all, which is the second half of the bug.
struct SeqProvider : TimelineCompositor::FrameProvider {
	bool perTrack = true;
	qint64 holdMs = 100;
	static constexpr qint64 kUnbounded = 1LL << 40;
	QHash<quint64, FakeSeeker> seekers;
	QHash<quint64, qint64> held; // last timestamp served per key, -1 = none
	// What each (source, track) was actually SHOWN, in call order, so the test
	// can look at the sequence rather than only the last value.
	std::vector<std::pair<quint64, qint64>> served;

	quint64 keyFor(int sourceId) const
	{
		return (quint64(quint32(sourceId)) << 32) | quint32(perTrack ? track() : 0);
	}

	QImage frameFor(int sourceId, qint64 srcMs) override
	{
		const quint64 k = keyFor(sourceId);
		FakeSeeker &sk = seekers[k];
		qint64 &lastShown = held[k];
		if (!held.contains(k))
			lastShown = -1;

		// The reuse branch: the decoder is a whisker past the request, so
		// serve what it holds rather than over-advancing it.
		if (lastShown >= 0 && sk.pos >= srcMs && sk.pos - srcMs <= holdMs) {
			served.emplace_back(quint64(quint32(sourceId)) << 32 | quint32(track()),
					    lastShown);
			return QImage(kCw, kCh, QImage::Format_RGBA8888);
		}
		if (sk.pos >= 0 && srcMs < sk.pos)
			sk.seekTo(srcMs);
		const qint64 got = sk.rollTo(srcMs);
		if (got >= 0)
			lastShown = got;
		served.emplace_back(quint64(quint32(sourceId)) << 32 | quint32(track()), lastShown);
		return QImage(kCw, kCh, QImage::Format_RGBA8888);
	}

	// Every timestamp shown to one (source, track) pair, in order.
	std::vector<qint64> timeline(int sourceId, int trackIndex) const
	{
		const quint64 k = quint64(quint32(sourceId)) << 32 | quint32(trackIndex);
		std::vector<qint64> out;
		for (const auto &s : served)
			if (s.first == k)
				out.push_back(s.second);
		return out;
	}
};

// Records which track index compose() announced for each clip it drew.
struct TrackSpy : TimelineCompositor::FrameProvider {
	std::vector<std::pair<int, int>> calls; // (sourceId, track)
	QImage frameFor(int sourceId, qint64) override
	{
		calls.emplace_back(sourceId, track());
		return QImage(kCw, kCh, QImage::Format_RGBA8888);
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

// The shape that breaks it: ONE source, on two tracks, at two different points
// in the file at the same time. Track 0 (drawn on top) reads from 30 s in;
// track 1 reads from the start.
TimelineModel stackedSameSource()
{
	TimelineModel m;
	TlTrack over;
	over.kind = TlTrack::Kind::Video;
	over.name = QStringLiteral("Overlay");
	TlClip pip = videoClip(1, 30000, 40000, 0);
	pip.scale = 0.4;
	over.clips.append(pip);
	m.tracks.append(over);

	TlTrack base;
	base.kind = TlTrack::Kind::Video;
	base.name = QStringLiteral("Base");
	base.clips.append(videoClip(1, 0, 10000, 0));
	m.tracks.append(base);
	return m;
}

// How many DISTINCT timestamps a track was shown. 1 means frozen.
int distinctCount(const std::vector<qint64> &v)
{
	int n = 0;
	for (size_t i = 0; i < v.size(); ++i)
		if (i == 0 || v[i] != v[i - 1])
			++n;
	return n;
}

} // namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- compose() says which track is asking --\n");
	{
		const TimelineModel m = stackedSameSource();
		TrackSpy spy;
		TimelineCompositor::compose(m, 1000, QSize(kCw, kCh), spy, nullptr, 30.0);
		std::printf("     %zu clip(s) drawn\n", spy.calls.size());
		ok(spy.calls.size() == 2, "both stacked clips were drawn");
		bool distinct = spy.calls.size() == 2 && spy.calls[0].second != spy.calls[1].second;
		ok(distinct, "and each reported a DIFFERENT track index");
		// Back-to-front: the bottom track is drawn first.
		ok(spy.calls.size() == 2 && spy.calls[0].second == 1 && spy.calls[1].second == 0,
		   "bottom track first, top track last — the order compose() documents");

		// Stable across frames: a key that changed frame to frame would give
		// each frame its own decoder and defeat the whole point.
		TrackSpy spy2;
		TimelineCompositor::compose(m, 2000, QSize(kCw, kCh), spy2, nullptr, 30.0);
		ok(spy2.calls == spy.calls || (spy2.calls.size() == 2 &&
					       spy2.calls[0].second == spy.calls[0].second &&
					       spy2.calls[1].second == spy.calls[1].second),
		   "and the same indices at a different instant — the key is stable");
	}

	std::printf("\n-- one source on two tracks, played --\n");
	{
		const TimelineModel m = stackedSameSource();
		SeqProvider fixed;
		fixed.perTrack = true;
		for (qint64 t = 0; t < 3000; t += 33)
			TimelineCompositor::compose(m, t, QSize(kCw, kCh), fixed, nullptr, 30.0);

		const std::vector<qint64> top = fixed.timeline(1, 0);   // reads 30s..
		const std::vector<qint64> bottom = fixed.timeline(1, 1); // reads 0s..
		std::printf("     top: %zu frames, %d distinct, %lld..%lld\n", top.size(),
			    distinctCount(top), (long long)(top.empty() ? -1 : top.front()),
			    (long long)(top.empty() ? -1 : top.back()));
		std::printf("     bottom: %zu frames, %d distinct, %lld..%lld\n", bottom.size(),
			    distinctCount(bottom), (long long)(bottom.empty() ? -1 : bottom.front()),
			    (long long)(bottom.empty() ? -1 : bottom.back()));

		ok(!top.empty() && !bottom.empty(), "both tracks were asked for frames");
		ok(distinctCount(top) > 50, "the top track's picture keeps moving");
		ok(distinctCount(bottom) > 50, "and so does the BOTTOM track's — the actual bug");
		// Each read its own part of the file, not the other's.
		ok(!top.empty() && top.front() >= 30000, "the top track read from 30 s in");
		ok(!bottom.empty() && bottom.back() < 30000,
		   "and the bottom track never wandered into the top one's range");
		ok(!bottom.empty() && bottom.back() > bottom.front(),
		   "the bottom track advanced monotonically through its own clip");
	}

	std::printf("\n-- CONTROL: the old source-only keying still breaks --\n");
	{
		// Same timeline, same provider, one field different. If this passed
		// too, the test above would be proving nothing about the keying.
		const TimelineModel m = stackedSameSource();
		SeqProvider shared;
		shared.perTrack = false;
		shared.holdMs = SeqProvider::kUnbounded; // the old, unbounded reuse
		for (qint64 t = 0; t < 3000; t += 33)
			TimelineCompositor::compose(m, t, QSize(kCw, kCh), shared, nullptr, 30.0);

		const std::vector<qint64> bottom = shared.timeline(1, 1);
		int own = 0;
		for (qint64 v : bottom)
			if (v >= 0 && v < 10000) // the bottom clip's own range
				++own;
		std::printf("     bottom track: %zu frames, %d of them from its own clip\n",
			    bottom.size(), own);
		// It is not merely stale -- it is showing the OTHER clip's moment, for
		// almost the whole playback.
		ok(own <= 1, "sharing one decoder leaves the bottom track showing the TOP clip");
		ok(!bottom.empty() && bottom.back() >= 30000,
		   "right through to the end of the run");
	}

	std::printf("\n-- a track that starts deep into its source --\n");
	{
		// Nothing to do with stacking: one track, one clip, reading from an
		// hour in. Rolling forward from zero at a handful of frames per tick
		// would take thousands of ticks to arrive, so the picture would be
		// frozen (or black) for the whole of it. A seek is the only way.
		TimelineModel m;
		TlTrack t;
		t.kind = TlTrack::Kind::Video;
		t.clips.append(videoClip(7, 3600000, 3610000, 0));
		m.tracks.append(t);

		SeqProvider fp;
		for (qint64 at = 0; at < 500; at += 33)
			TimelineCompositor::compose(m, at, QSize(kCw, kCh), fp, nullptr, 30.0);
		const std::vector<qint64> shown = fp.timeline(7, 0);
		std::printf("     first frame shown: %lld ms\n",
			    (long long)(shown.empty() ? -1 : shown.front()));
		ok(!shown.empty() && shown.front() >= 3600000,
		   "the very first frame is already at the clip's start, not at zero");
	}

	std::printf("\n-- a jump backwards within one track --\n");
	{
		// Two clips of one source on one track, the second reading EARLIER in
		// the file than the first -- an ordinary reordered cut. At the join the
		// decoder has to go backwards, which rolling forward cannot do.
		TimelineModel m;
		TlTrack t;
		t.kind = TlTrack::Kind::Video;
		t.clips.append(videoClip(1, 20000, 22000, 0));
		t.clips.append(videoClip(1, 1000, 3000, 2000));
		m.tracks.append(t);

		SeqProvider fp;
		for (qint64 at = 0; at < 4000; at += 33)
			TimelineCompositor::compose(m, at, QSize(kCw, kCh), fp, nullptr, 30.0);
		const std::vector<qint64> shown = fp.timeline(1, 0);
		bool wentBack = false;
		for (size_t i = 1; i < shown.size(); ++i)
			if (shown[i] < shown[i - 1])
				wentBack = true;
		ok(wentBack, "the picture does go back to the earlier cut");
		ok(!shown.empty() && shown.back() < 20000,
		   "and ends inside the second clip's range rather than stuck in the first's");
	}

	std::printf("\n%s (%d failures)\n",
		    failures ? "FAILURES" : "all playback-track checks passed", failures);
	return failures ? 1 : 0;
}
