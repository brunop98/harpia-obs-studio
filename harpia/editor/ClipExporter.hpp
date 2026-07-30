#pragma once

#include <QObject>
#include <QString>

#include "shader/ShaderEffect.hpp"    // ShaderParam (post-processing effect)
#include "timeline/TimelineModel.hpp" // TimelineModel (Full-editing export)

#include <atomic>
#include <map>
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

	// One kept section of a source, played at its own speed (multi-cut).
	struct Cut {
		qint64 startMs = 0;
		qint64 endMs = 0;
		double speed = 1.0;
		int source = 0; // index into Options::inputs (0 = primary/canvas source)
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
		int gifColors = 256;   // palette size, 2..256. Fewer = smaller file, banding
		bool gifDither = true; // dithered palette mapping; off is smaller and blockier
		bool gifLoop = true;   // loop forever, vs play once

		// Video-only
		int videoCrf = 20;    // x264/vp9 constant quality (lower = better)
		bool keepAudio = true; // MP4/MKV/MOV keep the (trimmed) audio; GIF/WebM silent
		// Output size, in pixels. 0 = whatever the path would produce anyway (the
		// source, the cropped area, or the timeline canvas). Set BOTH or neither:
		// the dialog derives the pair so the aspect is decided in one place
		// rather than half here and half in four encode paths.
		int outWidth = 0;
		int outHeight = 0;

		// Multi-cut assembly: when non-empty, the output is these source ranges
		// played back-to-back, each at its own speed, and startMs/endMs/speed
		// above are ignored. Audio (MP4/MKV/MOV) is time-stretched per cut with
		// the pitch-preserving atempo filter and re-encoded as one AAC track.
		// GIF is not supported with cuts.
		std::vector<Cut> cuts;

		// Multi-source mixing: the source files, index 0 = the primary whose
		// resolution+framerate define the output canvas (others scale to fit,
		// letterboxed). Each Cut::source indexes this list. When it has <= 1
		// entry the single-input `inPath` path is used unchanged; crop is honored
		// only for single-source exports.
		std::vector<std::string> inputs;

		// Voiceover narration mixed over the finished output audio (video
		// containers only; ignored for GIF). When non-empty, a post-process pass
		// lays these takes onto the assembled audio.
		std::vector<Voiceover> voiceovers;
		double originalVolume = 1.0; // global gain applied to the source audio (0..2)
		bool duckOriginal = false;   // dip the source under narration (sidechain)

		// Post-processing effect chain baked into the output frames (GPU),
		// applied in order. Empty = no effect. Each layer carries the fully
		// wrapped fragment shader (see wrapShaderToy), the uniform type defs (so
		// values push with the right GLSL type), and the values.
		struct Effect {
			QString source;
			QVector<ShaderParam> paramDefs;
			QMap<QString, double> params;
		};
		std::vector<Effect> effects;

		// ---- "Full editing" multi-track timeline -------------------------
		// When the timeline has clips it REPLACES cuts/inputs/trim: every output
		// frame is composited from the visible video tracks (with per-clip
		// transform, keyframes and text) and the audio of every clip is mixed at
		// its own output position. canvasW/H and timelineFps define the output
		// format; timelineSources maps a clip's sourceId to its file path.
		TimelineModel timeline;
		std::map<int, std::string> timelineSources;
		// Transform scripts by name, source included, so the worker never has to
		// touch the user's scripts folder mid-render.
		QMap<QString, QString> timelineScripts;
		int canvasW = 0;
		int canvasH = 0;
		double timelineFps = 30.0;
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
	// Multi-source multi-cut path: cuts drawn from opts.inputs, each frame scaled
	// and letterboxed onto the primary (inputs[0]) canvas.
	QString runVideoCutsMulti(const QString &outPath, const Options &opts);
	// "Full editing" path: composite the multi-track timeline frame by frame.
	QString runTimeline(const QString &outPath, const Options &opts);

	// Post-process: mix opts.voiceovers over the just-written `videoPath`'s audio
	// in place (via VoiceoverMixer). Returns "" on success, else an error.
	QString mixVoiceover(const QString &videoPath, const Options &opts);
	// Post-process for the timeline: mix every clip's audio at its own output
	// position (video clips contribute their source audio too).
	QString mixTimelineAudio(const QString &videoPath, const Options &opts);

	std::atomic<bool> cancel_{false};
};

} // namespace harpia
