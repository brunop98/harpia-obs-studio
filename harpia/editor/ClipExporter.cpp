#include "ClipExporter.hpp"

#include "AudioRetimer.hpp"
#include "FrameSeeker.hpp"
#include "StillImage.hpp"
#include "GifEncoder.hpp"
#include "TimelineAudio.hpp"
#include "VoiceoverMixer.hpp"
#include "component/ShaderComponent.hpp"
#include "script/TransformScript.hpp"
#include "shader/ShaderRenderer.hpp"
#include "timeline/TimelineCompositor.hpp"

#include <QImage>
#include <QSize>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace harpia {

QString ClipExporter::extensionFor(Format f)
{
	switch (f) {
	case Format::Mkv:
		return QStringLiteral("mkv");
	case Format::Mov:
		return QStringLiteral("mov");
	case Format::WebM:
		return QStringLiteral("webm");
	case Format::Gif:
		return QStringLiteral("gif");
	case Format::Mp4:
	default:
		return QStringLiteral("mp4");
	}
}

bool ClipExporter::webmAvailable()
{
	return avcodec_find_encoder_by_name("libvpx-vp9") != nullptr;
}

ClipExporter::ClipExporter(QObject *parent) : QObject(parent) {}

namespace {

int evenDown(int v)
{
	return v & ~1;
}

int64_t durationMsOf(AVFormatContext *fmt)
{
	return fmt && fmt->duration > 0 ? fmt->duration / (AV_TIME_BASE / 1000) : 0;
}

// Everything freed in the destructor so early returns clean up.
struct VideoState {
	AVFormatContext *ifmt = nullptr;
	AVFormatContext *ofmt = nullptr;
	AVCodecContext *vdec = nullptr;
	AVCodecContext *venc = nullptr;
	SwsContext *toYuv = nullptr; // only if the source isn't yuv420p already
	// Output scaling, when the chosen size is not the size the path produces.
	// Applied at the ONE point every path shares -- just before the encoder --
	// rather than in each path's own decode/crop/composite arithmetic, which is
	// four places to get the aspect wrong instead of one.
	SwsContext *toOut = nullptr;
	AVFrame *outScaled = nullptr;
	AVFrame *fullYuv = nullptr;  // full-frame yuv420p scratch (for non-yuv sources)
	AVFrame *cropFrame = nullptr; // cropped yuv420p frame fed to the encoder
	AVStream *vOut = nullptr;
	AVStream *aOut = nullptr; // audio stream-copy (optional)
	bool headerWritten = false;

	// The decoded frame in the encoder's pixel format, converting once and
	// reusing the buffer. Two export paths spelled this out identically (modulo
	// where the lines happened to wrap); it belongs here because fullYuv and
	// toYuv are this struct's own members and nobody outside should be reaching
	// for them.
	AVFrame *ensureEncodeFormat(AVFrame *f, AVPixelFormat want)
	{
		if (!f || AVPixelFormat(f->format) == want)
			return f;
		if (!fullYuv) {
			fullYuv = av_frame_alloc();
			fullYuv->format = want;
			fullYuv->width = vdec->width;
			fullYuv->height = vdec->height;
			av_frame_get_buffer(fullYuv, 0);
		}
		if (!toYuv)
			toYuv = sws_getContext(vdec->width, vdec->height, AVPixelFormat(f->format),
					       vdec->width, vdec->height, want, SWS_BILINEAR, nullptr,
					       nullptr, nullptr);
		av_frame_make_writable(fullYuv); // the encoder may still hold a ref
		sws_scale(toYuv, f->data, f->linesize, 0, vdec->height, fullYuv->data,
			  fullYuv->linesize);
		return fullYuv;
	}

	// The frame the encoder should actually receive. Identity unless the output
	// size differs from the frame's, in which case a yuv->yuv rescale runs into
	// a reused frame. Returns `in` unchanged on any allocation failure: a
	// wrongly sized frame is refused by the encoder, which is a clear error,
	// where a silent null would look like a truncated export.
	AVFrame *scaleForEncode(AVFrame *in)
	{
		if (!in || !venc)
			return in;
		// Format as well as size: with 4:4:4 chosen, a path that still produces
		// 4:2:0 has to be converted here or the encoder rejects the frame.
		if (in->width == venc->width && in->height == venc->height &&
		    AVPixelFormat(in->format) == venc->pix_fmt)
			return in;
		if (!toOut) {
			toOut = sws_getContext(in->width, in->height, AVPixelFormat(in->format),
					       venc->width, venc->height, venc->pix_fmt,
					       SWS_BICUBIC, nullptr, nullptr, nullptr);
			outScaled = av_frame_alloc();
			if (!toOut || !outScaled)
				return in;
			outScaled->format = venc->pix_fmt;
			outScaled->width = venc->width;
			outScaled->height = venc->height;
			if (av_frame_get_buffer(outScaled, 32) < 0)
				return in;
		}
		if (!outScaled)
			return in;
		sws_scale(toOut, in->data, in->linesize, 0, in->height, outScaled->data,
			  outScaled->linesize);
		outScaled->pts = in->pts;
		return outScaled;
	}

	~VideoState()
	{
		if (toYuv)
			sws_freeContext(toYuv);
		if (toOut)
			sws_freeContext(toOut);
		if (outScaled)
			av_frame_free(&outScaled);
		if (fullYuv)
			av_frame_free(&fullYuv);
		if (cropFrame)
			av_frame_free(&cropFrame);
		if (vdec)
			avcodec_free_context(&vdec);
		if (venc)
			avcodec_free_context(&venc);
		if (ofmt) {
			if (ofmt->pb && !(ofmt->oformat->flags & AVFMT_NOFILE))
				avio_closep(&ofmt->pb);
			avformat_free_context(ofmt);
		}
		if (ifmt)
			avformat_close_input(&ifmt);
	}
};

// ---- The output side, once -------------------------------------------------
//
// All four export paths — plain trim, multi-cut, multi-source and the Full
// editing timeline — end in the same place: an H.264 or VP9 encoder writing
// into a container. Each used to spell that out itself, which is how the
// timeline path ended up as the only one NOT asking for +faststart on its MP4s.
// Four copies of a setup means the fifth thing you fix reaches one of them.
//
// What genuinely differs between the paths is the time base, the frame rate and
// the GOP length, so those are arguments; everything else is the same by
// definition and now only exists once.

// The pixel format the encoder runs in, and therefore the format every path has
// to hand it. One function so the encoder, the rescaler and the compositor's
// RGBA conversion cannot disagree -- a mismatch there is not a quality problem, it
// is a refused frame.
AVPixelFormat encodePixFmt(const ClipExporter::Options &opts)
{
	// VP9 in this build is 4:2:0 only; asking for 444 there would fail to open
	// rather than look better.
	if (opts.chroma444 && opts.format != ClipExporter::Format::WebM)
		return AV_PIX_FMT_YUV444P;
	return AV_PIX_FMT_YUV420P;
}

// The size the encoder should be opened at, given what the path would otherwise
// have produced. Even numbers, because yuv420p subsamples by two and an odd
// dimension is rejected outright by both encoders.
QSize encodeSize(const ClipExporter::Options &opts, int w, int h)
{
	if (opts.outWidth <= 0 || opts.outHeight <= 0 || w <= 0 || h <= 0)
		return QSize(w, h);
	return QSize(std::max(2, opts.outWidth & ~1), std::max(2, opts.outHeight & ~1));
}

// Create the encoder, the output container and the video stream. On success
// `s.venc`, `s.ofmt` and `s.vOut` are live. Returns "" or an error to show.
QString openVideoEncoder(VideoState &s, const ClipExporter::Options &opts, const QByteArray &outPath,
			 int w, int h, AVRational timeBase, AVRational frameRate, int gopSize)
{
	const bool webm = opts.format == ClipExporter::Format::WebM;
	const AVCodec *vc = avcodec_find_encoder_by_name(webm ? "libvpx-vp9" : "libx264");
	if (!vc)
		return QStringLiteral("The output video encoder is not available in this build.");
	s.venc = avcodec_alloc_context3(vc);
	if (!s.venc)
		return QStringLiteral("Could not allocate the video encoder.");
	// Here rather than at the four call sites: every path passes the size it
	// would naturally produce, and the chosen output size overrides it in one
	// place. scaleForEncode then rescales whatever arrives to match.
	const QSize enc = encodeSize(opts, w, h);
	s.venc->width = enc.width();
	s.venc->height = enc.height();
	s.venc->pix_fmt = encodePixFmt(opts);
	s.venc->time_base = timeBase;
	s.venc->framerate = frameRate;
	s.venc->gop_size = gopSize;
	// Say what the colours MEAN. Left unset, a player has to guess the matrix
	// and primaries, and the usual guess for anything over SD is what we write
	// anyway -- but the guess for small frames is BT.601, which shifts every
	// colour in a downscaled export. Cheap to state, and then nobody guesses.
	s.venc->colorspace = AVCOL_SPC_BT709;
	s.venc->color_primaries = AVCOL_PRI_BT709;
	s.venc->color_trc = AVCOL_TRC_BT709;
	s.venc->color_range = AVCOL_RANGE_MPEG;

	// The container has to exist before the encoder opens: whether it wants a
	// global header changes a flag on the encoder.
	if (avformat_alloc_output_context2(&s.ofmt, nullptr, nullptr, outPath.constData()) < 0 ||
	    !s.ofmt)
		return QStringLiteral("Could not create the output file.");
	if (s.ofmt->oformat->flags & AVFMT_GLOBALHEADER)
		s.venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	using Effort = ClipExporter::Options::Effort;
	if (webm) {
		s.venc->bit_rate = 0; // constant-quality VP9
		av_opt_set_int(s.venc->priv_data, "crf", opts.videoCrf, 0);
		// cpu-used counts the other way from x264's preset: higher is faster
		// and worse. 5 is a streaming setting and was hard-coded here.
		const int cpuUsed = opts.effort == Effort::Best      ? 1
				    : opts.effort == Effort::Fast    ? 5
								     : 2;
		av_opt_set(s.venc->priv_data, "deadline",
			   opts.effort == Effort::Fast ? "realtime" : "good", 0);
		av_opt_set_int(s.venc->priv_data, "cpu-used", cpuUsed, 0);
		av_opt_set(s.venc->priv_data, "row-mt", "1", 0);
	} else {
		// "veryfast" is a LIVE preset: it exists so an encoder can keep up with
		// frames arriving in real time. An export is not real time, and paying
		// a slower preset buys real detail at the same CRF.
		const char *preset = opts.effort == Effort::Best     ? "slow"
				     : opts.effort == Effort::Fast   ? "veryfast"
								     : "medium";
		av_opt_set(s.venc->priv_data, "preset", preset, 0);
		// 4:4:4 needs its own profile; asking for "high" with a 444 pixel
		// format is a refusal, not a downgrade.
		av_opt_set(s.venc->priv_data, "profile",
			   s.venc->pix_fmt == AV_PIX_FMT_YUV444P ? "high444" : "high", 0);
		av_opt_set_int(s.venc->priv_data, "crf", opts.videoCrf, 0);
	}
	if (avcodec_open2(s.venc, vc, nullptr) < 0)
		return QStringLiteral("Could not open the video encoder.");

	s.vOut = avformat_new_stream(s.ofmt, nullptr);
	if (!s.vOut || avcodec_parameters_from_context(s.vOut->codecpar, s.venc) < 0)
		return QStringLiteral("Could not create the output video stream.");
	s.vOut->time_base = s.venc->time_base;
	return {};
}

// Open the file and write the container header. Call once every stream has been
// added — a stream added after this is not in the file.
//
// +faststart moves the MP4 index to the front so the file plays before it has
// fully downloaded. Three of the four paths asked for it and the timeline path
// did not, which is precisely the kind of difference that survives in copies.
QString openOutputFile(VideoState &s, const ClipExporter::Options &opts, const QByteArray &outPath)
{
	if (!(s.ofmt->oformat->flags & AVFMT_NOFILE) &&
	    avio_open(&s.ofmt->pb, outPath.constData(), AVIO_FLAG_WRITE) < 0)
		return QStringLiteral("Could not open the output file for writing.");
	AVDictionary *mux = nullptr;
	if (opts.format == ClipExporter::Format::Mp4 || opts.format == ClipExporter::Format::Mov)
		av_dict_set(&mux, "movflags", "+faststart", 0);
	const int hr = avformat_write_header(s.ofmt, &mux);
	av_dict_free(&mux);
	if (hr < 0)
		return QStringLiteral("Could not start writing the output file.");
	s.headerWritten = true;
	return {};
}

// Applies a compiled post-processing shader to output frames on the export
// worker thread. YUV -> RGBA -> shader -> the frame's own YUV format, in place
// on the frame about to be encoded, so the baked file matches the preview.
struct ShaderPass {
	ShaderRenderer renderer;
	SwsContext *toRgba = nullptr;
	SwsContext *toYuv = nullptr;
	int w = 0, h = 0;
	bool active = false;
	QVector<QMap<QString, double>> perLayer; // values per effect layer

	~ShaderPass()
	{
		if (toRgba)
			sws_freeContext(toRgba);
		if (toYuv)
			sws_freeContext(toYuv);
	}

	// Compile + ready the GPU. Returns an error string (empty on success). Leaves
	// active=false when the Options carry no effect.
	QString init(const ClipExporter::Options &o)
	{
		if (o.effects.empty())
			return QString();
		if (!renderer.ensureGl())
			return QStringLiteral("The effect needs OpenGL 3.3, which isn't available here: %1")
				.arg(renderer.lastError());
		QVector<ShaderLayerSource> layers;
		layers.reserve(int(o.effects.size()));
		perLayer.clear();
		for (const auto &e : o.effects) {
			layers.append({e.source, e.paramDefs});
			perLayer.append(e.params);
		}
		QString err;
		if (!renderer.setChain(layers, &err))
			return QStringLiteral("The effect shader failed to compile:\n%1").arg(err);
		active = true;
		return QString();
	}

	// Filter one writable YUV frame in place, whatever its chroma layout.
	// Returns false only on an unexpected sws failure.
	bool process(AVFrame *f, float tSec, int frame)
	{
		if (!active || !f)
			return true;
		const int W = f->width, H = f->height;
		if (!toRgba || W != w || H != h) {
			if (toRgba)
				sws_freeContext(toRgba);
			if (toYuv)
				sws_freeContext(toYuv);
			toRgba = sws_getContext(W, H, (AVPixelFormat)f->format, W, H, AV_PIX_FMT_RGBA,
						SWS_BILINEAR, nullptr, nullptr, nullptr);
			// Back into the frame's OWN format, not a hard-coded 4:2:0: this
			// writes over the frame in place, and with 4:4:4 chosen a 4:2:0
			// write would scribble past the U/V planes.
			toYuv = sws_getContext(W, H, AV_PIX_FMT_RGBA, W, H, (AVPixelFormat)f->format,
					       SWS_BILINEAR, nullptr, nullptr, nullptr);
			w = W;
			h = H;
		}
		if (!toRgba || !toYuv)
			return false;

		QImage img(W, H, QImage::Format_RGBA8888);
		uint8_t *dst[4] = {img.bits(), nullptr, nullptr, nullptr};
		int dstStride[4] = {(int)img.bytesPerLine(), 0, 0, 0};
		sws_scale(toRgba, f->data, f->linesize, 0, H, dst, dstStride);

		QImage out = renderer.apply(img, tSec, frame, perLayer);
		if (out.format() != QImage::Format_RGBA8888)
			out = out.convertToFormat(QImage::Format_RGBA8888);

		// The decoded frame may be a read-only, decoder-owned buffer (yuv420p
		// sources pass through un-copied); make it writable before overwriting it.
		if (av_frame_make_writable(f) < 0)
			return false;
		const uint8_t *src[4] = {out.constBits(), nullptr, nullptr, nullptr};
		int srcStride[4] = {(int)out.bytesPerLine(), 0, 0, 0};
		sws_scale(toYuv, src, srcStride, 0, H, f->data, f->linesize);
		return true;
	}
};

// The tail every export path shares.
//
// Bake the effect chain, rescale to the encoder's size and format, send, drain,
// write. This was spelled out four times -- three of them byte for byte
// identical, the fourth differing only in the name of its packet -- which is
// how the file came to have four encoder setups before openVideoEncoder pulled
// those together. Adding the output rescale meant four edits; the next thing
// will mean one.
//
// An object rather than a function so the shader and its frame counter, which
// have to live as long as the encode does, come along with it instead of being
// three more locals every caller has to remember to declare.
struct VideoSink {
	VideoSink(VideoState &st, AVPacket *p) : s(st), pkt(p) {}

	VideoState &s;
	AVPacket *pkt; // scratch for the drained packets; owned by the caller
	ShaderPass shader;
	int shaderFrame = 0;

	// Compile the effect chain, if there is one. Callers that bake no shaders
	// (the timeline composites them as components) simply do not call this, and
	// the pass stays inactive.
	QString initShader(const ClipExporter::Options &opts) { return shader.init(opts); }

	bool operator()(AVFrame *f)
	{
		// Downscale FIRST, then shade. The effects are resolution-independent
		// (they read normalized coordinates), so shading a 4K frame that is
		// about to become 1080p ran the whole chain on four times the pixels
		// that survive -- per frame, for the whole export.
		f = s.scaleForEncode(f); // no-op unless an output size was chosen
		if (f && shader.active)
			shader.process(f, float(f->pts * av_q2d(s.venc->time_base)), shaderFrame++);
		if (avcodec_send_frame(s.venc, f) < 0)
			return false;
		while (true) {
			const int r = avcodec_receive_packet(s.venc, pkt);
			if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
				break;
			if (r < 0)
				return false;
			av_packet_rescale_ts(pkt, s.venc->time_base, s.vOut->time_base);
			pkt->stream_index = s.vOut->index;
			if (av_interleaved_write_frame(s.ofmt, pkt) < 0)
				return false;
			av_packet_unref(pkt);
		}
		return true;
	}
};


} // namespace

void ClipExporter::run(const QString &inPath, const QString &outPath, const Options &opts)
{
	cancel_.store(false);

	// However this returns -- finished, cancelled, or any of the dozen early
	// errors below -- give back the GL a shader component created on THIS
	// thread, here, while the thread is still fully itself.
	//
	// A scope guard rather than a call at the end, because there is no single
	// end: run() returns from a dozen places, and the one path that forgets is
	// the one that leaks a GPU context per export. It used to be left to the
	// thread_local destructor at thread exit, which is where v0.1.286 aborted --
	// with the export finished and a good file on disk. See
	// ShaderComponents::releaseThreadResources.
	struct ReleaseGl {
		~ReleaseGl() { ShaderComponents::releaseThreadResources(); }
	} releaseGl;

	if (opts.format == Format::Gif) {
		if (!opts.cuts.empty()) {
			emit finished(false, false,
				      QStringLiteral("GIF export is not available in Multi-Cut mode yet — "
						     "choose MP4, MKV or MOV."));
			return;
		}
		// A timeline can't be handed to the palette encoder directly (it reads one
		// input file), so render the composite to a temp video first and GIF that.
		QString gifIn = inPath;
		QTemporaryDir tlTmp;
		Options gifOpts = opts;
		if (!opts.timeline.isEmpty()) {
			if (!tlTmp.isValid()) {
				emit finished(false, false,
					      QStringLiteral("Could not create a temporary folder."));
				return;
			}
			gifIn = tlTmp.filePath(QStringLiteral("timeline.mp4"));
			Options pre = opts;
			pre.format = Format::Mp4;
			const QString rerr = runTimeline(gifIn, pre);
			if (cancel_.load()) {
				emit finished(false, true, QString());
				return;
			}
			if (!rerr.isEmpty()) {
				emit finished(false, false, rerr);
				return;
			}
			// The render already applied trim/crop/speed, so the GIF pass just
			// re-encodes the whole thing.
			gifOpts.startMs = 0;
			gifOpts.endMs = 0;
			gifOpts.crop = false;
			gifOpts.speed = 1.0;
		}
		// Delegate to the avfilter-based GIF encoder; bridge its callbacks to our
		// Qt signals.
		GifEncoder::Params gp;
		gp.startMs = gifOpts.startMs;
		gp.endMs = gifOpts.endMs;
		gp.crop = gifOpts.crop;
		gp.cropX = gifOpts.cropX;
		gp.cropY = gifOpts.cropY;
		gp.cropW = gifOpts.cropW;
		gp.cropH = gifOpts.cropH;
		gp.fps = gifOpts.gifFps;
		// A chosen output size applies to GIF too; gifWidth stays as the
		// explicit override so nothing that already set it changes behaviour.
		gp.width = gifOpts.gifWidth > 0 ? gifOpts.gifWidth : gifOpts.outWidth;
		gp.colors = gifOpts.gifColors;
		gp.dither = gifOpts.gifDither;
		gp.loop = gifOpts.gifLoop;
		gp.speed = gifOpts.speed;
		QString err;
		const bool ok = GifEncoder::encode(
			gifIn, outPath, gp, [this]() { return cancel_.load(); },
			[this](int pct, qint64 eta, qint64 bytes) { emit progress(pct, eta, bytes); }, &err);
		if (cancel_.load())
			emit finished(false, true, QString());
		else
			emit finished(ok, false, ok ? QString() : err);
		return;
	}

	QString err;
	const bool timeline = !opts.timeline.isEmpty();
	if (timeline)
		err = runTimeline(outPath, opts); // Full editing: composited multi-track
	else if (opts.cuts.empty())
		err = runVideo(inPath, outPath, opts);
	else if (opts.inputs.size() > 1)
		err = runVideoCutsMulti(outPath, opts); // cuts drawn from several sources
	else
		err = runVideoCuts(inPath, outPath, opts);

	// Audio is mixed in a second pass over the finished file (video containers
	// only; GIF returned above). The timeline mixes every clip at its own
	// position; the other modes mix the voiceover takes over the source audio.
	if (err.isEmpty() && timeline && !cancel_.load()) {
		emit progress(97, 0, 0);
		err = mixTimelineAudio(outPath, opts);
	} else if (err.isEmpty() && !opts.voiceovers.empty() && !cancel_.load()) {
		emit progress(97, 0, 0);
		err = mixVoiceover(outPath, opts);
	}

	if (cancel_.load())
		emit finished(false, true, QString());
	else
		emit finished(err.isEmpty(), false, err);
}

QString ClipExporter::mixVoiceover(const QString &videoPath, const Options &opts)
{
	std::vector<VoiceoverMixer::Take> takes;
	takes.reserve(opts.voiceovers.size());
	for (const Voiceover &v : opts.voiceovers)
		takes.push_back(
			{v.path, v.outStartMs, v.srcStartMs, v.playMs, v.volume, v.fadeInMs, v.fadeOutMs});
	return VoiceoverMixer::mix(videoPath, opts.originalVolume, opts.duckOriginal, takes, &cancel_);
}

QString ClipExporter::runVideo(const QString &inPath, const QString &outPath, const Options &opts)
{
	VideoState s;
	const QByteArray in = inPath.toUtf8();
	const QByteArray out = outPath.toUtf8();

	if (avformat_open_input(&s.ifmt, in.constData(), nullptr, nullptr) < 0)
		return QStringLiteral("Could not open the source file.");
	if (avformat_find_stream_info(s.ifmt, nullptr) < 0)
		return QStringLiteral("Could not read the source file.");

	const int vIdx = av_find_best_stream(s.ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vIdx < 0)
		return QStringLiteral("The file has no video track.");
	AVStream *vin = s.ifmt->streams[vIdx];

	const double speed = opts.speed > 0.01 ? opts.speed : 1.0;
	const bool container_h264 = opts.format != Format::WebM;
	// Speeding the video up would desync stream-copied audio (and pitch-shift it),
	// so audio is only kept at normal speed.
	const bool wantAudio = opts.keepAudio && container_h264 && speed == 1.0;
	int aIdx = wantAudio ? av_find_best_stream(s.ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) : -1;
	AVStream *ain = (aIdx >= 0) ? s.ifmt->streams[aIdx] : nullptr;
	// Only stream-copy AAC/MP3 audio (MP4/MKV/MOV-friendly); otherwise drop it.
	if (ain && ain->codecpar->codec_id != AV_CODEC_ID_AAC && ain->codecpar->codec_id != AV_CODEC_ID_MP3) {
		aIdx = -1;
		ain = nullptr;
	}

	// ---- Video decoder ----
	const AVCodec *vdc = avcodec_find_decoder(vin->codecpar->codec_id);
	if (!vdc)
		return QStringLiteral("Unsupported source video codec.");
	s.vdec = avcodec_alloc_context3(vdc);
	if (!s.vdec || avcodec_parameters_to_context(s.vdec, vin->codecpar) < 0)
		return QStringLiteral("Could not set up the video decoder.");
	s.vdec->pkt_timebase = vin->time_base;
	if (avcodec_open2(s.vdec, vdc, nullptr) < 0)
		return QStringLiteral("Could not open the video decoder.");

	// ---- Resolve crop (default = full frame), even-aligned & clamped ----
	int cx = 0, cy = 0, cw = s.vdec->width, ch = s.vdec->height;
	if (opts.crop && opts.cropW > 0 && opts.cropH > 0) {
		cx = std::clamp(opts.cropX, 0, s.vdec->width - 2);
		cy = std::clamp(opts.cropY, 0, s.vdec->height - 2);
		cw = std::min(opts.cropW, s.vdec->width - cx);
		ch = std::min(opts.cropH, s.vdec->height - cy);
	}
	cx = evenDown(cx);
	cy = evenDown(cy);
	cw = std::max(2, evenDown(cw));
	ch = std::max(2, evenDown(ch));
	// No crop selected (the default) -> decoded frames are fed straight to
	// the encoder; the per-frame full-image copy is skipped entirely.
	const bool cropNeeded = (cx != 0 || cy != 0 || cw != s.vdec->width || ch != s.vdec->height);

	// ---- Video encoder + output container ----
	// The source's own time base and frame rate: a straight trim re-encodes on
	// the same clock it decoded from.
	const AVRational fr = av_guess_frame_rate(s.ifmt, vin, nullptr);
	if (const QString e = openVideoEncoder(
		    s, opts, out, cw, ch, vin->time_base, fr,
		    (fr.num > 0 && fr.den > 0) ? std::max(1, int(av_q2d(fr) * 2.0)) : 60);
	    !e.isEmpty())
		return e;

	if (ain) {
		s.aOut = avformat_new_stream(s.ofmt, nullptr);
		if (!s.aOut || avcodec_parameters_copy(s.aOut->codecpar, ain->codecpar) < 0)
			return QStringLiteral("Could not copy the audio stream.");
		s.aOut->codecpar->codec_tag = 0;
		s.aOut->time_base = ain->time_base;
	}

	// ---- Scratch frames ----
	s.cropFrame = av_frame_alloc();
	// The encoder's format, not a hard-coded 4:2:0. Every intermediate frame in
	// this path has to follow it: converting the decoded picture down to 4:2:0
	// here and letting scaleForEncode widen it back to 4:4:4 later would throw
	// the chroma away first and then pretend to keep it -- which is exactly what
	// happened, and made a CRF-0 "lossless" export not lossless at all.
	s.cropFrame->format = encodePixFmt(opts);
	s.cropFrame->width = cw;
	s.cropFrame->height = ch;
	if (av_frame_get_buffer(s.cropFrame, 0) < 0)
		return QStringLiteral("Out of memory.");

	// ---- Open + write header (+faststart for MP4) ----
	if (const QString e = openOutputFile(s, opts, out); !e.isEmpty())
		return e;

	// ---- Trim range in each stream's time base ----
	const int64_t startV = av_rescale_q(opts.startMs, {1, 1000}, vin->time_base);
	const int64_t endV = (opts.endMs > 0) ? av_rescale_q(opts.endMs, {1, 1000}, vin->time_base) : INT64_MAX;
	const int64_t startA = ain ? av_rescale_q(opts.startMs, {1, 1000}, ain->time_base) : 0;
	const int64_t endA = ain ? ((opts.endMs > 0) ? av_rescale_q(opts.endMs, {1, 1000}, ain->time_base)
						     : INT64_MAX)
				 : 0;

	// Seek to the keyframe at/before the start so we don't decode from 0.
	if (opts.startMs > 0)
		av_seek_frame(s.ifmt, vIdx, startV, AVSEEK_FLAG_BACKWARD);
	avcodec_flush_buffers(s.vdec);

	const double totalMs = double((opts.endMs > 0 ? opts.endMs : (durationMsOf(s.ifmt))) - opts.startMs);
	const auto startWall = std::chrono::steady_clock::now();
	auto lastEmit = startWall;
	double processedMs = 0.0;

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	AVPacket *outPkt = av_packet_alloc();
	bool errored = false;
	bool videoDone = false;

	// Encoder pts, rebased to the trim start and compressed by the speed factor
	// (2× speed => timestamps half as far apart => plays twice as fast), kept
	// strictly increasing.
	int64_t lastEncPts = -1;
	auto scaledPts = [&](int64_t pts) -> int64_t {
		int64_t v = (int64_t)llround(double(pts - startV) / speed);
		if (v <= lastEncPts)
			v = lastEncPts + 1;
		lastEncPts = v;
		return v;
	};

	auto ensureYuvFull = [&](AVFrame *f) { return s.ensureEncodeFormat(f, encodePixFmt(opts)); };

	VideoSink encodeVideo(s, outPkt);
	if (QString e = encodeVideo.initShader(opts); !e.isEmpty())
		return e;

	while (!errored && !videoDone) {
		if (cancel_.load())
			break;
		if (av_read_frame(s.ifmt, pkt) < 0)
			break;

		if (pkt->stream_index == vIdx) {
			if (avcodec_send_packet(s.vdec, pkt) >= 0) {
				while (avcodec_receive_frame(s.vdec, frame) >= 0) {
					const int64_t pts =
						frame->best_effort_timestamp != AV_NOPTS_VALUE
							? frame->best_effort_timestamp
							: 0;
					if (pts < startV) {
						av_frame_unref(frame);
						continue;
					}
					if (pts > endV) {
						videoDone = true;
						av_frame_unref(frame);
						break;
					}
					// Crop into the encoder frame only when a crop is set —
					// otherwise the decoded frame goes straight through.
					AVFrame *yf = ensureYuvFull(frame);
					AVFrame *toEnc = yf;
					if (cropNeeded) {
						if (av_frame_make_writable(s.cropFrame) < 0) {
							errored = true;
							av_frame_unref(frame);
							break;
						}
						const uint8_t *src[4] = {
							yf->data[0] + cy * yf->linesize[0] + cx,
							yf->data[1] + (cy / 2) * yf->linesize[1] + (cx / 2),
							yf->data[2] + (cy / 2) * yf->linesize[2] + (cx / 2),
							nullptr};
						av_image_copy(s.cropFrame->data, s.cropFrame->linesize, src,
							      yf->linesize, encodePixFmt(opts), cw, ch);
						toEnc = s.cropFrame;
					}
					toEnc->pts = scaledPts(pts);
					processedMs = std::max(processedMs, (pts - startV) * av_q2d(vin->time_base) *
										     1000.0);
					if (!encodeVideo(toEnc)) {
						errored = true;
						av_frame_unref(frame);
						break;
					}
					av_frame_unref(frame);
				}
			}
		} else if (ain && pkt->stream_index == aIdx) {
			if (pkt->pts != AV_NOPTS_VALUE && pkt->pts >= startA && pkt->pts <= endA) {
				pkt->stream_index = s.aOut->index;
				// Rebase timestamps so the trimmed clip starts at 0.
				if (pkt->pts != AV_NOPTS_VALUE)
					pkt->pts -= startA;
				if (pkt->dts != AV_NOPTS_VALUE)
					pkt->dts -= startA;
				av_packet_rescale_ts(pkt, ain->time_base, s.aOut->time_base);
				pkt->pos = -1;
				if (av_interleaved_write_frame(s.ofmt, pkt) < 0)
					errored = true;
			}
		}
		av_packet_unref(pkt);

		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEmit).count() >= 200) {
			lastEmit = now;
			const double wall =
				std::chrono::duration_cast<std::chrono::milliseconds>(now - startWall).count() /
				1000.0;
			const int pct = totalMs > 0 ? std::clamp(int(processedMs / totalMs * 100.0), 0, 99) : 0;
			const double speed = wall > 0.05 ? (processedMs / 1000.0) / wall : 0.0;
			const qint64 eta = (totalMs > 0 && speed > 0.01)
						   ? qint64((totalMs - processedMs) / 1000.0 / speed * 1000.0)
						   : 0;
			emit progress(pct, eta, s.ofmt->pb ? avio_tell(s.ofmt->pb) : 0);
		}
	}

	if (!errored && !cancel_.load()) {
		avcodec_send_packet(s.vdec, nullptr);
		while (avcodec_receive_frame(s.vdec, frame) >= 0) {
			const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
						    ? frame->best_effort_timestamp
						    : 0;
			if (pts >= startV && pts <= endV) {
				AVFrame *yf = ensureYuvFull(frame);
				AVFrame *toEnc = yf;
				bool ok = true;
				if (cropNeeded) {
					if (av_frame_make_writable(s.cropFrame) == 0) {
						const uint8_t *src[4] = {
							yf->data[0] + cy * yf->linesize[0] + cx,
							yf->data[1] + (cy / 2) * yf->linesize[1] + (cx / 2),
							yf->data[2] + (cy / 2) * yf->linesize[2] + (cx / 2),
							nullptr};
						av_image_copy(s.cropFrame->data, s.cropFrame->linesize, src,
							      yf->linesize, encodePixFmt(opts), cw, ch);
						toEnc = s.cropFrame;
					} else {
						ok = false;
					}
				}
				if (ok) {
					toEnc->pts = scaledPts(pts);
					encodeVideo(toEnc);
				}
			}
			av_frame_unref(frame);
		}
		encodeVideo(nullptr); // flush encoder
		av_write_trailer(s.ofmt);
	}

	av_packet_free(&pkt);
	av_frame_free(&frame);
	av_packet_free(&outPkt);

	if (s.ofmt && s.ofmt->pb && !(s.ofmt->oformat->flags & AVFMT_NOFILE))
		avio_closep(&s.ofmt->pb);

	if (cancel_.load())
		return QString();
	return errored ? QStringLiteral("Encoding failed.") : QString();
}

QString ClipExporter::runVideoCuts(const QString &inPath, const QString &outPath, const Options &opts)
{
	VideoState s;
	const QByteArray in = inPath.toUtf8();
	const QByteArray out = outPath.toUtf8();

	if (avformat_open_input(&s.ifmt, in.constData(), nullptr, nullptr) < 0)
		return QStringLiteral("Could not open the source file.");
	if (avformat_find_stream_info(s.ifmt, nullptr) < 0)
		return QStringLiteral("Could not read the source file.");

	const int vIdx = av_find_best_stream(s.ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vIdx < 0)
		return QStringLiteral("The file has no video track.");
	AVStream *vin = s.ifmt->streams[vIdx];

	// ---- Normalize the cut list ----
	const qint64 fileDurMs = durationMsOf(s.ifmt);
	std::vector<Cut> cuts;
	cuts.reserve(opts.cuts.size());
	for (Cut c : opts.cuts) {
		if (fileDurMs > 0) {
			c.startMs = std::clamp<qint64>(c.startMs, 0, fileDurMs);
			c.endMs = (c.endMs > 0) ? std::min(c.endMs, fileDurMs) : fileDurMs;
		}
		c.speed = std::clamp(c.speed, 0.05, 50.0);
		if (c.endMs - c.startMs >= 10)
			cuts.push_back(c);
	}
	if (cuts.empty())
		return QStringLiteral("There are no cuts to export.");

	// ---- Video decoder ----
	const AVCodec *vdc = avcodec_find_decoder(vin->codecpar->codec_id);
	if (!vdc)
		return QStringLiteral("Unsupported source video codec.");
	s.vdec = avcodec_alloc_context3(vdc);
	if (!s.vdec || avcodec_parameters_to_context(s.vdec, vin->codecpar) < 0)
		return QStringLiteral("Could not set up the video decoder.");
	s.vdec->pkt_timebase = vin->time_base;
	if (avcodec_open2(s.vdec, vdc, nullptr) < 0)
		return QStringLiteral("Could not open the video decoder.");

	// ---- Resolve crop (default = full frame), even-aligned & clamped ----
	int cx = 0, cy = 0, cw = s.vdec->width, ch = s.vdec->height;
	if (opts.crop && opts.cropW > 0 && opts.cropH > 0) {
		cx = std::clamp(opts.cropX, 0, s.vdec->width - 2);
		cy = std::clamp(opts.cropY, 0, s.vdec->height - 2);
		cw = std::min(opts.cropW, s.vdec->width - cx);
		ch = std::min(opts.cropH, s.vdec->height - cy);
	}
	cx = evenDown(cx);
	cy = evenDown(cy);
	cw = std::max(2, evenDown(cw));
	ch = std::max(2, evenDown(ch));
	// No crop selected (the default) -> decoded frames are fed straight to
	// the encoder; the per-frame full-image copy is skipped entirely.
	const bool cropNeeded = (cx != 0 || cy != 0 || cw != s.vdec->width || ch != s.vdec->height);

	// ---- Video encoder ----
	// Same clock as the source: the cuts are re-timed by pts, not by the base.
	const AVRational fr = av_guess_frame_rate(s.ifmt, vin, nullptr);
	if (const QString e = openVideoEncoder(
		    s, opts, out, cw, ch, vin->time_base, fr,
		    (fr.num > 0 && fr.den > 0) ? std::max(1, int(av_q2d(fr) * 2.0)) : 60);
	    !e.isEmpty())
		return e;

	// ---- Audio: decode → atempo per cut → one continuous AAC track ----
	AudioRetimer retimer;
	int aIdx = (opts.keepAudio && opts.format != Format::WebM)
			   ? av_find_best_stream(s.ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)
			   : -1;
	AVStream *ain = (aIdx >= 0) ? s.ifmt->streams[aIdx] : nullptr;
	if (ain) {
		QString aerr;
		if (retimer.init(ain->codecpar, ain->time_base.num, ain->time_base.den,
				 (s.ofmt->oformat->flags & AVFMT_GLOBALHEADER) != 0, &aerr)) {
			s.aOut = avformat_new_stream(s.ofmt, nullptr);
			if (!s.aOut ||
			    avcodec_parameters_from_context(s.aOut->codecpar, retimer.encoder()) < 0)
				return QStringLiteral("Could not create the output audio stream.");
			s.aOut->time_base = AVRational{1, retimer.sampleRate()};
		} else {
			ain = nullptr; // no usable audio — export video-only
			aIdx = -1;
		}
	}

	// ---- Scratch frames ----
	s.cropFrame = av_frame_alloc();
	s.cropFrame->format = encodePixFmt(opts);
	s.cropFrame->width = cw;
	s.cropFrame->height = ch;
	if (av_frame_get_buffer(s.cropFrame, 0) < 0)
		return QStringLiteral("Out of memory.");

	// ---- Open + write header (+faststart for MP4) ----
	if (const QString e = openOutputFile(s, opts, out); !e.isEmpty())
		return e;

	// ---- Shared encode helpers ----
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	AVPacket *outPkt = av_packet_alloc();
	bool errored = false;
	QString audioErr;

	auto ensureYuvFull = [&](AVFrame *f) { return s.ensureEncodeFormat(f, encodePixFmt(opts)); };

	VideoSink encodeVideo(s, outPkt);
	if (QString e = encodeVideo.initShader(opts); !e.isEmpty())
		return e;

	auto writeAudio = [&](AVPacket *p) -> bool {
		av_packet_rescale_ts(p, AVRational{1, retimer.sampleRate()}, s.aOut->time_base);
		p->stream_index = s.aOut->index;
		p->pos = -1;
		return av_interleaved_write_frame(s.ofmt, p) >= 0;
	};

	// Output-video pts: each cut's frames land on an accumulating base, with the
	// in-cut offset compressed by that cut's speed. Kept strictly increasing.
	int64_t lastEncPts = -1;
	double outBaseTicks = 0.0; // in vin->time_base units
	double doneOutMs = 0.0;    // completed cuts' output duration (progress)
	double totalOutMs = 0.0;
	for (const Cut &c : cuts)
		totalOutMs += double(c.endMs - c.startMs) / c.speed;

	const auto startWall = std::chrono::steady_clock::now();
	auto lastEmit = startWall;
	double processedMs = 0.0;

	for (size_t ci = 0; ci < cuts.size() && !errored; ++ci) {
		if (cancel_.load())
			break;
		const Cut &cut = cuts[ci];
		const int64_t startV = av_rescale_q(cut.startMs, {1, 1000}, vin->time_base);
		const int64_t endV = av_rescale_q(cut.endMs, {1, 1000}, vin->time_base);
		const int64_t endA = ain ? av_rescale_q(cut.endMs, {1, 1000}, ain->time_base) : 0;

		av_seek_frame(s.ifmt, vIdx, startV, AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(s.vdec);
		if (ain && !retimer.beginSegment(cut.startMs, cut.endMs, cut.speed, &audioErr)) {
			errored = true;
			break;
		}

		bool videoDone = false;
		bool audioDone = (ain == nullptr);

		auto handleDecodedFrame = [&](AVFrame *df) -> bool {
			const int64_t pts = df->best_effort_timestamp != AV_NOPTS_VALUE
						    ? df->best_effort_timestamp
						    : 0;
			if (pts < startV)
				return true;
			if (pts > endV) {
				videoDone = true;
				return true;
			}
			// Crop only when set — otherwise feed the decoded frame straight.
			AVFrame *yf = ensureYuvFull(df);
			AVFrame *toEnc = yf;
			if (cropNeeded) {
				if (av_frame_make_writable(s.cropFrame) < 0)
					return false;
				const uint8_t *src[4] = {yf->data[0] + cy * yf->linesize[0] + cx,
							 yf->data[1] + (cy / 2) * yf->linesize[1] + (cx / 2),
							 yf->data[2] + (cy / 2) * yf->linesize[2] + (cx / 2),
							 nullptr};
				av_image_copy(s.cropFrame->data, s.cropFrame->linesize, src, yf->linesize,
					      encodePixFmt(opts), cw, ch);
				toEnc = s.cropFrame;
			}
			int64_t v = (int64_t)llround(outBaseTicks + double(pts - startV) / cut.speed);
			if (v <= lastEncPts)
				v = lastEncPts + 1;
			lastEncPts = v;
			toEnc->pts = v;
			processedMs = std::max(processedMs,
					       doneOutMs + (pts - startV) * av_q2d(vin->time_base) *
								   1000.0 / cut.speed);
			return encodeVideo(toEnc);
		};

		while (!errored && !(videoDone && audioDone)) {
			if (cancel_.load())
				break;
			if (av_read_frame(s.ifmt, pkt) < 0)
				break; // end of file

			if (pkt->stream_index == vIdx && !videoDone) {
				if (avcodec_send_packet(s.vdec, pkt) >= 0) {
					while (avcodec_receive_frame(s.vdec, frame) >= 0) {
						if (!handleDecodedFrame(frame)) {
							errored = true;
							av_frame_unref(frame);
							break;
						}
						av_frame_unref(frame);
						if (videoDone)
							break;
					}
				}
			} else if (aIdx >= 0 && pkt->stream_index == aIdx && !audioDone) {
				if (pkt->pts != AV_NOPTS_VALUE && pkt->pts > endA) {
					audioDone = true;
				} else if (!retimer.push(pkt, writeAudio, &audioErr)) {
					errored = true;
				}
			}
			av_packet_unref(pkt);

			const auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEmit)
				    .count() >= 200) {
				lastEmit = now;
				const double wall =
					std::chrono::duration_cast<std::chrono::milliseconds>(
						now - startWall)
						.count() /
					1000.0;
				const int pct = totalOutMs > 0
							? std::clamp(int(processedMs / totalOutMs * 100.0),
								     0, 99)
							: 0;
				const double rate = wall > 0.05 ? (processedMs / 1000.0) / wall : 0.0;
				const qint64 eta =
					(totalOutMs > 0 && rate > 0.01)
						? qint64((totalOutMs - processedMs) / 1000.0 / rate *
							 1000.0)
						: 0;
				emit progress(pct, eta, s.ofmt->pb ? avio_tell(s.ofmt->pb) : 0);
			}
		}

		if (errored || cancel_.load())
			break;

		// Drain the video decoder so the cut's last (reordered) frames are kept,
		// then reset it for the next cut's seek.
		avcodec_send_packet(s.vdec, nullptr);
		while (avcodec_receive_frame(s.vdec, frame) >= 0) {
			const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
						    ? frame->best_effort_timestamp
						    : 0;
			if (pts >= startV && pts <= endV && !videoDone) {
				if (!handleDecodedFrame(frame))
					errored = true;
			}
			av_frame_unref(frame);
		}
		avcodec_flush_buffers(s.vdec);

		if (ain && !retimer.endSegment(writeAudio, &audioErr))
			errored = true;

		outBaseTicks += double(endV - startV) / cut.speed;
		doneOutMs += double(cut.endMs - cut.startMs) / cut.speed;
	}

	if (!errored && !cancel_.load()) {
		if (ain && !retimer.finish(writeAudio, &audioErr))
			errored = true;
		if (!errored) {
			encodeVideo(nullptr); // flush the video encoder
			av_write_trailer(s.ofmt);
		}
	}

	av_packet_free(&pkt);
	av_frame_free(&frame);
	av_packet_free(&outPkt);

	if (s.ofmt && s.ofmt->pb && !(s.ofmt->oformat->flags & AVFMT_NOFILE))
		avio_closep(&s.ofmt->pb);

	if (cancel_.load())
		return QString();
	if (errored)
		return audioErr.isEmpty() ? QStringLiteral("Encoding failed.") : audioErr;
	return QString();
}

// ---------------------------------------------------------------------------
// Multi-source multi-cut: cuts drawn from several input files, each frame
// scaled and letterboxed onto the primary (inputs[0]) canvas. Audio from each
// cut's own source is retimed into one continuous AAC track (only when every
// used source has audio — otherwise the export is video-only).
// ---------------------------------------------------------------------------
QString ClipExporter::runVideoCutsMulti(const QString &outPath, const Options &opts)
{
	const QByteArray out = outPath.toUtf8();

	// One decode context per input file.
	struct MSrc {
		AVFormatContext *ifmt = nullptr;
		int vIdx = -1, aIdx = -1;
		AVStream *vin = nullptr, *ain = nullptr;
		AVCodecContext *vdec = nullptr;
		SwsContext *sws = nullptr; // decoded -> fitW×fitH YUV420P (into the canvas)
		int fitW = 0, fitH = 0, padX = 0, padY = 0;
		~MSrc()
		{
			if (sws)
				sws_freeContext(sws);
			if (vdec)
				avcodec_free_context(&vdec);
			if (ifmt)
				avformat_close_input(&ifmt);
		}
	};
	std::vector<MSrc> src(opts.inputs.size());

	for (size_t i = 0; i < opts.inputs.size(); ++i) {
		MSrc &m = src[i];
		if (avformat_open_input(&m.ifmt, opts.inputs[i].c_str(), nullptr, nullptr) < 0 ||
		    avformat_find_stream_info(m.ifmt, nullptr) < 0)
			return QStringLiteral("Could not open a source file.");
		m.vIdx = av_find_best_stream(m.ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (m.vIdx < 0)
			return QStringLiteral("A source has no video track.");
		m.vin = m.ifmt->streams[m.vIdx];
		const AVCodec *vdc = avcodec_find_decoder(m.vin->codecpar->codec_id);
		if (!vdc)
			return QStringLiteral("Unsupported source video codec.");
		m.vdec = avcodec_alloc_context3(vdc);
		if (!m.vdec || avcodec_parameters_to_context(m.vdec, m.vin->codecpar) < 0)
			return QStringLiteral("Could not set up a video decoder.");
		m.vdec->pkt_timebase = m.vin->time_base;
		if (avcodec_open2(m.vdec, vdc, nullptr) < 0)
			return QStringLiteral("Could not open a video decoder.");
		m.aIdx = av_find_best_stream(m.ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		m.ain = (m.aIdx >= 0) ? m.ifmt->streams[m.aIdx] : nullptr;
	}

	// Canvas = primary (inputs[0]) full resolution + framerate. Crop is not
	// applied when mixing sources.
	const int cw = std::max(2, evenDown(src[0].vdec->width));
	const int ch = std::max(2, evenDown(src[0].vdec->height));
	const AVRational fr = av_guess_frame_rate(src[0].ifmt, src[0].vin, nullptr);

	// Fit rectangle for each source (preserve aspect, centered, even-aligned).
	for (MSrc &m : src) {
		const double sc = std::min(double(cw) / m.vdec->width, double(ch) / m.vdec->height);
		m.fitW = std::max(2, evenDown(int(m.vdec->width * sc)));
		m.fitH = std::max(2, evenDown(int(m.vdec->height * sc)));
		m.padX = evenDown((cw - m.fitW) / 2);
		m.padY = evenDown((ch - m.fitH) / 2);
		m.sws = sws_getContext(m.vdec->width, m.vdec->height, m.vdec->pix_fmt, m.fitW, m.fitH,
				       encodePixFmt(opts), SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!m.sws)
			return QStringLiteral("Could not create a video scaler.");
	}

	// Normalize cuts (clamp to each source's duration).
	std::vector<Cut> cuts;
	cuts.reserve(opts.cuts.size());
	for (Cut c : opts.cuts) {
		if (c.source < 0 || c.source >= int(src.size()))
			continue;
		const qint64 dur = durationMsOf(src[c.source].ifmt);
		if (dur > 0) {
			c.startMs = std::clamp<qint64>(c.startMs, 0, dur);
			c.endMs = (c.endMs > 0) ? std::min(c.endMs, dur) : dur;
		}
		c.speed = std::clamp(c.speed, 0.05, 50.0);
		if (c.endMs - c.startMs >= 10)
			cuts.push_back(c);
	}
	if (cuts.empty())
		return QStringLiteral("There are no cuts to export.");

	// Output container + H.264/VP9 encoder at the canvas size (VideoState owns
	// the output side; its input fields stay null).
	VideoState s;
	// A common 90 kHz tick rather than any one source's: the cuts can come from
	// files with different time bases.
	if (const QString e = openVideoEncoder(
		    s, opts, out, cw, ch, AVRational{1, 90000}, fr,
		    (fr.num > 0 && fr.den > 0) ? std::max(1, int(av_q2d(fr) * 2.0)) : 60);
	    !e.isEmpty())
		return e;

	// Audio: one retimer switched between sources — only if EVERY used source
	// has an audio stream (otherwise a gap would desync; export video-only).
	AudioRetimer retimer;
	bool wantAudio = (opts.keepAudio && opts.format != Format::WebM);
	if (wantAudio)
		for (const Cut &c : cuts)
			if (!src[c.source].ain) {
				wantAudio = false;
				break;
			}
	int curAudioSrc = -1;
	if (wantAudio) {
		const int first = cuts[0].source;
		MSrc &fm = src[first];
		QString aerr;
		if (retimer.init(fm.ain->codecpar, fm.ain->time_base.num, fm.ain->time_base.den,
				 (s.ofmt->oformat->flags & AVFMT_GLOBALHEADER) != 0, &aerr)) {
			curAudioSrc = first;
			s.aOut = avformat_new_stream(s.ofmt, nullptr);
			if (!s.aOut ||
			    avcodec_parameters_from_context(s.aOut->codecpar, retimer.encoder()) < 0)
				return QStringLiteral("Could not create the output audio stream.");
			s.aOut->time_base = AVRational{1, retimer.sampleRate()};
		} else {
			wantAudio = false; // fall back to video-only
		}
	}

	// Canvas frame (reuse cropFrame as the letterboxed output frame).
	s.cropFrame = av_frame_alloc();
	s.cropFrame->format = encodePixFmt(opts);
	s.cropFrame->width = cw;
	s.cropFrame->height = ch;
	if (av_frame_get_buffer(s.cropFrame, 0) < 0)
		return QStringLiteral("Out of memory.");

	if (const QString e = openOutputFile(s, opts, out); !e.isEmpty())
		return e;

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	AVPacket *outPkt = av_packet_alloc();
	bool errored = false;
	QString audioErr;

	auto fillBlack = [&](AVFrame *f) {
		std::memset(f->data[0], 16, size_t(f->linesize[0]) * f->height);
		std::memset(f->data[1], 128, size_t(f->linesize[1]) * (f->height / 2));
		std::memset(f->data[2], 128, size_t(f->linesize[2]) * (f->height / 2));
	};
	VideoSink encodeVideo(s, outPkt);
	if (QString e = encodeVideo.initShader(opts); !e.isEmpty())
		return e;
	auto writeAudio = [&](AVPacket *p) -> bool {
		av_packet_rescale_ts(p, AVRational{1, retimer.sampleRate()}, s.aOut->time_base);
		p->stream_index = s.aOut->index;
		p->pos = -1;
		return av_interleaved_write_frame(s.ofmt, p) >= 0;
	};

	int64_t lastEncPts = -1;
	double doneOutMs = 0.0, totalOutMs = 0.0, processedMs = 0.0;
	for (const Cut &c : cuts)
		totalOutMs += double(c.endMs - c.startMs) / c.speed;

	const auto startWall = std::chrono::steady_clock::now();
	auto lastEmit = startWall;

	for (size_t ci = 0; ci < cuts.size() && !errored; ++ci) {
		if (cancel_.load())
			break;
		const Cut &cut = cuts[ci];
		MSrc &m = src[cut.source];
		const int64_t startV = av_rescale_q(cut.startMs, {1, 1000}, m.vin->time_base);
		const int64_t endV = av_rescale_q(cut.endMs, {1, 1000}, m.vin->time_base);
		const int64_t endA = m.ain ? av_rescale_q(cut.endMs, {1, 1000}, m.ain->time_base) : 0;

		av_seek_frame(m.ifmt, m.vIdx, startV, AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(m.vdec);
		if (wantAudio) {
			if (cut.source != curAudioSrc) {
				if (!retimer.setInput(m.ain->codecpar, m.ain->time_base.num,
						      m.ain->time_base.den, &audioErr)) {
					errored = true;
					break;
				}
				curAudioSrc = cut.source;
			}
			if (!retimer.beginSegment(cut.startMs, cut.endMs, cut.speed, &audioErr)) {
				errored = true;
				break;
			}
		}

		bool videoDone = false;
		bool audioDone = !wantAudio;

		auto handleDecodedFrame = [&](AVFrame *df) -> bool {
			const int64_t pts =
				df->best_effort_timestamp != AV_NOPTS_VALUE ? df->best_effort_timestamp : 0;
			if (pts < startV)
				return true;
			if (pts > endV) {
				videoDone = true;
				return true;
			}
			if (av_frame_make_writable(s.cropFrame) < 0)
				return false;
			fillBlack(s.cropFrame); // letterbox background
			uint8_t *dst[4] = {
				s.cropFrame->data[0] + m.padY * s.cropFrame->linesize[0] + m.padX,
				s.cropFrame->data[1] + (m.padY / 2) * s.cropFrame->linesize[1] + (m.padX / 2),
				s.cropFrame->data[2] + (m.padY / 2) * s.cropFrame->linesize[2] + (m.padX / 2),
				nullptr};
			sws_scale(m.sws, df->data, df->linesize, 0, m.vdec->height, dst,
				  s.cropFrame->linesize);
			const double srcMs = double(pts) * av_q2d(m.vin->time_base) * 1000.0;
			const double outMs = doneOutMs + (srcMs - cut.startMs) / cut.speed;
			int64_t v = (int64_t)llround(outMs * 90.0); // 90000 ticks/s ÷ 1000
			if (v <= lastEncPts)
				v = lastEncPts + 1;
			lastEncPts = v;
			s.cropFrame->pts = v;
			processedMs = std::max(processedMs, outMs);
			return encodeVideo(s.cropFrame);
		};

		while (!errored && !(videoDone && audioDone)) {
			if (cancel_.load())
				break;
			if (av_read_frame(m.ifmt, pkt) < 0)
				break;
			if (pkt->stream_index == m.vIdx && !videoDone) {
				if (avcodec_send_packet(m.vdec, pkt) >= 0) {
					while (avcodec_receive_frame(m.vdec, frame) >= 0) {
						if (!handleDecodedFrame(frame)) {
							errored = true;
							av_frame_unref(frame);
							break;
						}
						av_frame_unref(frame);
						if (videoDone)
							break;
					}
				}
			} else if (wantAudio && pkt->stream_index == m.aIdx && !audioDone) {
				if (pkt->pts != AV_NOPTS_VALUE && pkt->pts > endA)
					audioDone = true;
				else if (!retimer.push(pkt, writeAudio, &audioErr))
					errored = true;
			}
			av_packet_unref(pkt);

			const auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEmit).count() >=
			    200) {
				lastEmit = now;
				const double wall =
					std::chrono::duration_cast<std::chrono::milliseconds>(now - startWall)
						.count() /
					1000.0;
				const int pct =
					totalOutMs > 0
						? std::clamp(int(processedMs / totalOutMs * 100.0), 0, 99)
						: 0;
				const double rate = wall > 0.05 ? (processedMs / 1000.0) / wall : 0.0;
				const qint64 eta =
					(totalOutMs > 0 && rate > 0.01)
						? qint64((totalOutMs - processedMs) / 1000.0 / rate * 1000.0)
						: 0;
				emit progress(pct, eta, s.ofmt->pb ? avio_tell(s.ofmt->pb) : 0);
			}
		}

		if (errored || cancel_.load())
			break;

		// Drain the video decoder for this cut's reordered tail.
		avcodec_send_packet(m.vdec, nullptr);
		while (avcodec_receive_frame(m.vdec, frame) >= 0) {
			const int64_t pts =
				frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : 0;
			if (pts >= startV && pts <= endV && !videoDone) {
				if (!handleDecodedFrame(frame))
					errored = true;
			}
			av_frame_unref(frame);
		}
		avcodec_flush_buffers(m.vdec);

		if (wantAudio && !retimer.endSegment(writeAudio, &audioErr))
			errored = true;

		doneOutMs += double(cut.endMs - cut.startMs) / cut.speed;
	}

	if (!errored && !cancel_.load()) {
		if (wantAudio && !retimer.finish(writeAudio, &audioErr))
			errored = true;
		if (!errored) {
			encodeVideo(nullptr);
			av_write_trailer(s.ofmt);
		}
	}

	av_packet_free(&pkt);
	av_frame_free(&frame);
	av_packet_free(&outPkt);

	if (s.ofmt && s.ofmt->pb && !(s.ofmt->oformat->flags & AVFMT_NOFILE))
		avio_closep(&s.ofmt->pb);

	if (cancel_.load())
		return QString();
	if (errored)
		return audioErr.isEmpty() ? QStringLiteral("Encoding failed.") : audioErr;
	return QString();
}

// ===================== Full editing: timeline render =====================

QString ClipExporter::runTimeline(const QString &outPath, const Options &opts)
{
	const TimelineModel &tl = opts.timeline;
	const qint64 totalMs = tl.durationMs();
	if (totalMs <= 0)
		return QStringLiteral("The timeline is empty — add a clip before exporting.");

	const int cw = evenDown(std::max(2, opts.canvasW));
	const int ch = evenDown(std::max(2, opts.canvasH));
	const double fps = std::clamp(opts.timelineFps > 0.1 ? opts.timelineFps : 30.0, 1.0, 240.0);
	const QSize canvas(cw, ch);

	// One decoder per source PER TRACK the timeline references. FrameSeeker's
	// frameAt rolls forward without seeking for near-future requests, which is
	// exactly the access pattern of a linear render -- but only if each decoder
	// gets a monotonic run of requests. Share one across a source that is
	// stacked on two tracks and it alternates between two distant positions,
	// paying two full seeks on every single frame of the export.
	struct Provider : TimelineCompositor::FrameProvider {
		std::map<std::pair<int, int>, std::unique_ptr<FrameSeeker>> seekers;
		std::map<int, QImage> stills; // image clips: same picture at every time
		int w = 0, h = 0;
		QImage frameFor(int sourceId, qint64 srcMs) override
		{
			if (const auto sit = stills.find(sourceId); sit != stills.end())
				return sit->second;
			auto it = seekers.find({sourceId, std::max(0, track())});
			if (it == seekers.end() || !it->second)
				return QImage();
			return it->second->frameAt(srcMs, w, h);
		}
	} provider;
	provider.w = cw;
	provider.h = ch;
	for (int ti = 0; ti < tl.tracks.size(); ++ti) {
		const TlTrack &t = tl.tracks[ti];
		if (t.kind != TlTrack::Kind::Video)
			continue;
		for (const TlClip &c : t.clips) {
			const auto sit = opts.timelineSources.find(c.sourceId);
			if (sit == opts.timelineSources.end())
				continue;
			const QString path = QString::fromStdString(sit->second);
			if (c.type == TlClip::Type::Image) {
				if (provider.stills.count(c.sourceId))
					continue;
				// readStillImage, not QImage(path): a WebP added through
				// the libav fallback previews fine and would otherwise
				// encode as an empty frame, which nothing reports.
				QImage img = readStillImage(path);
				if (!img.isNull())
					provider.stills[c.sourceId] =
						img.convertToFormat(QImage::Format_RGBA8888);
				continue;
			}
			if (c.type != TlClip::Type::Video ||
			    provider.seekers.count({c.sourceId, ti}))
				continue;
			auto fs = std::make_unique<FrameSeeker>();
			if (fs->open(path))
				provider.seekers[{c.sourceId, ti}] = std::move(fs);
		}
	}

	// Transform scripts run on THIS thread (a script runtime cannot be shared), from
	// the sources carried on Options rather than off disk.
	TransformEvaluator scriptEval;
	for (auto it = opts.timelineScripts.constBegin(); it != opts.timelineScripts.constEnd(); ++it) {
		QString serr;
		if (!scriptEval.compile(it.key(), it.value(), &serr))
			return QStringLiteral("The transform script \"%1\" failed to compile:\n%2")
				.arg(it.key(), serr);
	}

	// Optional post-processing shader chain, applied to the composited RGBA frame
	// before it is converted for the encoder (same chain as the preview).
	ShaderRenderer shader;
	QVector<QMap<QString, double>> shaderParams;
	bool shaderOn = false;
	if (!opts.effects.empty()) {
		if (!shader.ensureGl())
			return QStringLiteral("The effect needs OpenGL 3.3, which isn't available here: %1")
				.arg(shader.lastError());
		QVector<ShaderLayerSource> layers;
		for (const auto &e : opts.effects) {
			layers.append({e.source, e.paramDefs});
			shaderParams.append(e.params);
		}
		QString serr;
		if (!shader.setChain(layers, &serr))
			return QStringLiteral("The effect shader failed to compile:\n%1").arg(serr);
		shaderOn = true;
	}

	// ---- Output container + encoder (mirrors the multi-source path) -------
	VideoState s;
	const QByteArray out = outPath.toUtf8();
	// The timeline sets its own frame rate, so the GOP comes from that rather
	// than from any source file.
	if (const QString e = openVideoEncoder(s, opts, out, cw, ch, AVRational{1, 90000},
					       av_d2q(fps, 1000000), std::max(1, int(fps * 2.0)));
	    !e.isEmpty())
		return e;
	// This path used to write its header by hand, and was the only one that did
	// not ask for +faststart. Going through the shared helper fixes that.
	if (const QString e = openOutputFile(s, opts, out); !e.isEmpty())
		return e;

	// RGBA (composited) -> YUV420P (encoder).
	// Straight to the encoder's format. Going via 4:2:0 first and letting
	// scaleForEncode widen it back to 4:4:4 would be lossless-looking and
	// pointless: the chroma is thrown away in the first conversion, and this is
	// the path where composited text and graphics live.
	SwsContext *toYuv = sws_getContext(cw, ch, AV_PIX_FMT_RGBA, cw, ch, encodePixFmt(opts),
					   SWS_BILINEAR, nullptr, nullptr, nullptr);
	AVFrame *yuv = av_frame_alloc();
	AVPacket *pkt = av_packet_alloc();
	if (!toYuv || !yuv || !pkt) {
		if (toYuv)
			sws_freeContext(toYuv);
		if (yuv)
			av_frame_free(&yuv);
		if (pkt)
			av_packet_free(&pkt);
		return QStringLiteral("Could not allocate the render buffers.");
	}
	yuv->format = encodePixFmt(opts);
	yuv->width = cw;
	yuv->height = ch;
	av_frame_get_buffer(yuv, 0);

	auto cleanup = [&]() {
		sws_freeContext(toYuv);
		av_frame_free(&yuv);
		av_packet_free(&pkt);
	};
	// No initShader(): the timeline composites effects as components, so there
	// is nothing to bake here and the pass stays inactive.
	VideoSink encode(s, pkt);

	// ---- Render every frame -----------------------------------------------
	const auto t0 = std::chrono::steady_clock::now();
	const int64_t frames = std::max<int64_t>(1, int64_t(std::llround(totalMs / 1000.0 * fps)));
	for (int64_t i = 0; i < frames; ++i) {
		if (cancel_.load())
			break;
		const qint64 tMs = qint64(std::llround(double(i) * 1000.0 / fps));

		QImage composed =
			TimelineCompositor::compose(tl, tMs, canvas, provider, &scriptEval, fps);
		if (composed.isNull()) {
			cleanup();
			return QStringLiteral("Could not compose the timeline frame.");
		}
		if (shaderOn)
			composed = shader.apply(composed, float(tMs) / 1000.0f, int(i), shaderParams);
		if (composed.format() != QImage::Format_RGBA8888)
			composed = composed.convertToFormat(QImage::Format_RGBA8888);

		if (av_frame_make_writable(yuv) < 0) {
			cleanup();
			return QStringLiteral("Could not prepare the encoder frame.");
		}
		const uint8_t *src[4] = {composed.constBits(), nullptr, nullptr, nullptr};
		int srcStride[4] = {int(composed.bytesPerLine()), 0, 0, 0};
		sws_scale(toYuv, src, srcStride, 0, ch, yuv->data, yuv->linesize);
		yuv->pts = av_rescale_q(tMs, AVRational{1, 1000}, s.venc->time_base);
		if (!encode(yuv)) {
			cleanup();
			return QStringLiteral("Encoding failed.");
		}

		if ((i % 8) == 0) {
			const double done = double(i + 1) / double(frames);
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
						     std::chrono::steady_clock::now() - t0)
						     .count();
			const qint64 eta = done > 0.01 ? qint64(elapsed / done - elapsed) : 0;
			const qint64 bytes = s.ofmt->pb ? avio_tell(s.ofmt->pb) : 0;
			emit progress(std::min(95, int(done * 95.0)), eta, bytes);
		}
	}

	encode(nullptr); // flush
	av_write_trailer(s.ofmt);
	cleanup();
	if (cancel_.load())
		return QString();
	return QString();
}

QString ClipExporter::mixTimelineAudio(const QString &videoPath, const Options &opts)
{
	// Placement, speed, gain and fades come from TimelineAudio so the export
	// matches what the preview plays — the same reason the picture goes through
	// TimelineCompositor twice rather than being described twice.
	QTemporaryDir tmp;
	if (!tmp.isValid())
		return QStringLiteral("Could not create a temporary folder for the audio mix.");

	const std::vector<VoiceoverMixer::Take> takes =
		TimelineAudio::buildTakes(opts.timeline, [&opts](int id) {
			const auto it = opts.timelineSources.find(id);
			return it == opts.timelineSources.end() ? QString()
								: QString::fromStdString(it->second);
		}, tmp.path());

	if (takes.empty())
		return QString(); // a silent timeline is fine — leave the video as-is
	// originalVolume 0: the rendered video has no audio of its own to keep.
	return VoiceoverMixer::mix(videoPath, 0.0, false, takes, &cancel_);
}

} // namespace harpia
