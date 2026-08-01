#pragma once

// Editing proxies: a small, short-GOP stand-in that the PREVIEW decodes from.
//
// PreviewDecoder stopped the editor freezing on a 4K clip, but it did not make
// the decode any faster -- a scrub still resolves two or three frames a second,
// because every position seeks to a keyframe up to four seconds back and rolls
// forward through a GOP of 4K frames. No amount of threading fixes that; the
// file itself is the wrong shape for scrubbing.
//
// So make a better-shaped copy. Same duration, same timestamps, same frame
// rate; a fraction of the resolution and a keyframe every twelve frames. A seek
// then decodes at most twelve small frames instead of a hundred-odd large ones.
// The original is never touched and is still what gets exported -- the proxy
// exists only between the file and the preview.
//
// Built in the background, cached on disk keyed by the source's path, size and
// modification time, and reused across sessions. Only made for files that
// actually need one; an ordinary 1080p H.264 recording scrubs fine as it is and
// gets no proxy.

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>

namespace harpia {

// Where the proxy for `sourcePath` lives. Stable for a given file: the same
// file asked twice gives the same path, and editing the file gives a different
// one, so a stale proxy is never mistaken for a current one.
QString proxyPathFor(const QString &sourcePath);

// The folder proxies are cached in (created on demand).
QString proxyCacheDir();
// Evict least-recently-USED proxies until the cache fits under maxBytes.
// Returns the bytes freed. Needed because the key includes mtime: re-encoding
// a source orphans its old proxy forever, so without pruning the cache only
// ever grows.
qint64 pruneProxyCache(qint64 maxBytes);

// Whether a source is worth proxying. Yes for anything above 1080p, and for the
// codecs that are slow to seek even at 1080p (VP8/VP9/AV1 -- WEBM downloads are
// the usual way one of those turns up here). No for the ordinary case, where a
// proxy would cost a transcode and buy nothing.
bool wantsProxy(int width, int height, const QString &codecName);

// Reads width/height/codec from a file, so a caller with only a path can ask
// wantsProxy. Returns false if the file cannot be read.
bool probeVideo(const QString &path, int *width, int *height, QString *codecName);

// Builds proxies one at a time on a worker thread. Requests for a file that
// already has a valid cached proxy complete immediately.
class ProxyBuilder : public QObject {
	Q_OBJECT
public:
	explicit ProxyBuilder(QObject *parent = nullptr);
	~ProxyBuilder() override;

	// Ask for a proxy of `path` on behalf of `sourceId`. Emits ready() -- now if
	// the proxy is already on disk, later if it has to be built. Asking twice
	// for the same source is a no-op while the first is still queued.
	void request(int sourceId, const QString &path);

	// Abandon everything queued and stop the build in progress. Called when the
	// editor closes: nobody is waiting for the result any more.
	void cancelAll();

	bool isBusy() const;

signals:
	// The proxy for this source can be used. `proxyPath` is a real file.
	void ready(int sourceId, const QString &proxyPath);
	// 0-100 for the build in progress.
	void progress(int sourceId, int percent);
	// No proxy will arrive; the caller keeps using the original file. `reason`
	// is short and human, for the log.
	void failed(int sourceId, const QString &reason);

private:
	class Worker;
	friend class Worker;

	struct Job {
		int sourceId = 0;
		QString path;
	};

	std::unique_ptr<Worker> worker_;
};

} // namespace harpia
