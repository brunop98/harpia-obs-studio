#include "ThumbnailCache.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRunnable>
#include <QStandardPaths>
#include <QThreadPool>

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace harpia {

namespace {

// Mean brightness (0-255) of an already-RGB888 QImage. Used to reject the
// black lead-in frames that screen recordings typically open with.
double meanBrightness(const QImage &img)
{
	if (img.isNull())
		return 0.0;

	// Sample on a coarse grid so this stays cheap even for large frames.
	const int stepX = std::max(1, img.width() / 32);
	const int stepY = std::max(1, img.height() / 32);
	quint64 sum = 0;
	quint64 count = 0;
	for (int y = 0; y < img.height(); y += stepY) {
		const uchar *line = img.constScanLine(y);
		for (int x = 0; x < img.width(); x += stepX) {
			const uchar *px = line + x * 3; // RGB888: 3 bytes/pixel
			// Rec. 601 luma approximation.
			sum += (quint64)(px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8;
			++count;
		}
	}
	return count ? (double)sum / (double)count : 0.0;
}

// Convert a decoded frame to an RGB QImage scaled to fit `target` (aspect
// preserved). Returns null on failure.
QImage frameToImage(AVFrame *frame, const QSize &target)
{
	const int sw = frame->width;
	const int sh = frame->height;
	if (sw <= 0 || sh <= 0)
		return {};

	QSize out = QSize(sw, sh).scaled(target, Qt::KeepAspectRatio);
	if (out.width() < 1 || out.height() < 1)
		out = QSize(std::max(1, out.width()), std::max(1, out.height()));

	SwsContext *sws = sws_getContext(sw, sh, (AVPixelFormat)frame->format, out.width(), out.height(),
					 AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!sws)
		return {};

	QImage img(out.width(), out.height(), QImage::Format_RGB888);
	uint8_t *dst[4] = {img.bits(), nullptr, nullptr, nullptr};
	int dstStride[4] = {(int)img.bytesPerLine(), 0, 0, 0};
	sws_scale(sws, frame->data, frame->linesize, 0, sh, dst, dstStride);
	sws_freeContext(sws);
	return img;
}

} // namespace

ThumbnailCache::ThumbnailCache(QObject *parent) : QObject(parent)
{
	cacheDir_ = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
		    QStringLiteral("/thumbnails");
	QDir().mkpath(cacheDir_);
}

QString ThumbnailCache::keyFor(const QString &videoPath, qint64 mtimeSecs, const QSize &target)
{
	// Plain string key for the in-memory map — hashing happens only when the
	// key becomes a disk filename (diskPath), not on every cache lookup.
	return QStringLiteral("%1|%2|%3x%4")
		.arg(videoPath)
		.arg(mtimeSecs)
		.arg(target.width())
		.arg(target.height());
}

QString ThumbnailCache::diskPath(const QString &key) const
{
	const QString hashed = QString::fromLatin1(
		QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
	return cacheDir_ + QLatin1Char('/') + hashed + QStringLiteral(".png");
}

QImage ThumbnailCache::cached(const QString &videoPath, const QSize &target)
{
	const QFileInfo fi(videoPath);
	if (!fi.exists())
		return {};

	const QString key = keyFor(videoPath, fi.lastModified().toSecsSinceEpoch(), target);
	auto it = memory_.constFind(key);
	if (it != memory_.constEnd())
		return it.value();

	const QString disk = diskPath(key);
	if (QFileInfo::exists(disk)) {
		QImage img(disk);
		if (!img.isNull()) {
			memory_.insert(key, img);
			return img;
		}
	}
	return {};
}

void ThumbnailCache::ensure(const QString &videoPath, const QSize &target)
{
	const QFileInfo fi(videoPath);
	if (!fi.exists())
		return;

	const QString key = keyFor(videoPath, fi.lastModified().toSecsSinceEpoch(), target);
	if (memory_.contains(key) || inFlight_.contains(key))
		return;
	const QString disk = diskPath(key);
	if (QFileInfo::exists(disk)) {
		// A disk file only counts if it actually loads — a PNG truncated by a
		// crash mid-write would otherwise block regeneration forever.
		if (!QImage(disk).isNull())
			return;
		QFile::remove(disk);
	}

	inFlight_.insert(key);

	// Guard against this cache being destroyed while the job runs: hop back via
	// the (always-alive) application object and only touch the cache if it's
	// still there.
	QPointer<ThumbnailCache> guard(this);

	QThreadPool::globalInstance()->start(QRunnable::create([guard, videoPath, target, key, disk]() {
		QImage img = extractFrame(videoPath, target);
		if (!img.isNull()) {
			// Write atomically (temp + rename) so a crash mid-save can't
			// leave a truncated PNG at the final path.
			const QString tmp = disk + QStringLiteral(".tmp");
			if (img.save(tmp, "PNG")) {
				QFile::remove(disk);
				QFile::rename(tmp, disk);
			}
		}

		QMetaObject::invokeMethod(
			qApp,
			[guard, key, img, videoPath]() {
				if (guard)
					guard->jobFinished(key, img, videoPath);
			},
			Qt::QueuedConnection);
	}));
}

void ThumbnailCache::jobFinished(const QString &key, const QImage &img, const QString &videoPath)
{
	if (!img.isNull())
		memory_.insert(key, img);
	inFlight_.remove(key);
	emit ready(videoPath);
}

qint64 ThumbnailCache::probeDurationMs(const QString &videoPath)
{
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, videoPath.toUtf8().constData(), nullptr, nullptr) < 0)
		return 0;
	qint64 ms = 0;
	if (avformat_find_stream_info(fmt, nullptr) >= 0 && fmt->duration > 0)
		ms = (qint64)(fmt->duration / (AV_TIME_BASE / 1000)); // AV_TIME_BASE units -> ms
	avformat_close_input(&fmt);
	return ms;
}

QImage ThumbnailCache::extractFrame(const QString &videoPath, const QSize &target)
{
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, videoPath.toUtf8().constData(), nullptr, nullptr) < 0)
		return {};

	QImage result;
	AVCodecContext *ctx = nullptr;
	AVPacket *pkt = nullptr;
	AVFrame *frame = nullptr;

	do {
		if (avformat_find_stream_info(fmt, nullptr) < 0)
			break;

		const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (stream < 0)
			break;

		AVCodecParameters *par = fmt->streams[stream]->codecpar;
		const AVCodec *dec = avcodec_find_decoder(par->codec_id);
		if (!dec)
			break;

		ctx = avcodec_alloc_context3(dec);
		if (!ctx || avcodec_parameters_to_context(ctx, par) < 0)
			break;
		if (avcodec_open2(ctx, dec, nullptr) < 0)
			break;

		// Seek ~20% in (capped at 3s) to skip the black lead-in frames that
		// screen recordings typically open with.
		if (fmt->duration > 0) {
			int64_t seekTarget =
				std::min<int64_t>(3 * AV_TIME_BASE, fmt->duration / 5);
			if (seekTarget > 0) {
				av_seek_frame(fmt, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
				avcodec_flush_buffers(ctx);
			}
		}

		pkt = av_packet_alloc();
		frame = av_frame_alloc();
		if (!pkt || !frame)
			break;

		// A single seek can still land on a black keyframe, so decode a
		// bounded run of frames and keep the first that's clearly not black
		// (mean luma above a small threshold). Fall back to the brightest
		// frame seen if every candidate is dark.
		constexpr double kBlackThreshold = 16.0; // 0-255 mean luma
		constexpr int kMaxFrames = 60;
		QImage best;
		double bestBrightness = -1.0;
		int decoded = 0;

		while (decoded < kMaxFrames && av_read_frame(fmt, pkt) >= 0) {
			if (pkt->stream_index == stream &&
			    avcodec_send_packet(ctx, pkt) == 0) {
				while (avcodec_receive_frame(ctx, frame) == 0) {
					++decoded;
					QImage img = frameToImage(frame, target);
					av_frame_unref(frame);
					if (img.isNull())
						continue;
					const double b = meanBrightness(img);
					if (b > bestBrightness) {
						bestBrightness = b;
						best = img;
					}
					if (b >= kBlackThreshold) {
						result = img;
						break;
					}
				}
			}
			av_packet_unref(pkt);
			if (!result.isNull())
				break;
		}

		if (result.isNull())
			result = best; // all dark - best-effort brightest frame
	} while (false);

	if (frame)
		av_frame_free(&frame);
	if (pkt)
		av_packet_free(&pkt);
	if (ctx)
		avcodec_free_context(&ctx);
	avformat_close_input(&fmt);
	return result;
}

} // namespace harpia
