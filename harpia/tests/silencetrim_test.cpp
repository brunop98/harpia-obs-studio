#include "editor/SilenceTrim.hpp"
#include "editor/component/TextSubtitle.hpp"

#include <QCoreApplication>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace harpia;
using namespace harpia::silence_trim;

static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

static std::vector<float> pcm(int durationMs, int rate = 48000, int channels = 2)
{
	return std::vector<float>(size_t(durationMs) * rate / 1000 * channels, 0.0f);
}

static void sound(std::vector<float> &samples, int fromMs, int toMs, float amplitude = 0.2f,
		  int channels = 2, bool opposite = false)
{
	for (int frame = fromMs * 48; frame < toMs * 48; ++frame)
		for (int channel = 0; channel < channels; ++channel)
			samples[size_t(frame) * channels + channel] =
				amplitude * std::sin(2.0 * 3.141592653589793 * frame * 440.0 / 48000.0) *
				(opposite && channel == 1 ? -1.0 : 1.0);
}

static TlClip media(qint64 start, qint64 from, qint64 to, double speed = 1.0)
{
	TlClip clip;
	clip.outStartMs = start;
	clip.srcStartMs = from;
	clip.srcEndMs = to;
	clip.speed = speed;
	clip.sourceId = 1;
	return clip;
}

static TimelineModel fixture()
{
	TimelineModel model;
	TlTrack video;
	video.name = QStringLiteral("Camera");
	TlClip clip = media(0, 1000, 13000, 2.0);
	clip.link = 17;
	clip.tags = {9};
	clip.clipOptions = {{1, 1000}, {2, 5000}};
	clip.setKeyframeAt(0, {0.2, 0.4, 1.0, 0.0, 1.0});
	clip.setKeyframeAt(6000, {0.8, 0.7, 2.0, 30.0, 0.9});
	ComponentInstance transform;
	transform.typeId = QStringLiteral("harpia.transform");
	transform.instanceId = QStringLiteral("pose");
	transform.keys[QStringLiteral("rotation")] = {{0, 0.0}, {6000, 90.0}};
	clip.components = {transform};
	video.clips = {clip};
	model.tracks.append(video);
	TlTrack audio;
	audio.kind = TlTrack::Kind::Audio;
	audio.name = QStringLiteral("Voice");
	TlClip voice = media(0, 500, 6500);
	voice.link = 17;
	audio.clips = {voice};
	model.tracks.append(audio);
	TlTrack captions;
	captions.name = QStringLiteral("Captions");
	TlClip text = media(500, 0, 4000);
	text.type = TlClip::Type::Text;
	text.text.text = QStringLiteral("First <b>second</b>");
	text.text.variations = {QStringLiteral("Another caption")};
	text.text.random = false;
	text.link = 17;
	text.words = {{QStringLiteral("First"), 0, 1000}, {QStringLiteral("second"), 2500, 4000}};
	ComponentInstance subtitle;
	subtitle.typeId = QStringLiteral("harpia.subtitle");
	subtitle.instanceId = QStringLiteral("words");
	subtitle.props[QStringLiteral("mode")] = 3;
	text.components = {subtitle};
	TlClip image = media(1000, 0, 4000);
	image.type = TlClip::Type::Image;
	image.sourceId = 3;
	image.transition.enabled = false;
	captions.clips = {text, image};
	model.tracks.append(captions);
	TlTrack effects;
	effects.kind = TlTrack::Kind::Effect;
	TlClip effect = media(0, 0, 6000);
	effect.type = TlClip::Type::Effect;
	effect.fx.type = FxType::InverseSelection;
	effect.fx.keys = {{0, {{QStringLiteral("dim"), 0.1}}}, {6000, {{QStringLiteral("dim"), 0.8}}}};
	SpotMask mask;
	mask.keys = {{0, {}}, {6000, {}}};
	effect.fx.spot.masks = {mask};
	effects.clips = {effect};
	model.tracks.append(effects);
	model.markers = {500, 2500, 3000, 5500};
	model.tags = {{9, QStringLiteral("Narration"), QColor(Qt::cyan)}};
	TlSoundRule rule;
	rule.id = 3;
	rule.enabled = false;
	model.soundRules = {rule};
	return model;
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	{
		auto samples = pcm(4000);
		sound(samples, 0, 1000);
		sound(samples, 2000, 4000);
		check(detect(samples, 48000, 2) == QVector<TlSpan>{{1075, 1925}},
		      "a one-second pause keeps 150 milliseconds of breathing room");
		Options longer;
		longer.minSilenceMs = 1200;
		check(detect(samples, 48000, 2, longer).isEmpty(), "shorter pauses survive the minimum-pause setting");
		longer.minSilenceMs = 500;
		longer.keepPauseMs = 1000;
		check(detect(samples, 48000, 2, longer).isEmpty(), "a pause shorter than the requested retained room is kept");
		check(detectRange(samples, 48000, 2, 500, 2500) == QVector<TlSpan>{{1075, 1925}},
		      "range analysis keeps absolute timeline positions without copying PCM");
		check(detectRange(samples, 48000, 2, 1000, 2500).isEmpty(),
		      "a selection's leading quiet region is preserved by default");
		std::atomic<bool> stop{true};
		check(detect(samples, 48000, 2, {}, &stop).isEmpty(), "canceled analysis returns no proposed cuts");
	}
	{
		auto samples = pcm(2000);
		sound(samples, 500, 1500);
		check(detect(samples, 48000, 2).isEmpty(), "leading and trailing silence are preserved by default");
		Options options;
		options.includeEdges = true;
		check(detect(samples, 48000, 2, options) == QVector<TlSpan>{{0, 350}, {1650, 2000}},
		      "edge trimming retains quiet room next to the audible content");
		check(detect(pcm(3000), 48000, 2, options).isEmpty(), "an entirely silent recording is never erased");
		check(detect({}, 48000, 2).isEmpty() && detect(samples, 0, 2).isEmpty() &&
		      detect(std::vector<float>{0, 0, 0}, 48000, 2).isEmpty(),
		      "missing or malformed PCM does not propose destructive edits");
	}
	{
		auto samples = pcm(2000);
		sound(samples, 0, 2000, 0.2f, 2, true);
		check(detect(samples, 48000, 2).isEmpty(), "opposite-polarity stereo cannot cancel into false silence");
		for (size_t frame = 0; frame < samples.size() / 2; ++frame)
			samples[frame * 2] = 0;
		check(detect(samples, 48000, 2).isEmpty(), "audio in either channel protects the recording");
	}
	{
		auto samples = pcm(3500);
		sound(samples, 0, 1000);
		sound(samples, 2500, 3500);
		samples[size_t(1700) * 48 * 2] = 0.08f;
		const auto cuts = detect(samples, 48000, 2);
		bool removesTransient = false;
		for (const auto &cut : cuts)
			removesTransient = removesTransient || (cut.fromMs <= 1700 && cut.toMs > 1700);
		check(cuts.size() == 2 && !removesTransient, "the peak guard preserves a brief transient whose window RMS is quiet");
	}
	{
		const auto ranges = normalize({{3000, 5000}, {1000, 2000}, {1800, 3000}, {-100, 100}, {9000, 12000}}, 10000);
		check(ranges == QVector<TlSpan>{{0, 100}, {1000, 5000}, {9000, 10000}}, "candidate ranges are clamped, sorted and unioned");
		check(mapTime(2500, ranges) == 900 && mapTime(6000, ranges) == 1900,
		      "playhead positions inside and after removed pauses map to the edited timeline");
	}
	{
		const TimelineModel original = fixture();
		const EditResult edit = apply(original, {{2000, 3000}});
		if (!edit.error.isEmpty())
			std::printf("    %s\n", qPrintable(edit.error));
		check(edit.ok() && edit.removedMs == 1000 && edit.model.durationMs() == 5000,
		      "removing a pause shortens the whole timeline by the reviewed amount");
		if (!edit.ok())
			return 1;
		check(edit.model.tags == original.tags && edit.model.soundRules == original.soundRules &&
		      edit.model.tracks.size() == original.tracks.size(), "track settings, tags and sound-rule metadata are preserved");
		check(edit.model.markers == QVector<qint64>{500, 2000, 4500}, "markers inside cuts are removed and later markers follow the ripple");
		const auto &video = edit.model.tracks[0].clips;
		const auto &audio = edit.model.tracks[1].clips;
		check(video.size() == 2 && audio.size() == 2 && video[1].outStartMs == 2000 && audio[1].outStartMs == 2000,
		      "video and audio split at the same output times and remain synchronized");
		check(video[0].srcStartMs == 1000 && video[0].srcEndMs == 5000 &&
		      video[1].srcStartMs == 7000 && video[1].srcEndMs == 13000 && video[1].speed == 2.0,
		      "source in-points and source length respect the clip's playback speed");
		check(video[1].clipOptions[0].srcStartMs == 7000 && video[1].clipOptions[1].srcStartMs == 11000,
		      "every Random clip alternative advances by the same source-time offset");
		check(video[1].link == 17 && audio[1].link == 17 && video[1].tags == original.tracks[0].clips[0].tags,
		      "links and tags stay attached to all surviving pieces");
		const TlTransform pose = video[1].transformAt(4000);
		const TlTransform beforePose = original.tracks[0].clips[0].transformAt(5000);
		check(video[1].keys.first().tMs == -3000 && video[1].components[0].keys[QStringLiteral("rotation")].first().tMs == -3000 &&
		      pose.posX == beforePose.posX && pose.posY == beforePose.posY && pose.scale == beforePose.scale &&
		      pose.rotation == beforePose.rotation && pose.opacity == beforePose.opacity,
		      "transform and component keys retain their original animation phase");
		const auto &text = edit.model.tracks[2].clips[1];
		check(text.type == TlClip::Type::Text && text.text == original.tracks[2].clips[0].text &&
		      text.words[0].startMs == -2500 && text.words[1].startMs == 0,
		      "caption text and alternatives survive while spoken word times follow the surviving span");
		check(edit.model.tracks[2].clips.size() == 4 && edit.model.tracks[2].clips[3].type == TlClip::Type::Image,
		      "stills use the same global cuts as sound, captions and footage");
		const auto &effect = edit.model.tracks[3].clips[1];
		check(effect.fx.keys[0].tMs == -3000 && effect.fx.spot.masks[0].keys[0].tMs == -3000,
		      "legacy effect and spotlight animation keys preserve their phase too");
		check(video[0].fadeOutMs == 0 && video[1].fadeInMs == 0 &&
		      video[0].fadeInMs == original.tracks[0].clips[0].fadeInMs &&
		      video[1].fadeOutMs == original.tracks[0].clips[0].fadeOutMs,
		      "new joins do not restart audio fades and original outer fades are kept");
		check(original == fixture(), "the original project model remains untouched");
	}
	{
		TimelineModel model = fixture();
		model.tracks[2].clips[0].words.clear();
		const auto edit = apply(model, {{2000, 3000}});
		check(edit.ok() && edit.model.tracks[2].clips[1].words ==
		      QVector<ClipWordTime>{{QStringLiteral("First"), -2500, -500}, {QStringLiteral("second"), -500, 1500}},
		      "untimed animated captions materialize their original pacing before they are split");
	}
	{
		TimelineModel model = fixture();
		model.tracks[1].locked = true;
		const auto edit = apply(model, {{2000, 3000}});
		check(!edit.ok() && edit.error.contains(QStringLiteral("locked")) && edit.model == model,
		      "an affected locked audio lane blocks the edit instead of breaking synchronization");
		model.tracks[1].clips = {media(0, 0, 1000)};
		check(apply(model, {{2000, 3000}}).ok(), "locked content before every cut can remain untouched");
	}
	{
		TimelineModel model;
		TlTrack track;
		track.clips = {media(0, 0, 3000), media(2500, 0, 2500)};
		model.tracks = {track};
		const auto edit = apply(model, {{2600, 2700}});
		check(!edit.ok() && edit.error.contains(QStringLiteral("transition")) && edit.model == model,
		      "a cut through a transition is explained rather than silently restarting it");
	}
	{
		TimelineModel model = fixture();
		model.tracks[0].clips[0].scripts = {{QStringLiteral("motion"), {}}};
		check(!apply(model, {{2000, 3000}}).ok(), "script phases that cannot be represented after a split are protected");
		model = fixture();
		ComponentInstance motion;
		motion.typeId = QStringLiteral("harpia.alwaysGrow");
		model.tracks[0].clips[0].components.append(motion);
		check(!apply(model, {{2000, 3000}}).ok(), "duration-driven motion is not silently retimed");
		model = fixture();
		model.soundRules[0].enabled = true;
		check(!apply(model, {{2000, 3000}}).ok(), "split clips cannot accidentally trigger extra automatic sound effects");
		model = fixture();
		model.tracks[2].clips[0].text.random = true;
		check(!apply(model, {{2000, 3000}}).ok(), "animated Random text alternatives cannot acquire new word pacing silently");
		model = fixture();
		model.tracks[1].clips[0].tts = QStringLiteral("{\"text\":\"Generated narration\"}");
		const auto generated = apply(model, {{2000, 3000}});
		check(!generated.ok() && generated.model == model && generated.error.contains(QStringLiteral("Re-speak")),
		      "trimming cannot split editable narration and corrupt later sentence edits");
		model = fixture();
		model.tracks[0].clips[0].fadeInMs = 2500;
		check(!apply(model, {{2000, 3000}}).ok(), "cutting partway into an audio fade is diagnosed");
		model = fixture();
		check(!apply(model, {{0, 100000}}).ok(), "a reviewed selection still cannot erase the whole project");
		check(apply(model, {}).model == model && apply(model, {{100000, 200000}}).model == model,
		      "empty and out-of-range selections are harmless no-ops");
	}
	{
		TimelineModel model;
		TlTrack track;
		track.clips = {media(0, 1000, 2800, 0.3)};
		model.tracks = {track};
		check(apply(model, {{2000, 3000}}).ok(), "representable fractional playback speeds keep exact timing");
		const auto edit = apply(model, {{2001, 3000}});
		check(!edit.ok() && edit.model == model, "an unrepresentable fractional-speed boundary cannot introduce cumulative A/V drift");
	}
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
