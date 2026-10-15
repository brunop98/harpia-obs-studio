#pragma once

// The narration's audio: each piece of text comes back as its own file, is
// decoded to the editor's 48 kHz stereo 16-bit WAV (VoiceoverMixer), and the
// pieces are joined here, a short breath between them, into the one file the
// timeline plays. Header-only: plain WAV reading and writing, testable.

#include <QFile>
#include <QString>
#include <QVector>

#include <cstring>

namespace harpia::tts_audio {

struct Pcm {
	QVector<qint16> samples; // interleaved
	int rate = 48000;
	int channels = 2;
	qint64 frames() const { return channels > 0 ? samples.size() / channels : 0; }
	qint64 durationMs() const { return rate > 0 ? frames() * 1000 / rate : 0; }
};

inline bool readWav16(const QString &path, Pcm &out)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return false;
	const QByteArray d = f.readAll();
	if (d.size() < 44 || !d.startsWith("RIFF") || d.mid(8, 4) != "WAVE")
		return false;
	const auto u16 = [&](qsizetype at) { return int(quint8(d[at])) | int(quint8(d[at + 1])) << 8; };
	const auto u32 = [&](qsizetype at) { return quint32(u16(at)) | quint32(u16(at + 2)) << 16; };
	qsizetype at = 12;
	int bits = 0;
	while (at + 8 <= d.size()) {
		const QByteArray id = d.mid(at, 4);
		qsizetype len = qsizetype(u32(at + 4));
		if (id == "fmt " && at + 24 <= d.size()) {
			out.channels = u16(at + 10);
			out.rate = int(u32(at + 12));
			bits = u16(at + 22);
		} else if (id == "data") {
			if (bits != 16 || out.channels <= 0)
				return false;
			len = std::min(len, d.size() - at - 8);
			out.samples.resize(len / 2);
			std::memcpy(out.samples.data(), d.constData() + at + 8, size_t(out.samples.size()) * 2);
			return true;
		}
		at += 8 + len + (len & 1);
	}
	return false;
}

inline bool writeWav16(const QString &path, const Pcm &pcm)
{
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	const quint32 dataBytes = quint32(pcm.samples.size()) * 2;
	QByteArray h;
	const auto put32 = [&h](quint32 v) {
		for (int i = 0; i < 4; ++i)
			h.append(char((v >> (8 * i)) & 0xff));
	};
	const auto put16 = [&h](quint16 v) {
		h.append(char(v & 0xff));
		h.append(char(v >> 8));
	};
	h.append("RIFF");
	put32(36 + dataBytes);
	h.append("WAVEfmt ");
	put32(16);
	put16(1); // PCM
	put16(quint16(pcm.channels));
	put32(quint32(pcm.rate));
	put32(quint32(pcm.rate * pcm.channels * 2));
	put16(quint16(pcm.channels * 2));
	put16(16);
	h.append("data");
	put32(dataBytes);
	return f.write(h) == h.size() &&
	       f.write(reinterpret_cast<const char *>(pcm.samples.constData()), dataBytes) == qint64(dataBytes);
}

// The pieces back to back with `gapMs` of silence between them; `offsetsMs`
// gets where each one starts. All pieces must share rate and channels (the
// editor's decode makes them so); a piece that does not is skipped.
inline Pcm join(const QVector<Pcm> &pieces, qint64 gapMs, QVector<qint64> *offsetsMs)
{
	Pcm out;
	if (!pieces.isEmpty()) {
		out.rate = pieces.first().rate;
		out.channels = pieces.first().channels;
	}
	const qint64 gap = gapMs * out.rate / 1000 * out.channels;
	offsetsMs->clear();
	for (int i = 0; i < pieces.size(); ++i) {
		const Pcm &p = pieces[i];
		if (i > 0)
			out.samples.resize(out.samples.size() + gap); // zero-filled
		offsetsMs->append(out.frames() * 1000 / std::max(1, out.rate));
		if (p.rate == out.rate && p.channels == out.channels)
			out.samples += p.samples;
	}
	return out;
}

} // namespace harpia::tts_audio
