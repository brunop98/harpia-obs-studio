#pragma once

#include <QObject>
#include <QString>

#include <atomic>

namespace harpia {

// Non-destructive export of a trimmed (and optionally cropped) section of a clip
// to a new file. Video containers (MP4/MKV/MOV via H.264+AAC, WebM via VP9) are
// handled here directly with libav; GIF output is delegated to GifEncoder (which
// uses libavfilter's palettegen/paletteuse for good colors). Runs synchronously
// in run() — call it on a worker thread — and reports progress/cancel.
class ClipExporter : public QObject {
	Q_OBJECT
public:
	enum class Format { Mp4, Mkv, Mov, WebM, Gif };

	struct Options {
		Format format = Format::Mp4;
		qint64 startMs = 0;
		qint64 endMs = 0; // 0 == end of clip

		bool crop = false;
		int cropX = 0, cropY = 0, cropW = 0, cropH = 0; // source pixels

		// GIF-only
		int gifFps = 15;
		int gifWidth = 640; // output width (0 == crop/source width); height auto

		// Video-only
		int videoCrf = 20;    // x264/vp9 constant quality (lower = better)
		bool keepAudio = true; // MP4/MKV/MOV keep the (trimmed) audio; GIF/WebM silent
	};

	static QString extensionFor(Format f); // "mp4"/"mkv"/"mov"/"webm"/"gif"
	static bool webmAvailable();           // libvpx-vp9 present in this build

	explicit ClipExporter(QObject *parent = nullptr);

	void run(const QString &inPath, const QString &outPath, const Options &opts);
	void cancel() { cancel_.store(true); }

signals:
	void progress(int percent, qint64 etaMs, qint64 outBytes);
	void finished(bool ok, bool canceled, const QString &error);

private:
	// Video-container path (MP4/MKV/MOV/WebM). Returns "" on success, else error.
	QString runVideo(const QString &inPath, const QString &outPath, const Options &opts);

	std::atomic<bool> cancel_{false};
};

} // namespace harpia
