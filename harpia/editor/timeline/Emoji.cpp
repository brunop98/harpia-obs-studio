#include "Emoji.hpp"
#include "../SwsImage.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <QCoreApplication>
#include <QFile>
#include <QFontMetricsF>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QTextBoundaryFinder>

#include <cstring>

namespace harpia::emoji {

namespace {

struct Pack {
	QMutex lock;
	bool loaded = false;
	QString path;          // set by setPackPath (empty: the default places)
	QByteArray data;       // the whole pack
	QHash<QString, QPair<quint32, quint32>> index; // key -> offset, size
	QHash<QString, QImage> images;
	QHash<QString, QImage> scaled; // "key@side"
};

Pack &pack()
{
	static Pack p;
	return p;
}

quint32 le32(const char *p)
{
	const auto *u = reinterpret_cast<const unsigned char *>(p);
	return quint32(u[0]) | quint32(u[1]) << 8 | quint32(u[2]) << 16 | quint32(u[3]) << 24;
}

// Under the lock.
void loadLocked(Pack &p)
{
	if (p.loaded)
		return;
	p.loaded = true;
	QStringList candidates;
	if (!p.path.isEmpty())
		candidates << p.path;
	if (const QByteArray env = qgetenv("HARPIA_EMOJI_PACK"); !env.isEmpty())
		candidates << QString::fromLocal8Bit(env);
	if (QCoreApplication::instance()) {
		const QString dir = QCoreApplication::applicationDirPath();
		candidates << dir + QStringLiteral("/noto-emoji.pack")
			   << dir + QStringLiteral("/../../data/harpia/noto-emoji.pack");
	}
	for (const QString &c : candidates) {
		QFile f(c);
		if (!f.open(QIODevice::ReadOnly))
			continue;
		const QByteArray d = f.readAll();
		if (d.size() < 12 || std::memcmp(d.constData(), "HEMOJI01", 8) != 0)
			continue;
		const quint32 n = le32(d.constData() + 8);
		QHash<QString, QPair<quint32, quint32>> idx;
		qsizetype at = 12;
		bool good = true;
		for (quint32 i = 0; i < n && good; ++i) {
			if (at + 2 > d.size()) {
				good = false;
				break;
			}
			const int len = int(quint8(d[at])) | int(quint8(d[at + 1])) << 8;
			at += 2;
			if (at + len + 8 > d.size()) {
				good = false;
				break;
			}
			const QString key = QString::fromLatin1(d.constData() + at, len);
			at += len;
			const quint32 off = le32(d.constData() + at), size = le32(d.constData() + at + 4);
			at += 8;
			if (qint64(off) + size > d.size())
				good = false;
			idx.insert(key, {off, size});
		}
		if (!good)
			continue;
		p.data = d;
		p.index = idx;
		return;
	}
}

bool hasKey(const QString &key)
{
	Pack &p = pack();
	QMutexLocker l(&p.lock);
	loadLocked(p);
	return p.index.contains(key);
}

QImage decodeWebp(const QByteArray &bytes)
{
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_WEBP);
	if (!codec)
		return {};
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	QImage out;
	if (ctx && pkt && frame && avcodec_open2(ctx, codec, nullptr) >= 0 &&
	    av_new_packet(pkt, int(bytes.size())) >= 0) {
		std::memcpy(pkt->data, bytes.constData(), size_t(bytes.size()));
		if (avcodec_send_packet(ctx, pkt) >= 0 && avcodec_receive_frame(ctx, frame) >= 0) {
			// Same size in and out: the converter that overruns the last
			// row, so the image has room past it (see SwsImage.hpp).
			QImage img = swsTargetImage(frame->width, frame->height, QImage::Format_RGBA8888);
			SwsContext *sws = img.isNull() ? nullptr
						       : sws_getContext(frame->width, frame->height,
									AVPixelFormat(frame->format), frame->width,
									frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR,
									nullptr, nullptr, nullptr);
			if (sws) {
				uint8_t *dst[4] = {img.bits(), nullptr, nullptr, nullptr};
				int stride[4] = {int(img.bytesPerLine()), 0, 0, 0};
				sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, stride);
				sws_freeContext(sws);
				out = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
			}
		}
	}
	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&ctx);
	return out;
}

// Unicode's Emoji_Presentation=Yes below U+1F000 (emoji-data.txt, Unicode
// 16): the characters that are emoji even without U+FE0F. The rest of that
// range -- (TM), arrows, the card suits -- is text unless asked otherwise.
constexpr char32_t kBmpEmoji[][2] = {
	{0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE},
	{0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
	{0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4},
	{0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD},
	{0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E},
	{0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
	{0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
};

// Does this cluster read as an emoji (if the pack has it)? Asked for one
// (U+FE0F, a keycap), part of an emoji sequence (a joiner, a skin tone, flag
// letters, tags), or an emoji on its own: everything from U+1F000 (a few of
// those are text by default, but no text font draws them better), and the
// list above.
bool readsAsEmoji(const QList<uint> &cps)
{
	if (cps.isEmpty())
		return false;
	for (const uint cp : cps)
		if (cp == 0xFE0F || cp == 0x20E3 || cp == 0x200D || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
		    (cp >= 0x1F1E6 && cp <= 0x1F1FF) || (cp >= 0xE0020 && cp <= 0xE007F))
			return true;
	const uint first = cps.first();
	if (first >= 0x1F000)
		return true;
	for (const auto &r : kBmpEmoji)
		if (first >= r[0] && first <= r[1])
			return true;
	return false;
}

// Whether `text` could hold an emoji at all. Everything readsAsEmoji() looks
// for is at or above U+20E3 (the keycap mark) -- the selector, the BMP list,
// and everything from U+1F000, which arrives as surrogates -- except the
// joiner, and a joiner between characters that are none of those makes no
// emoji in the pack. So plain text, which is nearly every caption, never needs
// splitting into grapheme clusters: one pass over the code units says so.
bool mayHoldEmoji(const QString &text)
{
	for (const QChar ch : text)
		if (ch.unicode() >= 0x20E3)
			return true;
	return false;
}

} // namespace

void setPackPath(const QString &path)
{
	Pack &p = pack();
	QMutexLocker l(&p.lock);
	p.path = path;
	p.loaded = false;
	p.data.clear();
	p.index.clear();
	p.images.clear();
	p.scaled.clear();
}

bool available()
{
	return count() > 0;
}

int count()
{
	Pack &p = pack();
	QMutexLocker l(&p.lock);
	loadLocked(p);
	return int(p.index.size());
}

QString keyFor(const QString &cluster)
{
	QStringList parts;
	for (const char32_t cp : cluster.toUcs4())
		if (cp != 0xFE0F)
			parts << QStringLiteral("%1").arg(uint(cp), 4, 16, QLatin1Char('0'));
	return parts.join(QLatin1Char('_'));
}

QVector<Piece> split(const QString &text)
{
	QVector<Piece> out;
	if (text.isEmpty())
		return out;
	if (!mayHoldEmoji(text) || !available()) {
		out.push_back({text, QString()});
		return out;
	}
	QTextBoundaryFinder bf(QTextBoundaryFinder::Grapheme, text);
	int start = 0;
	QString plain;
	while (true) {
		const int next = bf.toNextBoundary();
		if (next < 0)
			break;
		const QString cluster = text.mid(start, next - start);
		start = next;
		QString key;
		if (readsAsEmoji(cluster.toUcs4())) {
			key = keyFor(cluster);
			if (!hasKey(key))
				key.clear();
		}
		if (key.isEmpty()) {
			plain += cluster;
			continue;
		}
		if (!plain.isEmpty()) {
			out.push_back({plain, QString()});
			plain.clear();
		}
		out.push_back({cluster, key});
	}
	if (!plain.isEmpty())
		out.push_back({plain, QString()});
	return out;
}

QImage image(const QString &key)
{
	Pack &p = pack();
	QMutexLocker l(&p.lock);
	loadLocked(p);
	if (auto it = p.images.constFind(key); it != p.images.constEnd())
		return it.value();
	const auto at = p.index.constFind(key);
	if (at == p.index.constEnd())
		return {};
	const QImage img = decodeWebp(p.data.mid(qsizetype(at->first), qsizetype(at->second)));
	p.images.insert(key, img);
	return img;
}

QImage image(const QString &key, int side)
{
	const QImage full = image(key);
	if (full.isNull() || side <= 0 || side >= full.width())
		return full;
	const QString k = key + QLatin1Char('@') + QString::number(side);
	Pack &p = pack();
	{
		QMutexLocker l(&p.lock);
		if (auto it = p.scaled.constFind(k); it != p.scaled.constEnd())
			return it.value();
	}
	const QImage img = full.scaled(side, side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	QMutexLocker l(&p.lock);
	if (p.scaled.size() > 4096) // a zoom animation: every size once
		p.scaled.clear();
	p.scaled.insert(k, img);
	return img;
}

namespace {
double em(const QFont &font)
{
	return font.pixelSize() > 0 ? font.pixelSize() : QFontMetricsF(font).height() / 1.2;
}
} // namespace

double advance(const QFont &font)
{
	return em(font) * 1.2;
}

QRectF square(const QFont &font, double x, double baseline)
{
	const double e = em(font), side = e * 1.1;
	return QRectF(x + (advance(font) - side) / 2.0, baseline - side * 0.84, side, side);
}

double width(const QFont &font, const QString &text)
{
	const QFontMetricsF fm(font);
	if (!mayHoldEmoji(text))
		return fm.horizontalAdvance(text); // what split() would come to, without it
	double w = 0.0;
	for (const Piece &pc : split(text))
		w += pc.isEmoji() ? advance(font) : fm.horizontalAdvance(pc.text);
	return w;
}

} // namespace harpia::emoji
