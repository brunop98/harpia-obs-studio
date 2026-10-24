#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>

namespace harpia {

// Produces a compressed, share-optimized copy of a recording using the bundled
// FFmpeg libraries (libx264 video + AAC audio, +faststart MP4). The original
// file is never touched; the copy is written to `outPath` (caller picks the
// `<name>_shared.mp4` path). The transcode runs synchronously in run() — call it
// on a worker thread — and reports progress via queued signals so a modal dialog
// can show a bar, speed, ETA and the growing output size, and can cancel.
class ShareExporter : public QObject {
	Q_OBJECT
public:
	// Quality/size trade-off. Low = smallest (mobile data / WhatsApp), Balanced
	// = best quality-per-byte (default), High = better quality, still smaller
	// than the original. Discord uses Balanced quality and size-limited parts.
	enum class Level { Low, Balanced, High, Discord };

	struct Options {
		int maxHeight;      // cap output height; source kept if already smaller (0 = no cap)
		int crf;            // x264 constant-quality factor (lower = better/bigger)
		const char *preset; // x264 speed preset ("veryfast", "medium"…)
		int audioKbps;      // AAC bitrate when the audio must be re-encoded
		// Keyframe spacing, in frames. 0 = the sharing default of two seconds.
		// Editing proxies set this small: the whole point of a proxy is that a
		// seek never has to decode far, and that is decided here.
		int gopFrames = 0;
		// Skip the audio track entirely. For a proxy, which is only ever asked
		// for pictures -- the real file still supplies the sound.
		bool dropAudio = false;
		// 0 = one file. Otherwise `outPath` is a NEW folder of numbered MP4s.
		qint64 maxPartBytes = 0;
	};

	static Options optionsFor(Level level);
	static QString levelName(Level level);

	explicit ShareExporter(QObject *parent = nullptr);

	// Blocking transcode of `inPath` -> `outPath` (a new folder for Discord).
	// Emits progress() periodically
	// and finished() once. On failure/cancel no valid output remains (a partial
	// single file is removed by the caller/dialog; Discord cleans up its own
	// temporary output). Safe to call from a worker thread.
	void run(const QString &inPath, const QString &outPath, Options opts);
	// Read after finished()/joining the worker. Empty on failure/cancel.
	QStringList outputFiles() const { return outputFiles_; }

	// Request cancellation from any thread; run() aborts at the next checkpoint.
	void cancel() { cancel_.store(true); }

signals:
	// percent 0-100; speed in playback-seconds encoded per wall second (2.0 = 2x
	// realtime); etaMs remaining; outBytes = current size of the output file.
	void progress(int percent, double speed, qint64 etaMs, qint64 outBytes);

	// ok=true on success. canceled=true if stopped via cancel(). error is a short
	// human message when ok is false and not canceled.
	void finished(bool ok, bool canceled, const QString &error);
	void statusChanged(const QString &message);

private:
	struct Result {
		bool ok = false, canceled = false;
		QString error;
	};
	Result transcode(const QString &inPath, const QString &outPath, Options opts);
	Result splitForSharing(const QString &inPath, const QString &directory, Options opts);
	std::atomic<bool> cancel_{false};
	QStringList outputFiles_;
	int transcodeProgressMax_ = 100;
};

} // namespace harpia
