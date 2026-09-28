// Sounds for events: a rule ("every crossfade plays this whoosh") is the
// source of truth and the clips it makes are derived, rebuilt on every
// change. What is pinned here:
//
//   - the event scan finds a transition at the start of its overlap, and a
//     clip start as what the clip IS (a typing caption matches both the
//     caption rule and the component rule);
//   - apply() places one clip per match on a locked Sounds lane at the bottom,
//     clears and re-places on the next call (no duplicates, no strays), takes
//     the lane away when nothing is left, and leaves the user's own audio
//     clips alone;
//   - a clip's shape: the rule's volume, its offset (clamped at 0), its cut
//     length, no crossfade with its neighbours;
//   - freeze() hands the clips to the user and removes the rules;
//   - the JSON round trip keeps a rule and the Sounds flag;
//   - the built-in sounds are real: non-silent, short, and written as a WAV
//     that reads back with the right length.
#include "editor/timeline/SoundRules.hpp"
#include "editor/timeline/TimelineJson.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void eqi(qint64 got, qint64 want, const char *w)
{
	const bool good = got == want;
	std::printf("  %s %s (got %lld, want %lld)\n", good ? "PASS" : "FAIL", w, (long long)got,
		    (long long)want);
	if (!good)
		++failures;
}

static TlClip media(int src, qint64 at, qint64 len)
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = src;
	c.srcStartMs = 0;
	c.srcEndMs = len;
	c.outStartMs = at;
	return c;
}
static TlClip still(qint64 at, qint64 len)
{
	TlClip c = media(0, at, len);
	c.type = TlClip::Type::Image;
	return c;
}
static TlClip caption(qint64 at, qint64 len, bool typing)
{
	TlClip c = media(0, at, len);
	c.type = TlClip::Type::Text;
	if (typing) {
		ComponentInstance ci;
		ci.typeId = QStringLiteral("harpia.textType");
		ci.instanceId = QStringLiteral("t1");
		c.components.append(ci);
	}
	return c;
}

// A sound library: source 50 is 400 ms, source 51 is 1500 ms, 52 unknown.
static SoundInfo lookup(int src)
{
	SoundInfo s;
	if (src == 50)
		s.durationMs = 400;
	else if (src == 51)
		s.durationMs = 1500;
	s.peaks = {0.5f, 0.2f};
	return s;
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);

	// V1: media 0-5000, media 4000-9000 (crossfade over 4000-5000), still at
	// 9000-11000, plain caption at 2000, typing caption at 6000.
	TimelineModel m;
	TlTrack v;
	v.kind = TlTrack::Kind::Video;
	v.clips << media(1, 0, 5000) << media(2, 4000, 5000) << still(9000, 2000);
	v.clips[1].transition.enabled = true;
	v.clips[1].transition.type = TransitionType::WipeLeft;
	TlTrack v2;
	v2.kind = TlTrack::Kind::Video;
	v2.clips << caption(2000, 1000, false) << caption(6000, 1000, true);
	TlTrack a;
	a.kind = TlTrack::Kind::Audio;
	a.clips << media(7, 0, 3000); // the user's own music
	m.tracks << v2 << v << a;

	std::printf("\n-- the events --\n");
	{
		const QVector<SoundEvent> ev = SoundRules::events(m);
		// 5 clip starts + 1 transition.
		eqi(ev.size(), 6, "every picture clip is a start, plus one transition");
		int trans = 0;
		qint64 transAt = -1;
		int type = -1;
		for (const SoundEvent &e : ev)
			if (e.kind == TlSoundRule::Trigger::Transition) {
				++trans;
				transAt = e.atMs;
				type = e.transitionType;
			}
		eqi(trans, 1, "one transition");
		eqi(transAt, 4000, "at the start of the overlap");
		eqi(type, int(TransitionType::WipeLeft), "with its type");
		ok(ev.first().atMs <= ev.last().atMs, "in time order");
		bool typingHasComp = false;
		for (const SoundEvent &e : ev)
			if (e.atMs == 6000 && e.components.contains(QStringLiteral("harpia.textType")))
				typingHasComp = true;
		ok(typingHasComp, "a typing caption's start carries its component");
		ok(SoundRules::events(TimelineModel()).isEmpty(), "an empty timeline has no events");
	}

	std::printf("\n-- matching --\n");
	{
		const QVector<SoundEvent> ev = SoundRules::events(m);
		auto count = [&](const TlSoundRule &r) {
			int n = 0;
			for (const SoundEvent &e : ev)
				if (SoundRules::matches(r, e))
					++n;
			return n;
		};
		TlSoundRule r;
		r.id = 1;
		r.sourceId = 50;
		r.trigger = TlSoundRule::Trigger::AnyTransition;
		eqi(count(r), 1, "any transition: the crossfade");
		r.trigger = TlSoundRule::Trigger::Transition;
		r.transitionType = int(TransitionType::WipeLeft);
		eqi(count(r), 1, "this transition type: matches");
		r.transitionType = int(TransitionType::Crossfade);
		eqi(count(r), 0, "another type: does not");
		r.trigger = TlSoundRule::Trigger::ImageAppears;
		eqi(count(r), 1, "image appears: the still");
		r.trigger = TlSoundRule::Trigger::TextAppears;
		eqi(count(r), 2, "caption appears: both captions, typing included");
		r.trigger = TlSoundRule::Trigger::Component;
		r.componentId = QStringLiteral("harpia.textType");
		eqi(count(r), 1, "the typing component: the typing caption only");
		r.trigger = TlSoundRule::Trigger::VideoStarts;
		eqi(count(r), 2, "video starts: the two media clips, not the music on the audio lane");
		r.trigger = TlSoundRule::Trigger::AnyClipStarts;
		eqi(count(r), 5, "any clip: every picture clip");
		r.lane = 0;
		eqi(count(r), 2, "narrowed to a lane: only that lane's clips");
		r.lane = -1;
		r.enabled = false;
		eqi(count(r), 0, "a disabled rule matches nothing");
		r.enabled = true;
		r.sourceId = 0;
		eqi(count(r), 0, "nor one with no sound");
	}

	std::printf("\n-- applying --\n");
	{
		TimelineModel w = m;
		TlSoundRule whoosh;
		whoosh.id = w.nextSoundRuleId();
		whoosh.trigger = TlSoundRule::Trigger::AnyTransition;
		whoosh.sourceId = 51;
		whoosh.volume = 0.6;
		whoosh.offsetMs = -100;
		whoosh.maxMs = 800;
		w.soundRules.append(whoosh);
		TlSoundRule click;
		click.id = w.nextSoundRuleId();
		click.trigger = TlSoundRule::Trigger::ImageAppears;
		click.sourceId = 50;
		w.soundRules.append(click);
		eqi(click.id, 2, "rule ids count up");

		ok(SoundRules::apply(w, lookup), "apply changes a timeline with matching rules");
		const int lane = w.soundsLane();
		ok(lane == w.tracks.size() - 1, "the Sounds lane is at the bottom");
		ok(lane >= 0 && w.tracks[lane].kind == TlTrack::Kind::Audio && w.tracks[lane].locked,
		   "and is a locked audio lane");
		ok(lane >= 0 && w.tracks[lane].name == QLatin1String("Sounds"), "named Sounds");
		eqi(lane >= 0 ? w.tracks[lane].clips.size() : -1, 2, "one clip per match");
		if (lane >= 0 && w.tracks[lane].clips.size() == 2) {
			const TlClip &s0 = w.tracks[lane].clips[0];
			const TlClip &s1 = w.tracks[lane].clips[1];
			eqi(s0.outStartMs, 3900, "the whoosh starts 100 ms before the overlap");
			eqi(s0.outDurationMs(), 800, "and is cut at the rule's length");
			ok(std::abs(s0.volume - 0.6) < 1e-9, "at the rule's volume");
			ok(s0.soundRule == whoosh.id && s1.soundRule == click.id, "each marked with its rule");
			ok(!s0.transition.enabled, "no crossfade between sounds");
			eqi(s1.outStartMs, 9000, "the click is at the still");
			eqi(s1.outDurationMs(), 400, "and plays whole");
			ok(!s1.peaks.isEmpty(), "with a waveform");
		}
		eqi(w.tracks[2].clips.size(), 1, "the user's music is untouched");

		// Again: nothing changes, nothing doubles.
		ok(!SoundRules::apply(w, lookup), "a second apply is a no-op");
		eqi(w.tracks[w.soundsLane()].clips.size(), 2, "still two");

		// Move the still: the click follows.
		w.tracks[1].clips[2].outStartMs = 9500;
		SoundRules::apply(w, lookup);
		eqi(w.tracks[w.soundsLane()].clips[1].outStartMs, 9500, "moving the still moves its click");

		// Add a second still: a second click, without touching anything else.
		w.tracks[1].clips << still(12000, 500);
		SoundRules::apply(w, lookup);
		eqi(w.tracks[w.soundsLane()].clips.size(), 3, "a new still gets its click at once");

		// Offset clamps at zero.
		w.tracks[1].clips[0].transition.enabled = true; // no overlap: no event
		TlSoundRule early = click;
		early.id = w.nextSoundRuleId();
		early.offsetMs = -20000;
		w.soundRules.append(early);
		SoundRules::apply(w, lookup);
		bool anyNeg = false, anyZero = false;
		for (const TlClip &c : w.tracks[w.soundsLane()].clips) {
			if (c.outStartMs < 0)
				anyNeg = true;
			if (c.outStartMs == 0 && c.soundRule == early.id)
				anyZero = true;
		}
		ok(!anyNeg && anyZero, "an offset before the start clamps at 0");
		w.soundRules.removeLast();

		// An unknown sound places nothing.
		TlSoundRule ghost = click;
		ghost.id = w.nextSoundRuleId();
		ghost.sourceId = 52;
		w.soundRules.append(ghost);
		SoundRules::apply(w, lookup);
		bool ghostClip = false;
		for (const TlClip &c : w.tracks[w.soundsLane()].clips)
			if (c.soundRule == ghost.id)
				ghostClip = true;
		ok(!ghostClip, "a rule whose sound cannot be found places nothing");
		w.soundRules.removeLast();

		// Disable, then remove: the lane goes when it would be empty.
		for (TlSoundRule &r : w.soundRules)
			r.enabled = false;
		SoundRules::apply(w, lookup);
		ok(w.soundsLane() < 0, "with every rule off the empty Sounds lane is taken away");
		for (TlSoundRule &r : w.soundRules)
			r.enabled = true;
		SoundRules::apply(w, lookup);
		ok(w.soundsLane() >= 0, "and comes back when a rule is on again");

		// A renamed Sounds lane is the user's: it stays.
		w.tracks[w.soundsLane()].name = QStringLiteral("SFX");
		w.soundRules.clear();
		SoundRules::apply(w, lookup);
		ok(w.soundsLane() >= 0 && w.tracks[w.soundsLane()].clips.isEmpty(),
		   "a renamed Sounds lane stays, empty, when the rules go");
	}

	std::printf("\n-- freezing --\n");
	{
		TimelineModel w = m;
		TlSoundRule click;
		click.id = 1;
		click.trigger = TlSoundRule::Trigger::ImageAppears;
		click.sourceId = 50;
		w.soundRules.append(click);
		SoundRules::apply(w, lookup);
		const int lane = w.soundsLane();
		SoundRules::freeze(w);
		ok(w.soundRules.isEmpty(), "freeze removes the rules");
		ok(lane >= 0 && !w.tracks[lane].autoSounds && !w.tracks[lane].locked,
		   "the lane is a normal, unlocked audio lane");
		ok(lane >= 0 && w.tracks[lane].clips.size() == 1 && w.tracks[lane].clips[0].soundRule == 0,
		   "its clip is the user's now");
		ok(!SoundRules::apply(w, lookup), "and the next apply leaves it alone");
		eqi(w.tracks[lane].clips.size(), 1, "one clip, kept");
	}

	std::printf("\n-- the file --\n");
	{
		TimelineModel w = m;
		TlSoundRule r;
		r.id = 3;
		r.trigger = TlSoundRule::Trigger::Transition;
		r.transitionType = int(TransitionType::Push);
		r.sourceId = 51;
		r.soundName = QStringLiteral("Whoosh");
		r.volume = 0.7;
		r.offsetMs = -50;
		r.maxMs = 900;
		r.lane = 1;
		r.enabled = false;
		const TlSoundRule back = soundRuleFromJson(soundRuleToJson(r));
		ok(back == r, "a rule survives the JSON round trip");
		TlTrack t;
		t.kind = TlTrack::Kind::Audio;
		t.autoSounds = true;
		t.locked = true;
		TlClip c = media(51, 100, 200);
		c.soundRule = 3;
		t.clips << c;
		const TlTrack tb = trackFromJson(trackToJson(t));
		ok(tb.autoSounds && tb.clips.size() == 1 && tb.clips[0].soundRule == 3,
		   "the Sounds flag and a clip's rule id are written and read");
		ok(t == tb, "the track compares equal after the trip");
		TlTrack plain;
		plain.kind = TlTrack::Kind::Audio;
		ok(!trackToJson(plain).contains(QStringLiteral("autoSounds")), "a plain lane writes no flag");
	}

	std::printf("\n-- labels --\n");
	{
		TlSoundRule r;
		r.trigger = TlSoundRule::Trigger::Transition;
		r.transitionType = int(TransitionType::Crossfade);
		ok(SoundRules::triggerLabel(r).toLower().contains(QLatin1String("crossfade")),
		   "a transition rule names its type");
		r.trigger = TlSoundRule::Trigger::Component;
		r.componentId = QStringLiteral("harpia.textType");
		ok(SoundRules::triggerLabel(r).contains(QLatin1String("typing")), "the typing component reads as typing");
		for (int i = 0; i < int(TlSoundRule::Trigger::Count); ++i)
			ok(!SoundRules::triggerLabel(TlSoundRule::Trigger(i)).isEmpty(), "every trigger has a label");
		ok(SoundRules::ruleForClip(caption(0, 100, true), true).trigger == TlSoundRule::Trigger::Component,
		   "a typing caption's natural rule is the component one");
		ok(SoundRules::ruleForClip(caption(0, 100, false), true).trigger == TlSoundRule::Trigger::TextAppears,
		   "a plain caption's is the caption one");
		ok(SoundRules::ruleForClip(still(0, 100), true).trigger == TlSoundRule::Trigger::ImageAppears,
		   "a still's is the image one");
		ok(SoundRules::ruleForClip(media(1, 0, 100), true).trigger == TlSoundRule::Trigger::VideoStarts,
		   "a media clip's is video starts");
		TlClip in = media(2, 0, 100);
		in.transition.type = TransitionType::Zoom;
		const TlSoundRule tr = SoundRules::ruleForTransition(in, true);
		ok(tr.trigger == TlSoundRule::Trigger::Transition && tr.transitionType == int(TransitionType::Zoom),
		   "a transition's rule carries its type");
		ok(SoundRules::ruleForTransition(in, false).trigger == TlSoundRule::Trigger::AnyTransition,
		   "or covers every transition");
	}

	std::printf("\n-- tags --\n");
	{
		TimelineModel w = m;
		const int intro = w.ensureTag(QStringLiteral("intro"));
		const int callout = w.ensureTag(QStringLiteral("Callout"), QColor(Qt::red));
		eqi(intro, 1, "the first tag is 1");
		eqi(w.ensureTag(QStringLiteral("  INTRO ")), intro, "the same name, any case, is the same tag");
		eqi(w.ensureTag(QString()), 0, "an empty name makes no tag");
		eqi(w.tags.size(), 2, "two tags");
		ok(w.tag(callout) && w.tag(callout)->color == QColor(Qt::red), "a tag keeps its colour");
		ok(w.tagName(intro) == QLatin1String("intro"), "and its name");

		// Tag the still, a caption and the user's music.
		w.tracks[1].clips[2].setTag(intro, true);
		w.tracks[1].clips[2].setTag(intro, true);
		eqi(w.tracks[1].clips[2].tags.size(), 1, "tagging twice is still one tag");
		w.tracks[0].clips[0].setTag(intro, true);
		w.tracks[2].clips[0].setTag(intro, true);
		w.tracks[0].clips[1].setTag(callout, true);

		TlSoundRule r = SoundRules::ruleForTag(intro);
		r.id = 1;
		r.sourceId = 50;
		w.soundRules.append(r);
		SoundRules::apply(w, lookup);
		const int lane = w.soundsLane();
		int n = 0;
		QVector<qint64> at;
		for (const TlClip &c : w.tracks[lane].clips)
			if (c.soundRule == 1) {
				++n;
				at << c.outStartMs;
			}
		eqi(n, 3, "a tag rule sounds every tagged clip, audio lanes included");
		ok(at.contains(9000) && at.contains(2000) && at.contains(0), "at each clip's start");

		// Other rules still ignore audio lanes.
		TlSoundRule any;
		any.id = 2;
		any.trigger = TlSoundRule::Trigger::AnyClipStarts;
		any.sourceId = 50;
		w.soundRules.append(any);
		SoundRules::apply(w, lookup);
		int anyN = 0;
		for (const TlClip &c : w.tracks[w.soundsLane()].clips)
			if (c.soundRule == 2)
				++anyN;
		eqi(anyN, 5, "\"every clip starts\" still means the picture lanes");
		w.soundRules.removeLast();

		// Untag: the sound goes.
		w.tracks[1].clips[2].setTag(intro, false);
		SoundRules::apply(w, lookup);
		n = 0;
		for (const TlClip &c : w.tracks[w.soundsLane()].clips)
			if (c.soundRule == 1)
				++n;
		eqi(n, 2, "untagging a clip takes its sound away");

		ok(SoundRules::triggerLabel(r, &w) == QLatin1String("Every clip tagged intro"),
		   "the rule reads with the tag's name");

		// Removing the tag removes it from clips and its rules.
		w.removeTag(intro);
		ok(!w.tag(intro) && w.soundRules.isEmpty(), "removing a tag removes its rules");
		bool anyLeft = false;
		for (const TlTrack &t : w.tracks)
			for (const TlClip &c : t.clips)
				if (c.hasTag(intro))
					anyLeft = true;
		ok(!anyLeft, "and it is gone from every clip");
		ok(w.tracks[0].clips[1].hasTag(callout), "other tags stay");

		// The file.
		const TlTag tb = tagFromJson(tagToJson(*w.tag(callout)));
		ok(tb == *w.tag(callout), "a tag survives the JSON round trip");
		const TlClip cb = clipFromJson(clipToJson(w.tracks[0].clips[1]));
		ok(cb.tags == QVector<int>{callout}, "a clip's tags are written and read");
		ok(!clipToJson(w.tracks[1].clips[0]).contains(QStringLiteral("tags")), "an untagged clip writes none");
		TlSoundRule tr = SoundRules::ruleForTag(callout);
		tr.id = 9;
		ok(soundRuleFromJson(soundRuleToJson(tr)) == tr, "a tag rule survives the trip");
		ok(!(w == m), "tags are part of the model's equality");
	}

	std::printf("\n-- the Sound component --\n");
	{
		TimelineModel w = m;
		TlClip &st = w.tracks[1].clips[2]; // the still, 9000-11000
		ComponentInstance snd;
		snd.typeId = QString::fromLatin1(kSoundComponentId);
		snd.instanceId = QStringLiteral("snd");
		snd.props.insert(QStringLiteral("inSound"), 50);
		snd.props.insert(QStringLiteral("inOffsetMs"), -200);
		snd.props.insert(QStringLiteral("inVolume"), 50);
		st.components.append(snd);
		ok(SoundRules::apply(w, lookup), "a Sound component places a sound with no rules at all");
		const int lane = w.soundsLane();
		ok(lane >= 0 && w.tracks[lane].clips.size() == 1, "one sound on the Sounds lane");
		if (lane >= 0 && w.tracks[lane].clips.size() == 1) {
			const TlClip &c = w.tracks[lane].clips[0];
			eqi(c.outStartMs, 8800, "at the clip's start plus the offset");
			ok(std::abs(c.volume - 0.5) < 1e-9, "at the component's volume");
			ok(c.soundRule == kSoundFromComponent, "marked as a component's sound");
		}
		// Out sound, at the end.
		ComponentInstance &sc = w.tracks[1].clips[2].components.last();
		sc.props.insert(QStringLiteral("outOn"), true);
		sc.props.insert(QStringLiteral("outSound"), 51);
		sc.props.insert(QStringLiteral("outOffsetMs"), 100);
		SoundRules::apply(w, lookup);
		eqi(w.tracks[w.soundsLane()].clips.size(), 2, "Out sound on: two sounds");
		eqi(w.tracks[w.soundsLane()].clips[1].outStartMs, 11100, "the Out sound at the end plus its offset");

		// Match Slide timing.
		ComponentInstance slide;
		slide.typeId = QStringLiteral("harpia.slide");
		slide.instanceId = QStringLiteral("sl");
		slide.props.insert(QStringLiteral("durationMs"), 400);
		slide.props.insert(QStringLiteral("slideOut"), true);
		w.tracks[1].clips[2].components.append(slide);
		SoundRules::apply(w, lookup);
		eqi(w.tracks[w.soundsLane()].clips[1].outStartMs, 10700,
		    "with a Slide out, the Out sound starts where the slide out does (plus offset)");
		w.tracks[1].clips[2].components[0].props.insert(QStringLiteral("alignSlide"), false);
		SoundRules::apply(w, lookup);
		eqi(w.tracks[w.soundsLane()].clips[1].outStartMs, 11100, "Match Slide timing off: back to the end");

		// Moving the clip moves both.
		w.tracks[1].clips[2].outStartMs = 12000;
		SoundRules::apply(w, lookup);
		eqi(w.tracks[w.soundsLane()].clips[0].outStartMs, 11800, "moving the clip moves its In sound");
		ok(!SoundRules::apply(w, lookup), "and applying again changes nothing");

		// Disabled: gone.
		w.tracks[1].clips[2].components[0].enabled = false;
		SoundRules::apply(w, lookup);
		ok(w.soundsLane() < 0, "a disabled Sound component places nothing (and the lane goes)");
		w.tracks[1].clips[2].components[0].enabled = true;

		// A component's sound never triggers a tag rule or another component.
		SoundRules::apply(w, lookup);
		const QVector<SoundEvent> ev = SoundRules::events(w);
		bool derived = false;
		for (const SoundEvent &e : ev)
			if (e.track == w.soundsLane())
				derived = true;
		ok(!derived, "the Sounds lane is never an event source");

		// Freeze: the sounds stay as normal clips and the component goes.
		SoundRules::freeze(w);
		int sounds = 0;
		for (const TlTrack &t : w.tracks)
			for (const TlClip &c : t.clips)
				if (c.sourceId == 50 || c.sourceId == 51)
					++sounds;
		eqi(sounds, 2, "freeze keeps both sounds as clips");
		bool compLeft = false;
		for (const ComponentInstance &ci : w.tracks[1].clips[2].components)
			if (ci.typeId == QLatin1String(kSoundComponentId))
				compLeft = true;
		ok(!compLeft, "and removes the Sound component so they are not placed twice");
		ok(!SoundRules::apply(w, lookup), "nothing is placed again after freezing");

		ok(clipFromJson(clipToJson(w.tracks[w.tracks.size() - 1].clips.value(0))).soundRule == 0,
		   "a frozen clip saves as the user's");
		TlClip marked;
		marked.soundRule = kSoundFromComponent;
		ok(clipFromJson(clipToJson(marked)).soundRule == kSoundFromComponent, "the component mark is saved");
	}

	std::printf("\n-- the built-in sounds --\n");
	{
		QTemporaryDir dir;
		for (const QString &name : SoundRules::builtinSounds()) {
			const std::vector<float> pcm = SoundRules::synthesize(name);
			float peak = 0.f;
			for (float v : pcm)
				peak = std::max(peak, std::abs(v));
			ok(!pcm.empty() && pcm.size() <= 48000 * 2 && peak > 0.1f && peak <= 1.0f,
			   qPrintable(QStringLiteral("%1: short, audible, in range").arg(name)));
			const QString wav = dir.filePath(name + QStringLiteral(".wav"));
			ok(SoundRules::writeBuiltin(name, wav), qPrintable(QStringLiteral("%1 writes a WAV").arg(name)));
			eqi(QFileInfo(wav).size(), qint64(44 + pcm.size() * 2), "of the right size");
		}
		ok(SoundRules::synthesize(QStringLiteral("nosuch")).empty(), "an unknown name makes nothing");
		ok(SoundRules::synthesize(QStringLiteral("Pop")) == SoundRules::synthesize(QStringLiteral("pop")),
		   "the same name always makes the same sound");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
