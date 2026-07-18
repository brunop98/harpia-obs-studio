#include "ThumbnailCache.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
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
	const QString raw = QStringLiteral("%1|%2|%3x%4")
				    .arg(videoPath)
				    .arg(mtimeSecs)
				    .arg(target.width())
				    .arg(target.height());
	return QString::fromLatin1(QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Md5).toHex());
}

QString ThumbnailCache::diskPath(const QString &key) const
{
	return cacheDir_ + QLatin1Char('/') + key + QStringLiteral(".png");
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
	if (memory_.contains(key) || QFileInfo::exists(diskPath(key)) || inFlight_.contains(key))
		return;

	inFlight_.insert(key);
	const QString disk = diskPath(key);

	// Guard against this cache being destroyed while the job runs: hop back via
	// the (always-alive) application object and only touch the cache if it's
	// still there.
	QPointer<ThumbnailCache> guard(this);

	QThreadPool::globalInstance()->start(QRunnable::create([guard, videoPath, target, key, disk]() {
		QImage img = extractFrame(videoPath, target);
		if (!img.isNull())
			img.save(disk, "PNG");

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

		// Seek a little way in (10% or 1s, whichever is smaller) to avoid
		// black lead-in frames.
		if (fmt->duration > 0) {
			int64_t seekTarget = std::min<int64_t>(AV_TIME_BASE, fmt->duration / 10);
			if (seekTarget > 0) {
				av_seek_frame(fmt, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
				avcodec_flush_buffers(ctx);
			}
		}

		pkt = av_packet_alloc();
		frame = av_frame_alloc();
		if (!pkt || !frame)
			break;

		while (av_read_frame(fmt, pkt) >= 0) {
			if (pkt->stream_index == stream) {
				if (avcodec_send_packet(ctx, pkt) == 0 &&
				    avcodec_receive_frame(ctx, frame) == 0) {
					result = frameToImage(frame, target);
					av_packet_unref(pkt);
					break;
				}
			}
			av_packet_unref(pkt);
		}
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
