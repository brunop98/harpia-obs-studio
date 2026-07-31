#include "editor/ProxyMedia.hpp"

#include "core/ShareExporter.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>
#include <QWaitCondition>

#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace harpia {

// How the proxy is encoded. 540p is comfortably above what the preview widget
// shows on a normal window, and small enough that a whole GOP decodes in a few
// milliseconds. The short GOP is the actual point: a keyframe every twelve
// frames means a seek anywhere costs at most twelve small frames, where the
// source cost a hundred-odd large ones.
static constexpr int kProxyHeight = 540;
static constexpr int kProxyGop = 12;
static constexpr int kProxyCrf = 28; // a preview, not a master

QString proxyCacheDir()
{
	const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
	const QString dir = base + QStringLiteral("/harpia/proxies");
	QDir().mkpath(dir);
	return dir;
}

QString proxyPathFor(const QString &sourcePath)
{
	const QFileInfo fi(sourcePath);
	// Path, size and modification time. Re-encode the file in place and the key
	// changes, so a proxy of the old content can never be served for the new --
	// which would show the editor a video that no longer exists.
	QCryptographicHash h(QCryptographicHash::Sha1);
	h.addData(fi.absoluteFilePath().toUtf8());
	h.addData(QByteArray::number(fi.size()));
	h.addData(QByteArray::number(fi.lastModified().toMSecsSinceEpoch()));
	return proxyCacheDir() + QStringLiteral("/") + QString::fromLatin1(h.result().toHex()) +
	       QStringLiteral(".mp4");
}

bool wantsProxy(int width, int height, const QString &codecName)
{
	if (width <= 0 || height <= 0)
		return false;
	// Above 1080p: the decode itself is the problem whatever the codec.
	if (qint64(width) * height > qint64(1920) * 1080)
		return true;
	// At or below 1080p only the slow-to-seek codecs are worth it. These are
	// the WEBM/AV1 downloads; an H.264 screen recording scrubs fine as it is,
	// and proxying it would be a transcode that buys nothing.
	const QString c = codecName.toLower();
	if (c == QLatin1String("vp9") || c == QLatin1String("vp8") || c == QLatin1String("av1"))
		return qint64(width) * height > qint64(1280) * 720;
	return false;
}

bool probeVideo(const QString &path, int *width, int *height, QString *codecName)
{
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0)
		return false;
	bool ok = false;
	if (avformat_find_stream_info(fmt, nullptr) >= 0) {
		const int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (vi >= 0) {
			AVCodecParameters *p = fmt->streams[vi]->codecpar;
			if (width)
				*width = p->width;
			if (height)
				*height = p->height;
			if (codecName) {
				const char *n = avcodec_get_name(p->codec_id);
				*codecName = QString::fromLatin1(n ? n : "");
			}
			ok = true;
		}
	}
	avformat_close_input(&fmt);
	return ok;
}

// ---------------------------------------------------------------------------

class ProxyBuilder::Worker : public QThread {
public:
	explicit Worker(ProxyBuilder *owner) : owner_(owner) {}

	void post(const Job &j)
	{
		QMutexLocker lock(&mutex_);
		for (const Job &q : queue_)
			if (q.sourceId == j.sourceId)
				return; // already waiting for this one
		queue_.push_back(j);
		wake_.wakeOne();
	}

	void stop()
	{
		{
			QMutexLocker lock(&mutex_);
			quit_ = true;
			queue_.clear();
			wake_.wakeAll();
		}
		if (ShareExporter *ex = exporter_.load())
			ex->cancel();
	}

	bool busy() const
	{
		QMutexLocker lock(&mutex_);
		return running_ || !queue_.isEmpty();
	}

protected:
	void run() override
	{
		for (;;) {
			Job job;
			{
				QMutexLocker lock(&mutex_);
				while (!quit_ && queue_.isEmpty())
					wake_.wait(&mutex_);
				if (quit_)
					return;
				job = queue_.takeFirst();
				running_ = true;
			}
			build(job);
			{
				QMutexLocker lock(&mutex_);
				running_ = false;
			}
		}
	}

private:
	void build(const Job &job)
	{
		const QString out = proxyPathFor(job.path);
		// Already built, by an earlier session or an earlier source using the
		// same file. Nothing about the file has changed or the key would differ.
		if (QFileInfo::exists(out) && QFileInfo(out).size() > 0) {
			emit owner_->ready(job.sourceId, out);
			return;
		}

		// Written under a temporary name and renamed on success, so a build
		// interrupted by a crash or a close can never leave a half-file that
		// the next session would trust and decode from. The .mp4 stays LAST:
		// libav picks the muxer from the extension, and a name ending .part has
		// no format it can choose.
		const QString tmp = out + QStringLiteral(".part.mp4");
		QFile::remove(tmp);

		ShareExporter ex;
		exporter_.store(&ex);
		// Context-free connections: the lambdas run in this thread, where the
		// signals are emitted, which is what lets the results below be captured
		// by reference.
		QObject::connect(&ex, &ShareExporter::progress,
				 [this, id = job.sourceId](int pct, double, qint64, qint64) {
					 emit owner_->progress(id, pct);
				 });

		ShareExporter::Options opts{};
		opts.maxHeight = kProxyHeight;
		opts.crf = kProxyCrf;
		opts.preset = "veryfast";
		opts.audioKbps = 0;
		opts.gopFrames = kProxyGop;
		opts.dropAudio = true;

		bool ok = false;
		bool canceled = false;
		QString err;
		QObject::connect(&ex, &ShareExporter::finished, [&](bool o, bool c, const QString &e) {
			ok = o;
			canceled = c;
			err = e;
		});
		ex.run(job.path, tmp, opts);
		exporter_.store(nullptr);

		if (!ok || canceled) {
			QFile::remove(tmp);
			if (!canceled)
				emit owner_->failed(job.sourceId,
						    err.isEmpty() ? QStringLiteral("proxy build failed")
								  : err);
			return;
		}
		QFile::remove(out); // a stale file at the target would block the rename
		if (!QFile::rename(tmp, out)) {
			QFile::remove(tmp);
			emit owner_->failed(job.sourceId, QStringLiteral("could not store the proxy"));
			return;
		}
		emit owner_->ready(job.sourceId, out);
	}

	ProxyBuilder *owner_;
	mutable QMutex mutex_;
	QWaitCondition wake_;
	QVector<Job> queue_;
	bool quit_ = false;
	bool running_ = false;
	// Written by the build thread, read by whoever calls stop().
	std::atomic<ShareExporter *> exporter_{nullptr};
};

ProxyBuilder::ProxyBuilder(QObject *parent) : QObject(parent)
{
	worker_ = std::make_unique<Worker>(this);
	worker_->start(QThread::LowestPriority); // never at the expense of the editor
}

ProxyBuilder::~ProxyBuilder()
{
	worker_->stop();
	worker_->wait();
}

void ProxyBuilder::request(int sourceId, const QString &path)
{
	if (path.isEmpty())
		return;
	worker_->post(Job{sourceId, path});
}

void ProxyBuilder::cancelAll()
{
	worker_->stop();
}

bool ProxyBuilder::isBusy() const
{
	return worker_->busy();
}

} // namespace harpia
