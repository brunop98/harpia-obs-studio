#include "WavSilence.hpp"

#include <QFile>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace harpia {

namespace {
quint32 u32(const char *p)
{
	return quint32(quint8(p[0])) | (quint32(quint8(p[1])) << 8) | (quint32(quint8(p[2])) << 16) |
	       (quint32(quint8(p[3])) << 24);
}
quint16 u16(const char *p)
{
	return quint16(quint8(p[0]) | (quint8(p[1]) << 8));
}
void putU32(char *p, quint32 v)
{
	for (int i = 0; i < 4; ++i)
		p[i] = char((v >> (8 * i)) & 0xff);
}
} // namespace

WavTrim trimWavSilence(const QString &path, double thresholdDb, int keepLeadMs, int keepTailMs)
{
	WavTrim out;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return out;
	const QByteArray all = f.readAll();
	f.close();
	if (all.size() < 44 || std::memcmp(all.constData(), "RIFF", 4) != 0 ||
	    std::memcmp(all.constData() + 8, "WAVE", 4) != 0)
		return out;

	// Walk the chunks for "fmt " and "data"; anything else is kept as it is.
	int channels = 0, rate = 0, bits = 0;
	qsizetype dataAt = -1, dataLen = 0, dataHeaderAt = -1;
	for (qsizetype at = 12; at + 8 <= all.size();) {
		const char *h = all.constData() + at;
		const qsizetype len = qsizetype(u32(h + 4));
		if (std::memcmp(h, "fmt ", 4) == 0 && len >= 16 && at + 8 + 16 <= all.size()) {
			if (u16(h + 8) != 1) // PCM only
				return out;
			channels = u16(h + 10);
			rate = int(u32(h + 12));
			bits = u16(h + 22);
		} else if (std::memcmp(h, "data", 4) == 0) {
			dataHeaderAt = at;
			dataAt = at + 8;
			dataLen = std::min<qsizetype>(len, all.size() - dataAt);
			break;
		}
		at += 8 + len + (len & 1);
	}
	if (channels <= 0 || rate <= 0 || bits != 16 || dataAt < 0)
		return out;

	const qsizetype frameBytes = qsizetype(channels) * 2;
	const qsizetype frames = dataLen / frameBytes;
	const auto *s = reinterpret_cast<const qint16 *>(all.constData() + dataAt);
	const int threshold = std::max(1, int(std::lround(32768.0 * std::pow(10.0, thresholdDb / 20.0))));
	const auto loud = [&](qsizetype frame) {
		for (int c = 0; c < channels; ++c)
			if (std::abs(int(s[frame * channels + c])) >= threshold)
				return true;
		return false;
	};
	qsizetype first = 0;
	while (first < frames && !loud(first))
		++first;
	out.ok = true;
	if (first >= frames) {
		out.silent = true;
		out.durationMs = frames * 1000 / rate;
		return out;
	}
	qsizetype last = frames - 1;
	while (last > first && !loud(last))
		--last;

	const qsizetype from = std::max<qsizetype>(0, first - qsizetype(keepLeadMs) * rate / 1000);
	const qsizetype to = std::min<qsizetype>(frames, last + 1 + qsizetype(keepTailMs) * rate / 1000);
	out.leadMs = from * 1000 / rate;
	out.tailMs = (frames - to) * 1000 / rate;
	out.durationMs = (to - from) * 1000 / rate;
	if (from == 0 && to == frames)
		return out; // nothing worth cutting

	// Same header, new sizes, the kept frames.
	QByteArray kept = all.left(dataHeaderAt + 8);
	kept.append(all.constData() + dataAt + from * frameBytes, (to - from) * frameBytes);
	putU32(kept.data() + 4, quint32(kept.size() - 8));
	putU32(kept.data() + dataHeaderAt + 4, quint32((to - from) * frameBytes));
	QSaveFile w(path);
	if (!w.open(QIODevice::WriteOnly) || w.write(kept) != kept.size() || !w.commit()) {
		out.ok = false;
		out.leadMs = out.tailMs = 0;
		out.durationMs = frames * 1000 / rate;
	}
	return out;
}

} // namespace harpia
