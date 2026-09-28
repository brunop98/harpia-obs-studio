#pragma once

// Sounds for events: the engine behind TlSoundRule.
//
// A rule says "whenever THIS happens, play THAT". This file turns the rules
// and the timeline into the audio clips that make it so, and does it from
// scratch every time (apply), so the clips can never drift from the rules or
// from the clips they follow. Moving a caption moves its click; deleting a
// transition deletes its whoosh; adding a rule sounds every existing event
// at once.
//
// Pure functions over the model, so a test can hold them still. The window
// supplies what only it knows -- how long each sound is and its waveform --
// through SoundInfoLookup, and puts the result back into the view.

#include "TimelineModel.hpp"

#include <QString>
#include <QVector>

#include <functional>

namespace harpia {

// One thing a rule can fire on, found on the timeline.
struct SoundEvent {
	TlSoundRule::Trigger kind = TlSoundRule::Trigger::AnyClipStarts;
	int track = -1;         // the picture lane it happened on
	int clip = -1;          // the clip (for a transition: the incoming one)
	qint64 atMs = 0;        // when, in output time
	int transitionType = 0; // for transitions
	QStringList components; // component type ids on the clip
	TlClip::Type clipType = TlClip::Type::Video;
};

struct SoundInfo {
	qint64 durationMs = 0; // 0 = unknown sound: the rule places nothing
	QVector<float> peaks;  // waveform for the strip, may be empty
};
using SoundInfoLookup = std::function<SoundInfo(int sourceId)>;

class SoundRules {
public:
	// Every event on the picture lanes, in time order. A transition is one
	// event (at the start of the overlap), whatever else the incoming clip is;
	// a clip start is one event carrying everything the clip is, so a rule for
	// captions and a rule for the typing component both match a typing caption.
	static QVector<SoundEvent> events(const TimelineModel &m);

	// Does this rule fire on this event?
	static bool matches(const TlSoundRule &r, const SoundEvent &e);

	// The clip a rule places for an event. `outStartMs` is clamped at 0 when
	// the offset would put it before the start.
	static TlClip clipFor(const TlSoundRule &r, const SoundEvent &e, const SoundInfo &info);

	// Rebuild every rule-made clip. Removes all clips with soundRule > 0 from
	// every lane, then places the current rules' clips on the Sounds lane
	// (made, locked, at the bottom, when there is none and something is to be
	// placed; removed again when it would be left empty and unnamed). Returns
	// true when the model changed.
	static bool apply(TimelineModel &m, const SoundInfoLookup &info);

	// "Convert to normal clips": the rules' clips become the user's -- their
	// soundRule is cleared, the lane unlocks and stops being the Sounds lane --
	// and the rules that made them go, so the next apply makes nothing new.
	static void freeze(TimelineModel &m);

	// Names for the list. "Every crossfade", "Every image appears"...
	static QString triggerLabel(const TlSoundRule &r);
	static QString triggerLabel(TlSoundRule::Trigger t, int transitionType = 0,
				    const QString &componentId = QString());

	// The rule that would cover a given clip or transition, for "Sound for
	// every ..." on a right-click: the trigger the clip most specifically is.
	static TlSoundRule ruleForClip(const TlClip &c, bool onPictureLane);
	static TlSoundRule ruleForTransition(const TlClip &incoming, bool thisTypeOnly);

	// Built-in sounds, made rather than shipped: a handful of short, clean
	// effects synthesised into a 48 kHz mono WAV, so the feature works with
	// nothing to download. Names as shown ("Whoosh", "Pop"...).
	static QStringList builtinSounds();
	static bool writeBuiltin(const QString &name, const QString &wavPath);
	// The samples themselves, for the test and the writer.
	static std::vector<float> synthesize(const QString &name, int rate = 48000);

	static constexpr int kFadeInMs = 4;
	static constexpr int kFadeOutMs = 40;
};

} // namespace harpia
