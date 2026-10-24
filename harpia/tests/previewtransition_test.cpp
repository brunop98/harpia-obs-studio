// A transition can show two different moments of ONE recording on ONE track.
// Those two positions need separate decoder slots; otherwise every preview
// frame seeks backwards and forwards through the same long GOP.
#include "editor/FrameSeeker.hpp"
#include "editor/timeline/TimelineCompositor.hpp"

#include <QGuiApplication>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>

#include <cstdio>
#include <map>
#include <memory>

using namespace harpia;

static int failures = 0;
static void ok(bool condition, const char *why)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", why);
	if (!condition)
		++failures;
}

static bool makeCounter(const QString &path)
{
	QProcess ff;
	ff.start(QStringLiteral("ffmpeg"), {
		"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
		"nullsrc=s=64x64:r=30:d=12,format=yuv420p,"
		"geq=lum='16+219*mod(floor(N/pow(2\\,floor(X/8)))\\,2)':cb=128:cr=128",
		"-c:v", "libx264", "-preset", "ultrafast", "-g", "360", "-keyint_min", "360",
		"-sc_threshold", "0", "-crf", "8", "-pix_fmt", "yuv420p", path});
	return ff.waitForFinished(20000) && ff.exitStatus() == QProcess::NormalExit && ff.exitCode() == 0;
}

class SlotProvider final : public TimelineCompositor::FrameProvider {
public:
	explicit SlotProvider(const QString &path) : path_(path) {}
	QImage frameFor(int sourceId, qint64 srcMs) override
	{
		tracks.insert(track());
		decoderSlots.insert(decoderSlot());
		const auto key = std::make_pair(sourceId, track() * 2 + decoderSlot());
		auto &decoder = decoders_[key];
		if (!decoder) {
			decoder = std::make_unique<FrameSeeker>();
			if (!decoder->open(path_))
				return {};
		}
		return decoder->frameAt(srcMs, 64, 64);
	}
	int seeks() const
	{
		int total = 0;
		for (const auto &entry : decoders_)
			total += entry.second->seekCount();
		return total;
	}
	int decoderCount() const { return int(decoders_.size()); }
	QSet<int> decoderSlots, tracks;

private:
	QString path_;
	std::map<std::pair<int, int>, std::unique_ptr<FrameSeeker>> decoders_;
};

// The reference has one reader for each known source range, independent of
// the compositor's announced slot, so a missing/wrong slot cannot fool it.
class Reference final : public TimelineCompositor::FrameProvider {
public:
	explicit Reference(const QString &path) { valid = outgoing_.open(path) && incoming_.open(path); }
	QImage frameFor(int, qint64 srcMs) override
	{
		return (srcMs >= 7000 ? outgoing_ : incoming_).frameAt(srcMs, 64, 64);
	}
	bool valid = false;

private:
	FrameSeeker outgoing_, incoming_;
};

static TlClip clip(qint64 sourceStart, qint64 sourceEnd, qint64 outputStart)
{
	TlClip c;
	c.sourceId = 1;
	c.srcStartMs = sourceStart;
	c.srcEndMs = sourceEnd;
	c.outStartMs = outputStart;
	return c;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	QTemporaryDir scratch;
	if (!scratch.isValid())
		return 2;
	const QString video = scratch.filePath(QStringLiteral("long-gop.mp4"));
	if (!makeCounter(video)) {
		ok(false, "created a numbered long-GOP recording");
		return 1;
	}
	TimelineModel transition;
	TlTrack track;
	track.clips = {clip(8000, 10000, 0), clip(1000, 3000, 1000)};
	track.clips[1].transition.enabled = true;
	transition.tracks.append(track);
	SlotProvider provider(video);
	Reference reference(video);
	bool pixels = reference.valid;
	for (int i = 0; i < 30; ++i) {
		const qint64 ms = 1000 + i * 33;
		const QImage actual = TimelineCompositor::compose(transition, ms, QSize(64, 64), provider);
		const QImage expected = TimelineCompositor::compose(transition, ms, QSize(64, 64), reference);
		pixels = !actual.isNull() && actual == expected && pixels;
	}
	ok(pixels, "same-source transition pixels match independent source readers");
	ok(provider.tracks == QSet<int>{0} && provider.decoderSlots == QSet<int>({0, 1}),
	   "both clips keep their track identity and receive separate decoder slots");
	std::printf("     %d decoder(s), %d seek(s) over 30 transition frames\n",
		    provider.decoderCount(), provider.seeks());
	ok(provider.decoderCount() == 2 && provider.seeks() <= 4,
	   "the overlap rolls forward instead of seeking for every source frame");
	provider.decoderSlots.clear();
	const QImage after = TimelineCompositor::compose(transition, 2200, QSize(64, 64), provider);
	const QImage afterReference = TimelineCompositor::compose(transition, 2200, QSize(64, 64), reference);
	ok(!after.isNull() && after == afterReference && provider.decoderSlots == QSet<int>{0},
	   "after the transition, ordinary playback returns to its primary decoder slot");

	TimelineModel adjacent;
	track.clips = {clip(0, 1000, 0), clip(1000, 2000, 1000)};
	adjacent.tracks.append(track);
	SlotProvider uninterrupted(video);
	bool drawn = true;
	for (qint64 ms = 0; ms < 2000; ms += 33)
		drawn = !TimelineCompositor::compose(adjacent, ms, QSize(64, 64), uninterrupted).isNull() && drawn;
	ok(drawn && uninterrupted.decoderSlots == QSet<int>{0} && uninterrupted.decoderCount() == 1 && uninterrupted.seeks() <= 2,
	   "adjacent nonoverlapping clips continue sharing one sequential decoder");
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
