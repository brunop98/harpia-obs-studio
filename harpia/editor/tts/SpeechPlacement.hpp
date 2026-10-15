#pragma once

// Where a text-to-speech narration and its captions go on the timeline, as
// one linked group (TlClip::link): move one, move all.
//
// The narration takes the first unlocked audio lane with nothing under its
// span (a new "Speech" lane at the bottom when there is none); the captions
// take the first unlocked "Subtitles" lane free over THEIR span (a new one
// at the top of the picture stack when there is none -- captions draw over
// everything). All of them get one new link id.
//
// Pure, on a model copy, so the window can swap the result in as one undo step
// and a test can check it without a window.

#include "../timeline/TimelineModel.hpp"

#include <QVector>

namespace harpia {

struct SpeechPlacement {
	int audioTrack = -1, audioClip = -1;
	int captionTrack = -1;
	QVector<int> captionClips;
	int link = 0;
	// Every clip placed, for selecting the group.
	QVector<QPair<int, int>> all() const
	{
		QVector<QPair<int, int>> out;
		if (audioTrack >= 0)
			out.append({audioTrack, audioClip});
		for (int c : captionClips)
			out.append({captionTrack, c});
		return out;
	}
};

namespace speechdetail {
inline bool laneFree(const TlTrack &t, qint64 a, qint64 b)
{
	if (t.locked)
		return false;
	for (const TlClip &k : t.clips)
		if (k.outStartMs < b && k.outEndMs() > a)
			return false;
	return true;
}
} // namespace speechdetail

inline SpeechPlacement placeSpeech(TimelineModel &m, TlClip audio, QVector<TlClip> captions)
{
	SpeechPlacement out;
	out.link = m.nextLinkId();
	audio.link = out.link;
	for (TlClip &c : captions)
		c.link = out.link;

	// Captions first: a lane inserted at the top moves every index below it.
	if (!captions.isEmpty()) {
		qint64 a = captions.first().outStartMs, b = captions.first().outEndMs();
		for (const TlClip &c : captions) {
			a = std::min(a, c.outStartMs);
			b = std::max(b, c.outEndMs());
		}
		for (int i = 0; i < m.tracks.size() && out.captionTrack < 0; ++i)
			if (m.tracks[i].kind == TlTrack::Kind::Video &&
			    m.tracks[i].name.startsWith(QLatin1String("Subtitles")) &&
			    speechdetail::laneFree(m.tracks[i], a, b))
				out.captionTrack = i;
		if (out.captionTrack < 0) {
			TlTrack t;
			t.kind = TlTrack::Kind::Video;
			t.name = QStringLiteral("Subtitles");
			t.color = QColor(0xe0, 0xb3, 0x4a);
			m.tracks.insert(0, t);
			out.captionTrack = 0;
		}
		for (const TlClip &c : captions) {
			out.captionClips.append(int(m.tracks[out.captionTrack].clips.size()));
			m.tracks[out.captionTrack].clips.append(c);
		}
	}

	for (int i = 0; i < m.tracks.size() && out.audioTrack < 0; ++i)
		if (m.tracks[i].kind == TlTrack::Kind::Audio &&
		    speechdetail::laneFree(m.tracks[i], audio.outStartMs, audio.outEndMs()))
			out.audioTrack = i;
	if (out.audioTrack < 0) {
		TlTrack t;
		t.kind = TlTrack::Kind::Audio;
		t.name = QStringLiteral("Speech");
		t.color = QColor(0x5b, 0x9b, 0xd5);
		m.tracks.append(t);
		out.audioTrack = int(m.tracks.size()) - 1;
	}
	out.audioClip = int(m.tracks[out.audioTrack].clips.size());
	m.tracks[out.audioTrack].clips.append(audio);
	return out;
}

} // namespace harpia
