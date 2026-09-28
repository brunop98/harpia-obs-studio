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
				if (c.tags.isEmpty() || c.soundRule > 0)
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
			if (c.transition.enabled && t.overlapBefore(ci) > 0) {
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
	// What is there now, to know whether anything changed at the end.
	const TimelineModel before = m;

	// Out with every rule-made clip, wherever it is (a lane may have been
	// converted, or the file hand-edited).
	for (TlTrack &t : m.tracks)
		t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
					     [](const TlClip &c) { return c.soundRule > 0; }),
			      t.clips.end());

	// In with the current ones.
	QVector<TlClip> made;
	if (!m.soundRules.isEmpty()) {
		const QVector<SoundEvent> evs = events(m);
		for (const TlSoundRule &r : m.soundRules) {
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
	std::stable_sort(made.begin(), made.end(), [](const TlClip &a, const TlClip &b) {
		return a.outStartMs < b.outStartMs;
	});

	int lane = m.soundsLane();
	if (!made.isEmpty()) {
		if (lane < 0) {
			TlTrack t;
			t.kind = TlTrack::Kind::Audio;
			t.name = QStringLiteral("Sounds");
			t.locked = true;
			t.autoSounds = true;
			t.color = QColor(0x8e, 0x6c, 0xb8);
			m.tracks.append(t); // the bottom: no other lane's index moves
			lane = m.tracks.size() - 1;
		}
		m.tracks[lane].clips.append(made);
	} else if (lane >= 0 && m.tracks[lane].clips.isEmpty()) {
		// Nothing left to hold: the lane goes with it, unless it was
		// renamed, which makes it the user's.
		if (m.tracks[lane].name == QLatin1String("Sounds"))
			m.tracks.remove(lane);
	}
	return !(m == before);
}

void SoundRules::freeze(TimelineModel &m)
{
	for (TlTrack &t : m.tracks) {
		for (TlClip &c : t.clips)
			c.soundRule = 0;
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
