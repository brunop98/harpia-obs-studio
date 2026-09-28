#include "SoundRules.hpp"

#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

namespace harpia {

// ---- events -------------------------------------------------------------------

QVector<SoundEvent> SoundRules::events(const TimelineModel &m)
{
	QVector<SoundEvent> out;
	for (int ti = 0; ti < m.tracks.size(); ++ti) {
		const TlTrack &t = m.tracks[ti];
		if (!TimelineModel::isPictureKind(t.kind)) {
			// Audio lanes: tagged clips only, and never the Sounds lane or a
			// clip a rule made -- a rule firing on its own output would loop.
			if (t.autoSounds)
				continue;
			for (int ci = 0; ci < t.clips.size(); ++ci) {
				const TlClip &c = t.clips[ci];
				if (c.tags.isEmpty() || c.soundRule != 0)
					continue;
				SoundEvent e;
				e.kind = TlSoundRule::Trigger::AnyClipStarts;
				e.track = ti;
				e.clip = ci;
				e.atMs = c.outStartMs;
				e.clipType = c.type;
				e.tags = c.tags;
				e.audioLane = true;
				out.append(e);
			}
			continue;
		}
		// A hidden lane is not on screen, so nothing on it "appears".
		if (t.hidden)
			continue;
		// Every clip's incoming overlap in one sweep: asking overlapBefore()
		// per clip is a scan per clip, and this runs on every edit.
		const QVector<qint64> overlaps = t.overlapsBefore();
		for (int ci = 0; ci < t.clips.size(); ++ci) {
			const TlClip &c = t.clips[ci];
			SoundEvent e;
			e.track = ti;
			e.clip = ci;
			e.clipType = c.type;
			for (const ComponentInstance &comp : c.components)
				if (comp.enabled)
					e.components << comp.typeId;
			e.tags = c.tags;
			// The transition, when this clip arrives over the one before it.
			if (c.transition.enabled && ci < overlaps.size() && overlaps[ci] > 0) {
				SoundEvent tr = e;
				tr.kind = TlSoundRule::Trigger::Transition;
				tr.atMs = c.outStartMs;
				tr.transitionType = int(c.transition.type);
				out.append(tr);
			}
			e.kind = c.type == TlClip::Type::Image  ? TlSoundRule::Trigger::ImageAppears
				 : c.type == TlClip::Type::Text  ? TlSoundRule::Trigger::TextAppears
				 : c.type == TlClip::Type::Video ? TlSoundRule::Trigger::VideoStarts
								 : TlSoundRule::Trigger::AnyClipStarts;
			e.atMs = c.outStartMs;
			out.append(e);
		}
	}
	std::stable_sort(out.begin(), out.end(), [](const SoundEvent &a, const SoundEvent &b) {
		return a.atMs < b.atMs;
	});
	return out;
}

bool SoundRules::matches(const TlSoundRule &r, const SoundEvent &e)
{
	if (!r.enabled || r.sourceId <= 0)
		return false;
	if (r.lane >= 0 && r.lane != e.track)
		return false;
	if (e.audioLane && r.trigger != TlSoundRule::Trigger::Tagged)
		return false;
	const bool isTransition = e.kind == TlSoundRule::Trigger::Transition;
	switch (r.trigger) {
	case TlSoundRule::Trigger::AnyTransition:
		return isTransition;
	case TlSoundRule::Trigger::Transition:
		return isTransition && e.transitionType == r.transitionType;
	case TlSoundRule::Trigger::ImageAppears:
		return !isTransition && e.clipType == TlClip::Type::Image;
	case TlSoundRule::Trigger::TextAppears:
		return !isTransition && e.clipType == TlClip::Type::Text;
	case TlSoundRule::Trigger::VideoStarts:
		return !isTransition && e.clipType == TlClip::Type::Video;
	case TlSoundRule::Trigger::Component:
		return !isTransition && !r.componentId.isEmpty() && e.components.contains(r.componentId);
	case TlSoundRule::Trigger::AnyClipStarts:
		return !isTransition;
	case TlSoundRule::Trigger::Tagged:
		return !isTransition && r.tagId > 0 && e.tags.contains(r.tagId);
	case TlSoundRule::Trigger::Count:
		break;
	}
	return false;
}

TlClip SoundRules::clipFor(const TlSoundRule &r, const SoundEvent &e, const SoundInfo &info)
{
	TlClip c;
	c.type = TlClip::Type::Video; // a media clip; the lane makes it audio
	c.sourceId = r.sourceId;
	c.soundRule = r.id;
	c.srcStartMs = 0;
	qint64 len = info.durationMs;
	if (r.maxMs > 0)
		len = std::min<qint64>(len, r.maxMs);
	c.srcEndMs = std::max<qint64>(1, len);
	c.outStartMs = std::max<qint64>(0, e.atMs + r.offsetMs);
	c.volume = std::clamp(r.volume, 0.0, 2.0);
	c.fadeInMs = kFadeInMs;
	c.fadeOutMs = kFadeOutMs;
	c.clampFades();
	// Two sounds close together simply add up. With the transition flag on,
	// the mixer would crossfade them as if they were a cut, and a whoosh over
	// a click would dip both.
	c.transition.enabled = false;
	c.peaks = info.peaks;
	return c;
}

bool SoundRules::apply(TimelineModel &m, const SoundInfoLookup &info)
{
	// This runs on every edit -- every mouse move of a drag -- so the common
	// case, "the derived clips are already exactly right", is answered by
	// reading, without copying or comparing the whole timeline. Everything
	// here reads `m` through a const reference: iterating a shared copy
	// non-const makes Qt deep-copy every clip.
	const TimelineModel &cm = m;

	// What should be there.
	QVector<TlClip> made;
	if (!cm.soundRules.isEmpty()) {
		const QVector<SoundEvent> evs = events(cm);
		for (const TlSoundRule &r : cm.soundRules) {
			if (!r.enabled || r.sourceId <= 0)
				continue;
			SoundInfo si;
			bool looked = false;
			for (const SoundEvent &e : evs) {
				if (!matches(r, e))
					continue;
				if (!looked) {
					si = info ? info(r.sourceId) : SoundInfo();
					looked = true;
				}
				if (si.durationMs <= 0)
					break; // the sound is not there: nothing to place
				made.append(clipFor(r, e, si));
			}
		}
	}
	// Sound components: every enabled one on any clip that is not itself a
	// derived sound.
	for (const TlTrack &t : cm.tracks) {
		if (t.autoSounds)
			continue;
		for (const TlClip &c : t.clips) {
			if (c.soundRule != 0 || c.components.isEmpty())
				continue;
			for (const ComponentInstance &ci : c.components) {
				if (!ci.enabled || ci.typeId != QLatin1String(kSoundComponentId))
					continue;
				qint64 inAt = -1, outAt = -1;
				componentSoundTimes(c, ci, &inAt, &outAt);
				auto place = [&](const char *srcKey, const char *volKey, qint64 at) {
					const int src = ci.props.value(QString::fromLatin1(srcKey), 0).toInt();
					if (src <= 0 || at < 0)
						return;
					const SoundInfo si = info ? info(src) : SoundInfo();
					if (si.durationMs <= 0)
						return;
					TlSoundRule r;
					r.sourceId = src;
					r.volume = std::clamp(ci.props.value(QString::fromLatin1(volKey), 100).toDouble() / 100.0, 0.0, 2.0);
					SoundEvent e;
					e.atMs = at;
					TlClip sc = clipFor(r, e, si);
					sc.soundRule = kSoundFromComponent;
					made.append(sc);
				};
				place("inSound", "inVolume", inAt);
				if (outAt >= 0)
					place("outSound", "outVolume", outAt);
			}
		}
	}
	std::stable_sort(made.begin(), made.end(), [](const TlClip &a, const TlClip &b) {
		return a.outStartMs < b.outStartMs;
	});

	// Is that already what is there? The derived clips sit at the end of the
	// Sounds lane, in this order, and nowhere else; and the lane exists
	// exactly when it has to. (Peaks compare by their shared data first, so
	// an unchanged waveform costs nothing.)
	const int lane = cm.soundsLane();
	bool same = true;
	for (int ti = 0; ti < cm.tracks.size() && same; ++ti) {
		if (ti == lane)
			continue;
		for (const TlClip &c : cm.tracks[ti].clips)
			if (c.soundRule != 0) {
				same = false;
				break;
			}
	}
	if (same) {
		if (lane < 0) {
			same = made.isEmpty();
		} else {
			const QVector<TlClip> &lc = cm.tracks[lane].clips;
			const int n = lc.size(), k = made.size();
			int derived = 0;
			for (const TlClip &c : lc)
				if (c.soundRule != 0)
					++derived;
			same = derived == k;
			for (int i = 0; same && i < k; ++i) {
				const TlClip &c = lc[n - k + i];
				same = c.soundRule != 0 && c == made[i] && c.peaks == made[i].peaks;
			}
			// An emptied, unrenamed Sounds lane is taken away.
			if (same && n == 0 && cm.tracks[lane].name == QLatin1String("Sounds"))
				same = false;
		}
	}
	if (same)
		return false;

	// Rebuild. Only the lanes that hold a derived clip are touched, so the
	// others keep sharing their clips with the caller's copy.
	for (int ti = 0; ti < cm.tracks.size(); ++ti) {
		bool has = false;
		for (const TlClip &c : cm.tracks[ti].clips)
			if (c.soundRule != 0) {
				has = true;
				break;
			}
		if (!has)
			continue;
		QVector<TlClip> &clips = m.tracks[ti].clips;
		clips.erase(std::remove_if(clips.begin(), clips.end(),
					   [](const TlClip &c) { return c.soundRule != 0; }),
			    clips.end());
	}
	int at = cm.soundsLane();
	if (!made.isEmpty()) {
		if (at < 0) {
			TlTrack t;
			t.kind = TlTrack::Kind::Audio;
			t.name = QStringLiteral("Sounds");
			t.locked = true;
			t.autoSounds = true;
			t.color = QColor(0x8e, 0x6c, 0xb8);
			m.tracks.append(t); // the bottom: no other lane's index moves
			at = m.tracks.size() - 1;
		}
		m.tracks[at].clips.append(made);
	} else if (at >= 0 && cm.tracks[at].clips.isEmpty()) {
		// Nothing left to hold: the lane goes with it, unless it was
		// renamed, which makes it the user's.
		if (cm.tracks[at].name == QLatin1String("Sounds"))
			m.tracks.remove(at);
	}
	return true;
}

void SoundRules::componentSoundTimes(const TlClip &c, const ComponentInstance &ci, qint64 *inAt, qint64 *outAt)
{
	auto num = [&](const char *k, double def) { return ci.props.value(QString::fromLatin1(k), def).toDouble(); };
	*inAt = std::max<qint64>(0, c.outStartMs + qint64(std::llround(num("inOffsetMs", 0))));
	*outAt = -1;
	if (!ci.props.value(QStringLiteral("outOn"), false).toBool() || num("outSound", 0) <= 0)
		return;
	qint64 anchor = c.outEndMs();
	if (ci.props.value(QStringLiteral("alignSlide"), true).toBool()) {
		// The same arithmetic as slidePoseAt: the slide takes durationMs, or
		// half the clip when both halves would not fit.
		for (const ComponentInstance &s : c.components) {
			if (!s.enabled || s.typeId != QLatin1String("harpia.slide") ||
			    !s.props.value(QStringLiteral("slideOut"), false).toBool())
				continue;
			const qint64 dur = std::max<qint64>(1, c.outDurationMs());
			qint64 d = std::max<qint64>(0, qint64(std::llround(s.props.value(QStringLiteral("durationMs"), 500).toDouble())));
			d = std::min(d, dur / 2);
			anchor = c.outEndMs() - d;
			break;
		}
	}
	*outAt = std::max<qint64>(0, anchor + qint64(std::llround(num("outOffsetMs", 0))));
}

void SoundRules::freeze(TimelineModel &m)
{
	for (TlTrack &t : m.tracks) {
		for (TlClip &c : t.clips) {
			c.soundRule = 0;
			// Its sounds are ordinary clips now; a Sound component left on
			// the clip would place them a second time.
			c.components.erase(std::remove_if(c.components.begin(), c.components.end(),
							  [](const ComponentInstance &ci) {
								  return ci.typeId == QLatin1String(kSoundComponentId);
							  }),
					   c.components.end());
		}
		if (t.autoSounds) {
			t.autoSounds = false;
			t.locked = false;
		}
	}
	m.soundRules.clear();
}

// ---- names --------------------------------------------------------------------

QString SoundRules::triggerLabel(const TlSoundRule &r, const TimelineModel *m)
{
	return triggerLabel(r.trigger, r.transitionType, r.componentId,
			    m ? m->tagName(r.tagId) : QStringLiteral("#%1").arg(r.tagId));
}

TlSoundRule SoundRules::ruleForTag(int tagId)
{
	TlSoundRule r;
	r.trigger = TlSoundRule::Trigger::Tagged;
	r.tagId = tagId;
	return r;
}

QString SoundRules::triggerLabel(TlSoundRule::Trigger t, int transitionType, const QString &componentId,
				 const QString &tagName)
{
	switch (t) {
	case TlSoundRule::Trigger::AnyTransition:
		return QStringLiteral("Every transition");
	case TlSoundRule::Trigger::Transition:
		return QStringLiteral("Every %1 transition")
			.arg(QString::fromLatin1(transitionName(transitionFromInt(transitionType))).toLower());
	case TlSoundRule::Trigger::ImageAppears:
		return QStringLiteral("Every image appears");
	case TlSoundRule::Trigger::TextAppears:
		return QStringLiteral("Every caption appears");
	case TlSoundRule::Trigger::VideoStarts:
		return QStringLiteral("Every video clip starts");
	case TlSoundRule::Trigger::Component: {
		QString n = componentId;
		if (n == QLatin1String("harpia.textType"))
			n = QStringLiteral("typing caption");
		else if (n.startsWith(QLatin1String("harpia.")))
			n = n.mid(7);
		return QStringLiteral("Every %1 appears").arg(n);
	}
	case TlSoundRule::Trigger::AnyClipStarts:
		return QStringLiteral("Every clip starts");
	case TlSoundRule::Trigger::Tagged:
		return QStringLiteral("Every clip tagged %1").arg(tagName.isEmpty() ? QStringLiteral("(no tag)") : tagName);
	case TlSoundRule::Trigger::Count:
		break;
	}
	return QString();
}

TlSoundRule SoundRules::ruleForClip(const TlClip &c, bool onPictureLane)
{
	TlSoundRule r;
	// The most specific thing the clip is: a typing caption is a component
	// rule (only those), a plain caption a caption rule, and so on.
	for (const ComponentInstance &ci : c.components)
		if (ci.enabled && ci.typeId == QLatin1String("harpia.textType")) {
			r.trigger = TlSoundRule::Trigger::Component;
			r.componentId = ci.typeId;
			return r;
		}
	if (c.type == TlClip::Type::Image)
		r.trigger = TlSoundRule::Trigger::ImageAppears;
	else if (c.type == TlClip::Type::Text)
		r.trigger = TlSoundRule::Trigger::TextAppears;
	else if (c.type == TlClip::Type::Video && onPictureLane)
		r.trigger = TlSoundRule::Trigger::VideoStarts;
	else
		r.trigger = TlSoundRule::Trigger::AnyClipStarts;
	return r;
}

TlSoundRule SoundRules::ruleForTransition(const TlClip &incoming, bool thisTypeOnly)
{
	TlSoundRule r;
	r.trigger = thisTypeOnly ? TlSoundRule::Trigger::Transition : TlSoundRule::Trigger::AnyTransition;
	r.transitionType = int(incoming.transition.type);
	return r;
}

// ---- built-in sounds ----------------------------------------------------------

QStringList SoundRules::builtinSounds()
{
	return {QStringLiteral("Whoosh"), QStringLiteral("Pop"), QStringLiteral("Click"),
		QStringLiteral("Ding"), QStringLiteral("Typing"), QStringLiteral("Swoosh up"),
		QStringLiteral("Thud")};
}

namespace {
constexpr double kPi = 3.14159265358979323846; // kPi needs _USE_MATH_DEFINES on MSVC
// Deterministic noise, so the same name always makes the same file.
struct Noise {
	quint32 s = 0x9e3779b9u;
	float next()
	{
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return float(s) / 4294967296.0f * 2.0f - 1.0f;
	}
};
// A one-pole low-pass; `k` in 0..1, higher = brighter.
struct LowPass {
	float y = 0.f;
	float run(float x, float k)
	{
		y += k * (x - y);
		return y;
	}
};
float envAD(double t, double attack, double decay)
{
	if (t < 0)
		return 0.f;
	if (t < attack)
		return float(t / attack);
	const double d = (t - attack) / decay;
	return d >= 1 ? 0.f : float(std::pow(1.0 - d, 2.0));
}
} // namespace

std::vector<float> SoundRules::synthesize(const QString &name, int rate)
{
	std::vector<float> pcm;
	const double dt = 1.0 / rate;
	auto samples = [&](double seconds) { return int(seconds * rate); };
	const QString n = name.toLower();
	if (n == QLatin1String("whoosh") || n == QLatin1String("swoosh up")) {
		// Filtered noise that sweeps: down for a whoosh, up for the swoosh.
		const bool up = n.startsWith(QLatin1String("swoosh"));
		const int len = samples(0.6);
		Noise nz;
		LowPass lp;
		for (int i = 0; i < len; ++i) {
			const double t = i * dt;
			const double u = t / 0.6;
			const double sweep = up ? u : 1.0 - u;
			const float k = float(0.02 + 0.5 * sweep * sweep);
			const float env = envAD(t, 0.15, 0.45);
			pcm.push_back(lp.run(nz.next(), k) * env * 0.9f);
		}
	} else if (n == QLatin1String("pop")) {
		// A short sine whose pitch drops fast.
		const int len = samples(0.12);
		double ph = 0;
		for (int i = 0; i < len; ++i) {
			const double t = i * dt;
			const double f = 900.0 * std::exp(-t * 35.0) + 120.0;
			ph += 2 * kPi * f * dt;
			pcm.push_back(float(std::sin(ph)) * envAD(t, 0.002, 0.11) * 0.8f);
		}
	} else if (n == QLatin1String("click")) {
		const int len = samples(0.03);
		Noise nz;
		LowPass lp;
		for (int i = 0; i < len; ++i) {
			const double t = i * dt;
			pcm.push_back(lp.run(nz.next(), 0.6f) * envAD(t, 0.0005, 0.025) * 0.7f);
		}
	} else if (n == QLatin1String("ding")) {
		// Two partials, the upper one fading faster, like a small bell.
		const int len = samples(1.2);
		for (int i = 0; i < len; ++i) {
			const double t = i * dt;
			const float a = float(std::sin(2 * kPi * 1568.0 * t)) * float(std::exp(-t * 3.0));
			const float b = float(std::sin(2 * kPi * 1568.0 * 2.76 * t)) * float(std::exp(-t * 9.0)) * 0.4f;
			pcm.push_back((a + b) * envAD(t, 0.002, 1.2) * 0.6f);
		}
	} else if (n == QLatin1String("typing")) {
		// A run of key clicks at a typist's pace, each a little different.
		const int len = samples(1.5);
		pcm.assign(len, 0.f);
		Noise nz;
		double at = 0.0;
		int key = 0;
		while (at < 1.45) {
			LowPass lp;
			const int start = samples(at);
			const float bright = 0.35f + 0.3f * (nz.next() * 0.5f + 0.5f);
			for (int i = 0; i < samples(0.02) && start + i < len; ++i) {
				const double t = i * dt;
				pcm[start + i] += lp.run(nz.next(), bright) * envAD(t, 0.0005, 0.015) * 0.6f;
			}
			at += 0.06 + 0.05 * (nz.next() * 0.5 + 0.5);
			++key;
		}
	} else if (n == QLatin1String("thud")) {
		const int len = samples(0.35);
		double ph = 0;
		Noise nz;
		LowPass lp;
		for (int i = 0; i < len; ++i) {
			const double t = i * dt;
			const double f = 160.0 * std::exp(-t * 12.0) + 45.0;
			ph += 2 * kPi * f * dt;
			const float body = float(std::sin(ph)) * envAD(t, 0.003, 0.3);
			const float knock = lp.run(nz.next(), 0.15f) * envAD(t, 0.001, 0.04) * 0.5f;
			pcm.push_back((body + knock) * 0.85f);
		}
	}
	return pcm;
}

bool SoundRules::writeBuiltin(const QString &name, const QString &wavPath)
{
	const int rate = 48000;
	const std::vector<float> pcm = synthesize(name, rate);
	if (pcm.empty())
		return false;
	QFile f(wavPath);
	if (!f.open(QIODevice::WriteOnly))
		return false;
	const quint32 dataBytes = quint32(pcm.size() * 2);
	auto u32 = [&](quint32 v) { f.write(reinterpret_cast<const char *>(&v), 4); };
	auto u16 = [&](quint16 v) { f.write(reinterpret_cast<const char *>(&v), 2); };
	f.write("RIFF");
	u32(36 + dataBytes);
	f.write("WAVE");
	f.write("fmt ");
	u32(16);
	u16(1);
	u16(1); // mono
	u32(quint32(rate));
	u32(quint32(rate * 2));
	u16(2);
	u16(16);
	f.write("data");
	u32(dataBytes);
	std::vector<qint16> block(pcm.size());
	for (size_t i = 0; i < pcm.size(); ++i)
		block[i] = qint16(std::lround(std::clamp(pcm[i], -1.f, 1.f) * 32767.f));
	f.write(reinterpret_cast<const char *>(block.data()), qint64(block.size() * 2));
	return true;
}

} // namespace harpia
