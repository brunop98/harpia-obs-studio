#include "AudioForSpeech.hpp"

#include "../VoiceoverMixer.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <algorithm>

namespace harpia {

namespace {
// The RIFF the mixer writes: PCM, 16-bit, 48 kHz, stereo. Returns the frame
// count and points `data` at the samples; false if it is not that.
bool readMixerWav(const QByteArray &wav, const qint16 **data, qint64 *frames)
{
	if (wav.size() < 44 || !wav.startsWith("RIFF") || wav.mid(8, 4) != "WAVE")
		return false;
	int pos = 12;
	int channels = 0, rate = 0, bits = 0;
	while (pos + 8 <= wav.size()) {
		const QByteArray id = wav.mid(pos, 4);
		const quint32 len = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(wav.constData() + pos + 4));
		pos += 8;
		if (id == "fmt " && len >= 16) {
			const uchar *f = reinterpret_cast<const uchar *>(wav.constData() + pos);
			channels = qFromLittleEndian<quint16>(f + 2);
			rate = int(qFromLittleEndian<quint32>(f + 4));
			bits = qFromLittleEndian<quint16>(f + 14);
		} else if (id == "data") {
			if (channels != 2 || rate != 48000 || bits != 16)
				return false;
			const qint64 avail = std::min<qint64>(len, wav.size() - pos);
			*data = reinterpret_cast<const qint16 *>(wav.constData() + pos);
			*frames = avail / 4;
			return *frames > 0;
		}
		pos += int(len + (len & 1));
	}
	return false;
}
} // namespace

QVector<qint16> AudioForSpeech::downmixTo16k(const qint16 *s, qint64 frames)
{
	QVector<qint16> out;
	out.reserve(int(frames / 3 + 1));
	for (qint64 f = 0; f + 3 <= frames; f += 3) {
		int acc = 0;
		for (int k = 0; k < 3; ++k)
			acc += int(s[(f + k) * 2]) + int(s[(f + k) * 2 + 1]);
		out.append(qint16(std::clamp(acc / 6, -32768, 32767)));
	}
	return out;
}

bool AudioForSpeech::writeWav16k(const QString &path, const QVector<qint16> &mono)
{
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	auto u32 = [](quint32 v) {
		char b[4] = {char(v & 0xff), char((v >> 8) & 0xff), char((v >> 16) & 0xff), char((v >> 24) & 0xff)};
		return QByteArray(b, 4);
	};
	auto u16 = [](quint16 v) {
		char b[2] = {char(v & 0xff), char((v >> 8) & 0xff)};
		return QByteArray(b, 2);
	};
	const quint32 dataBytes = quint32(mono.size()) * 2;
	f.write("RIFF");
	f.write(u32(36 + dataBytes));
	f.write("WAVE");
	f.write("fmt ");
	f.write(u32(16));
	f.write(u16(1)); // PCM
	f.write(u16(1)); // mono
	f.write(u32(kRate));
	f.write(u32(kRate * 2));
	f.write(u16(2));
	f.write(u16(16));
	f.write("data");
	f.write(u32(dataBytes));
	// Little-endian on every platform this ships on; written sample by sample
	// so a big-endian host would still be right.
	QByteArray pcm;
	pcm.resize(int(dataBytes));
	for (int i = 0; i < mono.size(); ++i) {
		pcm[2 * i] = char(mono[i] & 0xff);
		pcm[2 * i + 1] = char((mono[i] >> 8) & 0xff);
	}
	f.write(pcm);
	return true;
}

QVector<SpeechAudioChunk> AudioForSpeech::prepare(const QString &sourcePath, qint64 srcStartMs,
						   qint64 srcEndMs, const QString &workDir, QString *err)
{
	QVector<SpeechAudioChunk> out;
	QDir().mkpath(workDir);
	const QString full = QDir(workDir).filePath(QStringLiteral("speech-src.wav"));
	if (!VoiceoverMixer::decodeToWav(sourcePath, full)) {
		if (err)
			*err = QStringLiteral("could not decode the audio of %1").arg(QFileInfo(sourcePath).fileName());
		return out;
	}
	QFile f(full);
	if (!f.open(QIODevice::ReadOnly)) {
		if (err)
			*err = QStringLiteral("could not read the decoded audio");
		return out;
	}
	const QByteArray wav = f.readAll();
	f.close();
	QFile::remove(full);
	const qint16 *pcm = nullptr;
	qint64 frames = 0;
	if (!readMixerWav(wav, &pcm, &frames)) {
		if (err)
			*err = QStringLiteral("the decoded audio was not in the expected format");
		return out;
	}
	const qint64 totalMs = frames * 1000 / 48000;
	const qint64 from = std::clamp<qint64>(srcStartMs, 0, totalMs);
	const qint64 to = std::clamp<qint64>(srcEndMs <= 0 ? totalMs : srcEndMs, from, totalMs);
	if (to - from < 200) {
		if (err)
			*err = QStringLiteral("the clip holds less than a fifth of a second of audio");
		return out;
	}
	int n = 0;
	for (qint64 at = from; at < to; at += kChunkMs) {
		const qint64 end = std::min(to, at + kChunkMs);
		const qint64 f0 = at * 48000 / 1000;
		const qint64 f1 = end * 48000 / 1000;
		const QVector<qint16> mono = downmixTo16k(pcm + f0 * 2, f1 - f0);
		SpeechAudioChunk ch;
		ch.wavPath = QDir(workDir).filePath(QStringLiteral("speech-%1.wav").arg(n++));
		ch.offsetMs = at;
		ch.lengthMs = end - at;
		if (!writeWav16k(ch.wavPath, mono)) {
			if (err)
				*err = QStringLiteral("could not write %1").arg(ch.wavPath);
			return {};
		}
		out.append(ch);
	}
	return out;
}

} // namespace harpia
