// Audio preview and export share this mixer. Check both its sample behavior
// and the lifetime of cached inputs when another mix evicts those inputs.
#include "editor/TimelineAudio.hpp"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool condition, const char *what)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++failures;
}

static bool writeTone(const QString &path, qint16 value)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	constexpr quint32 frames = 24000;
	constexpr quint32 bytes = frames * 2 * 2;
	QDataStream out(&file);
	out.setByteOrder(QDataStream::LittleEndian);
	out.writeRawData("RIFF", 4);
	out << quint32(36 + bytes);
	out.writeRawData("WAVEfmt ", 8);
	out << quint32(16) << quint16(1) << quint16(2) << quint32(48000)
	    << quint32(192000) << quint16(4) << quint16(16);
	out.writeRawData("data", 4);
	out << bytes;
	for (quint32 sample = 0; sample < frames * 2; ++sample)
		out << value;
	return out.status() == QDataStream::Ok;
}

static TlClip clip(int source, qint64 start = 0)
{
	TlClip c;
	c.sourceId = source;
	c.srcEndMs = 100;
	c.outStartMs = start;
	c.fadeInMs = c.fadeOutMs = 0;
	return c;
}

static TimelineModel modelFor(const TlClip &c)
{
	TlTrack track;
	track.kind = TlTrack::Kind::Audio;
	track.clips = {c};
	TimelineModel m;
	m.tracks = {track};
	return m;
}

static float sampleAt(const std::vector<float> &pcm, qint64 ms)
{
	const size_t sample = size_t(TimelineAudio::frameForMs(ms)) * TimelineAudio::kChannels;
	return sample < pcm.size() ? pcm[sample] : 0.0f;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	QTemporaryDir scratch;
	if (!scratch.isValid())
		return 2;
	QMap<int, QString> sources;
	bool generated = true;
	for (int source = 1; source <= 15; ++source) {
		const QString path = scratch.filePath(QStringLiteral("tone%1.wav").arg(source));
		generated = writeTone(path, qint16(3000 + source * 100)) && generated;
		sources.insert(source, path);
	}
	ok(generated, "generated real stereo source audio");
	const auto lookup = [&sources](int id) { return sources.value(id); };
	const auto mix = [&lookup](const TimelineModel &m) { return TimelineAudio::mixToBuffer(m, lookup); };

	std::printf("\n-- audible placement and levels --\n");
	TimelineModel basic = modelFor(clip(1, 50));
	const auto reference = mix(basic);
	const float unity = sampleAt(reference, 100);
	ok(!reference.empty() && unity > 0.08f, "the audio clip is decoded and mixed");
	ok(sampleAt(reference, 25) == 0.0f && sampleAt(reference, 100) > 0.0f,
	   "placement leaves silence before the clip and sound inside it");
	TimelineModel gain = basic;
	gain.tracks[0].gain = 0.5;
	gain.tracks[0].clips[0].volume = 0.5;
	ok(std::abs(sampleAt(mix(gain), 100) - unity * 0.25f) < 1e-6f,
	   "clip volume and lane gain multiply at sample precision");
	TimelineModel muted = basic;
	muted.tracks[0].muted = true;
	ok(mix(muted).empty() && TimelineAudio::mixKey(muted).isEmpty(), "muted tracks contribute no audio");
	TimelineModel hidden = basic;
	hidden.tracks[0].hidden = true;
	ok(mix(hidden) == reference && TimelineAudio::mixKey(hidden) == TimelineAudio::mixKey(basic),
	   "hiding video leaves its sound unchanged");
	TimelineModel solo = basic;
	solo.tracks[0].solo = true;
	TlTrack extra = basic.tracks[0];
	extra.clips[0].sourceId = 2;
	solo.tracks.append(extra);
	ok(mix(solo) == reference, "solo excludes the other audible lane");
	TimelineModel image = basic;
	image.tracks[0].clips[0].type = TlClip::Type::Image;
	ok(mix(image).empty() && TimelineAudio::mixKey(image).isEmpty(), "images do not generate an audio mix");
	TimelineModel sped = modelFor(clip(1));
	sped.tracks[0].clips[0].srcStartMs = 100;
	sped.tracks[0].clips[0].srcEndMs = 300;
	sped.tracks[0].clips[0].speed = 2.0;
	const auto fastTakes = TimelineAudio::buildTakes(sped, lookup, scratch.path());
	ok(fastTakes.size() == 1 && fastTakes[0].srcStartMs == 50 && fastTakes[0].playMs == 100 &&
		   sampleAt(VoiceoverMixer::renderTakes(fastTakes), 50) > 0.05f,
	   "speed scales the audio source offset and output duration together");

	std::printf("\n-- changes that must invalidate a pending mix --\n");
	TimelineModel fade = modelFor(clip(1));
	fade.tracks[0].clips[0].fadeInMs = 100;
	const QString linearKey = TimelineAudio::mixKey(fade);
	const auto linear = mix(fade);
	fade.tracks[0].clips[0].fadeInCurve = FadeCurve::EaseIn;
	ok(TimelineAudio::mixKey(fade) != linearKey && mix(fade) != linear,
	   "changing the fade-in shape changes both identity and samples");
	const QString inKey = TimelineAudio::mixKey(fade);
	fade.tracks[0].clips[0].fadeOutCurve = FadeCurve::EqualPower;
	ok(TimelineAudio::mixKey(fade) != inKey, "fade-out shape is part of the mix identity too");
	TimelineModel transition = modelFor(clip(1));
	transition.tracks[0].clips.append(clip(2, 50));
	transition.tracks[0].clips[1].transition.enabled = false;
	const QString beforeTransition = TimelineAudio::mixKey(transition);
	const auto unblended = mix(transition);
	transition.tracks[0].clips[1].transition.enabled = true;
	ok(TimelineAudio::mixKey(transition) != beforeTransition && mix(transition) != unblended,
	   "enabling a transition invalidates the old mix and applies the audio crossfade");
	TimelineModel precise = basic;
	precise.tracks[0].clips[0].volume += 0.00001;
	ok(TimelineAudio::mixKey(precise) != TimelineAudio::mixKey(basic),
	   "small gain changes cannot collide through rounded decimal keys");
	const QString sourceKey = TimelineAudio::mixKey(basic, lookup);
	auto repointed = sources;
	repointed[1] = sources[2];
	ok(TimelineAudio::mixKey(basic, [&repointed](int id) { return repointed.value(id); }) != sourceKey,
	   "repointing a source changes the mix identity without changing clip IDs");
	TimelineModel closeSpeeds = modelFor(clip(1));
	closeSpeeds.tracks[0].clips[0].speed = 2.0001;
	TlClip otherSpeed = closeSpeeds.tracks[0].clips[0];
	otherSpeed.speed = 2.0004;
	otherSpeed.outStartMs = 100;
	closeSpeeds.tracks[0].clips.append(otherSpeed);
	const auto differentSpeeds = TimelineAudio::buildTakes(closeSpeeds, lookup, scratch.path());
	ok(differentSpeeds.size() == 2 && differentSpeeds[0].path != differentSpeeds[1].path,
	   "different speeds in one rounding bucket retain separate time-stretched source files");

	std::printf("\n-- one mix must not evict another mix's inputs --\n");
	TimelineModel many = modelFor(clip(1));
	many.tracks[0].clips.clear();
	for (int source = 1; source <= 14; ++source)
		many.tracks[0].clips.append(clip(source, (source - 1) * 120));
	auto held = TimelineAudio::buildTakes(many, lookup, scratch.path());
	ok(held.size() == 14, "one mix can hold more sources than the cache limit");
	const auto expected = VoiceoverMixer::renderTakes(held);
	const auto later = TimelineAudio::buildTakes(modelFor(clip(15)), lookup, scratch.path());
	bool filesPresent = !held.empty();
	for (const auto &take : held)
		filesPresent = filesPresent && QFile::exists(take.path) && bool(take.sourceLease);
	ok(filesPresent, "a second mix leaves every held decoded WAV readable after eviction");
	ok(!expected.empty() && VoiceoverMixer::renderTakes(held) == expected,
	   "the first mix still renders every sample after another mix prunes the cache");
	// Source 1 is the oldest entry of the fourteen-source build and was evicted.
	const QString evicted = held.empty() ? QString() : held.front().path;
	held.clear();
	ok(!evicted.isEmpty() && !QFile::exists(evicted), "an evicted WAV is deleted after its last take releases it");
	ok(later.size() == 1 && !VoiceoverMixer::renderTakes(later).empty(), "the newer mix also remains playable");
	QFile updated(sources[1]);
	ok(updated.open(QIODevice::Append) && updated.write("\0\0", 2) == 2,
	   "the original audio file can be replaced in place");
	updated.close();
	ok(TimelineAudio::mixKey(basic, lookup) != sourceKey,
	   "an in-place source change invalidates a pending mix identity");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
