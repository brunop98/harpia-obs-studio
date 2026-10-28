// An explicit video end cuts the shared preview/export mix without changing
// the audio envelope before that point. Markers themselves are navigation.
#include "editor/TimelineAudio.hpp"

#include <QCoreApplication>
#include <QDataStream>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace harpia;

static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

static bool writeTone(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	constexpr quint32 frames = 96000, dataBytes = frames * 2 * 2;
	QDataStream out(&file);
	out.setByteOrder(QDataStream::LittleEndian);
	out.writeRawData("RIFF", 4);
	out << quint32(36 + dataBytes);
	out.writeRawData("WAVEfmt ", 8);
	out << quint32(16) << quint16(1) << quint16(2) << quint32(48000)
	    << quint32(192000) << quint16(4) << quint16(16);
	out.writeRawData("data", 4);
	out << dataBytes;
	for (quint32 i = 0; i < frames; ++i) {
		const qint16 value = qint16(7000 * std::sin(double(i) * 0.05) + 1000);
		out << value << value;
	}
	return out.status() == QDataStream::Ok;
}

static size_t samplesForMs(qint64 ms)
{
	return size_t(TimelineAudio::frameForMs(ms)) * TimelineAudio::kChannels;
}

static bool hasPrefix(const std::vector<float> &a, const std::vector<float> &b)
{
	return a.size() <= b.size() && std::equal(a.begin(), a.end(), b.begin());
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	QTemporaryDir temporary;
	const QString path = temporary.filePath(QStringLiteral("source.wav"));
	if (!temporary.isValid() || !writeTone(path))
		return 2;
	const auto lookup = [&path](int) { return path; };
	TlClip clip;
	clip.sourceId = 1;
	clip.srcStartMs = 100;
	clip.srcEndMs = 1600;
	clip.outStartMs = 200;
	clip.volume = 0.7;
	clip.fadeInMs = 400;
	clip.fadeOutMs = 600;
	clip.fadeInCurve = FadeCurve::EaseIn;
	clip.fadeOutCurve = FadeCurve::EqualPower;
	TlTrack track;
	track.kind = TlTrack::Kind::Audio;
	track.gain = 0.8;
	track.clips = {clip};
	TimelineModel model;
	model.tracks = {track};
	const auto original = TimelineAudio::mixToBuffer(model, lookup);
	check(original.size() == samplesForMs(1700), "automatic end retains the existing mix length");

	for (qint64 end : {900, 1400}) {
		TimelineModel shortened = model;
		shortened.videoEndMs = end;
		const auto pcm = TimelineAudio::mixToBuffer(shortened, lookup);
		check(pcm.size() == samplesForMs(end), "explicit end bounds the audio sample count");
		check(hasPrefix(pcm, original), "cutting before/during a fade preserves the uncut samples exactly");
		check(shortened.tracks == model.tracks, "audio end never trims or retimes the source clips");
	}
	TimelineModel extended = model;
	extended.videoEndMs = 2200;
	const auto padded = TimelineAudio::mixToBuffer(extended, lookup);
	check(padded.size() == samplesForMs(2200) && hasPrefix(original, padded),
	      "an extended end preserves the whole audio clip");
	check(padded.size() > original.size() &&
		      std::all_of(padded.begin() + original.size(), padded.end(), [](float sample) { return sample == 0; }),
	      "the extended tail is silence");

	TimelineModel withLateSource = model;
	withLateSource.videoEndMs = 900;
	TlClip late = clip;
	late.sourceId = 2;
	late.outStartMs = 100000;
	withLateSource.tracks[0].clips.append(late);
	int lateLookups = 0;
	const auto boundedLookup = [&path, &lateLookups](int id) {
		if (id == 2)
			++lateLookups;
		return path;
	};
	const auto takes = TimelineAudio::buildTakes(withLateSource, boundedLookup, temporary.path());
	check(takes.size() == 1 && lateLookups == 0, "sources placed beyond the video end are never decoded");
	TimelineModel lateOnly = withLateSource;
	lateOnly.tracks[0].clips = {late};
	check(TimelineAudio::mixToBuffer(lateOnly, boundedLookup).empty() && lateLookups == 0,
	      "audio entirely beyond the video end remains silent without allocating a mix");

	const QString originalKey = TimelineAudio::mixKey(model, lookup);
	TimelineModel markers = model;
	markers.markers = {500};
	markers.markerLabels.insert(500, QStringLiteral("Chapter 1"));
	check(TimelineAudio::mixKey(markers, lookup) == originalKey,
	      "ordinary marker positions and labels do not invalidate audio");
	markers.videoEndMs = 900;
	const QString shortKey = TimelineAudio::mixKey(markers, lookup);
	markers.videoEndMs = 1400;
	check(shortKey != originalKey && TimelineAudio::mixKey(markers, lookup) != shortKey,
	      "editing the video end invalidates a pending audio mix");
	std::atomic<bool> canceled{true};
	check(TimelineAudio::mixToBuffer(markers, lookup, &canceled).empty(), "cancellation still returns no mix");
	check(VoiceoverMixer::renderTakes(takes, nullptr, std::numeric_limits<qint64>::max()).empty(),
	      "an unrepresentable imported audio end cannot overflow the output allocation");
	std::printf("%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
