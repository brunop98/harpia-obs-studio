#pragma once

#include <QObject>
#include <QString>

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
	// than the original.
	enum class Level { Low, Balanced, High };

	struct Options {
		int maxHeight;      // cap output height; source kept if already smaller (0 = no cap)
		int crf;            // x264 constant-quality factor (lower = better/bigger)
		const char *preset; // x264 speed preset ("veryfast", "medium"…)
		int audioKbps;      // AAC bitrate when the audio must be re-encoded
	};

	static Options optionsFor(Level level);
	static QString levelName(Level level);

	explicit ShareExporter(QObject *parent = nullptr);

	// Blocking transcode of `inPath` -> `outPath`. Emits progress() periodically
	// and finished() once. On failure/cancel no valid output remains (a partial
	// file is removed by the caller/dialog). Safe to call from a worker thread.
	void run(const QString &inPath, const QString &outPath, Options opts);

	// Request cancellation from any thread; run() aborts at the next checkpoint.
	void cancel() { cancel_.store(true); }

signals:
	// percent 0-100; speed in playback-seconds encoded per wall second (2.0 = 2x
	// realtime); etaMs remaining; outBytes = current size of the output file.
	void progress(int percent, double speed, qint64 etaMs, qint64 outBytes);

	// ok=true on success. canceled=true if stopped via cancel(). error is a short
	// human message when ok is false and not canceled.
	void finished(bool ok, bool canceled, const QString &error);

private:
	std::atomic<bool> cancel_{false};
};

} // namespace harpia
