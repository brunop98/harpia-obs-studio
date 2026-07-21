#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <vector>

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

	// One kept section of the source, played at its own speed (multi-cut).
	struct Cut {
		qint64 startMs = 0;
		qint64 endMs = 0;
		double speed = 1.0;
	};

	// One recorded narration take, positioned on the OUTPUT timeline. Mixed over
	// the (already assembled) output audio in a post-process pass.
	struct Voiceover {
		QString path;          // WAV on disk
		qint64 outStartMs = 0; // where it begins on the output timeline
		qint64 srcStartMs = 0; // offset into the source (trim/split)
		qint64 playMs = 0;     // played length (0 = to end of source)
		double volume = 1.0;   // linear gain
		int fadeInMs = 15;
		int fadeOutMs = 15;
	};

	struct Options {
		Format format = Format::Mp4;
		qint64 startMs = 0;
		qint64 endMs = 0; // 0 == end of clip

		bool crop = false;
		int cropX = 0, cropY = 0, cropW = 0, cropH = 0; // source pixels

		// Playback speed multiplier (1.0 = normal, 2.0 = twice as fast). Applies to
		// every format; audio is dropped when speed != 1.
		double speed = 1.0;

		// GIF-only
		int gifFps = 15;
		int gifWidth = 0; // output width (0 == crop/source width — no downscale); height auto

		// Video-only
		int videoCrf = 20;    // x264/vp9 constant quality (lower = better)
		bool keepAudio = true; // MP4/MKV/MOV keep the (trimmed) audio; GIF/WebM silent

		// Multi-cut assembly: when non-empty, the output is these source ranges
		// played back-to-back, each at its own speed, and startMs/endMs/speed
		// above are ignored. Audio (MP4/MKV/MOV) is time-stretched per cut with
		// the pitch-preserving atempo filter and re-encoded as one AAC track.
		// GIF is not supported with cuts.
		std::vector<Cut> cuts;

		// Voiceover narration mixed over the finished output audio (video
		// containers only; ignored for GIF). When non-empty, a post-process pass
		// lays these takes onto the assembled audio.
		std::vector<Voiceover> voiceovers;
		double originalVolume = 1.0; // global gain applied to the source audio (0..2)
		bool duckOriginal = false;   // dip the source under narration (sidechain)
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
	// Multi-cut path: opts.cuts concatenated, per-cut speed, atempo'd audio.
	QString runVideoCuts(const QString &inPath, const QString &outPath, const Options &opts);

	// Post-process: mix opts.voiceovers over the just-written `videoPath`'s audio
	// in place (via VoiceoverMixer). Returns "" on success, else an error.
	QString mixVoiceover(const QString &videoPath, const Options &opts);

	std::atomic<bool> cancel_{false};
};

} // namespace harpia
