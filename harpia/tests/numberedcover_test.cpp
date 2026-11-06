// Covers are extra JPEG metadata, never a title card. Verify actual MP4/MOV
// streams, decoded frames and sound, including the opt-in frame-zero fallback.
#include "editor/BatchExport.hpp"
#include "editor/CoverArt.hpp"
#include "editor/ExportOptionsDialog.hpp"
#include "editor/NumberedCover.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineJson.hpp"
#include "editor/SwsColor.hpp"
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"

#include <QtWidgets>
#include <QTest>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

using namespace harpia;
namespace {
int failures = 0;
void check(bool ok, const char *why)
{
	std::printf("%s %s\n", ok ? "PASS" : "FAIL", why);
	failures += !ok;
}
QByteArray command(const QStringList &args)
{
	QProcess process;
	process.start(QStringLiteral("ffmpeg"), args);
	if (!process.waitForFinished(30000) || process.exitCode() != 0) {
		std::printf("ffmpeg failed: %s\n", process.readAllStandardError().constData());
		++failures;
		return {};
	}
	return process.readAllStandardOutput();
}
QStringList frames(const QString &file)
{
	const auto raw = command({"-v", "error", "-i", file, "-map", "0:v:0", "-an", "-f", "framemd5", "-"});
	QStringList hashes;
	for (const auto &line : raw.split('\n')) {
		if (!line.isEmpty() && !line.startsWith('#')) {
			hashes.append(QString::fromLatin1(line.split(',').last().trimmed()));
		}
	}
	return hashes;
}
QByteArray audio(const QString &file)
{
	return command({"-v", "error", "-i", file, "-map", "0:a:0", "-vn", "-f", "s16le", "-"});
}
bool exportFile(const QString &input, const QString &path, const ClipExporter::Options &options)
{
	ClipExporter exporter;
	bool ok = false;
	QObject::connect(&exporter, &ClipExporter::finished, [&](bool done, bool cancelled, const QString &why) {
		ok = done && !cancelled;
		if (!ok) {
			std::printf("Export failed: %s\n", qPrintable(why));
		}
	});
	exporter.run(input, path, options);
	return ok;
}
QByteArray coverData(const QString &file)
{
	AVFormatContext *format = nullptr;
	QByteArray bytes;
	if (avformat_open_input(&format, file.toUtf8().constData(), nullptr, nullptr) < 0) {
		return bytes;
	}
	avformat_find_stream_info(format, nullptr);
	const int best = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	check(best >= 0 && format->streams[best]->codecpar->codec_id == AV_CODEC_ID_H264,
	      "the cover is an attached picture and H.264 remains the main video");
	for (unsigned i = 0; i < format->nb_streams; ++i) {
		const auto *stream = format->streams[i];
		if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
			bytes = QByteArray(reinterpret_cast<const char *>(stream->attached_pic.data),
					   stream->attached_pic.size);
		}
	}
	avformat_close_input(&format);
	return bytes;
}
QImage decodeJpeg(const QByteArray &bytes)
{
	QImage image;
	AVCodecContext *decoder = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_MJPEG));
	AVFrame *frame = av_frame_alloc();
	AVPacket *packet = av_packet_alloc();
	if (decoder && frame && packet && avcodec_open2(decoder, decoder->codec, nullptr) >= 0 &&
	    av_new_packet(packet, int(bytes.size())) >= 0) {
		std::memcpy(packet->data, bytes.constData(), size_t(bytes.size()));
		if (avcodec_send_packet(decoder, packet) >= 0 && avcodec_receive_frame(decoder, frame) >= 0) {
			image = QImage(frame->width, frame->height, QImage::Format_RGB888);
			SwsContext *sws = sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format),
							 frame->width, frame->height, AV_PIX_FMT_RGB24, SWS_BILINEAR,
							 nullptr, nullptr, nullptr);
			if (sws) {
				swsDecodeColors(sws, frame);
				uint8_t *pixels[] = {image.bits(), nullptr, nullptr, nullptr};
				int strides[] = {int(image.bytesPerLine()), 0, 0, 0};
				sws_scale(sws, frame->data, frame->linesize, 0, frame->height, pixels, strides);
				sws_freeContext(sws);
			}
		}
	}
	av_packet_free(&packet);
	av_frame_free(&frame);
	avcodec_free_context(&decoder);
	return image;
}
QRect whiteText(const QImage &image)
{
	QRect bounds;
	for (int y = 0; y < image.height(); ++y) {
		for (int x = 0; x < image.width(); ++x) {
			const auto colour = image.pixelColor(x, y);
			if (colour.red() > 225 && colour.green() > 225 && colour.blue() > 225) {
				bounds |= QRect(x, y, 1, 1);
			}
		}
	}
	return bounds;
}

double labelDifference(const QImage &image, const QString &label)
{
	if (image.isNull()) {
		return 1e12;
	}
	QImage expected(image.size(), QImage::Format_RGB32);
	expected.fill(image.pixelColor(5, 5));
	cover::drawLabel(expected, label);
	double difference = 0;
	for (int y = image.height() / 3; y < image.height() * 2 / 3; ++y) {
		for (int x = 0; x < image.width(); ++x) {
			const auto a = image.pixelColor(x, y), b = expected.pixelColor(x, y);
			difference += std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) +
				      std::abs(a.blue() - b.blue());
		}
	}
	return difference;
}

void checkEditorExports(const QString &directory, const QString &red, const QString &blue)
{
	QSettings().clear();
	QSettings().setValue(QStringLiteral("export/format"), int(ClipExporter::Format::Mp4));
	TlClip a, b;
	a.srcEndMs = 1000;
	a.clipOptions = {{0, 0}, {0, 0}, {0, 0}}; // identical footage: only the cover number distinguishes variants
	b.srcEndMs = 1000;
	b.sourceId = 1;
	b.outStartMs = 1000;
	TlTrack track;
	track.clips = {a, b};
	TimelineModel model;
	model.tracks = {track};
	QJsonObject root{{"harpiaProject", 3}, {"editMode", "full"}, {"canvasW", 320}, {"canvasH", 180}, {"fps", 10}};
	root[QStringLiteral("sources")] =
		QJsonArray{QJsonObject{{"id", 0}, {"path", red}}, QJsonObject{{"id", 1}, {"path", blue}}};
	timelineToProject(model, root);
	const QString project = QDir(directory).filePath(QStringLiteral("numbered.harpia"));
	QFile file(project);
	file.open(QIODevice::WriteOnly);
	file.write(QJsonDocument(root).toJson());
	file.close();
	VideoEditorWindow window{QString()};
	check(window.openProjectAt(project), "real editor loads a project supporting Spark and Variations");
	window.show();
	for (bool spark : {false, true}) {
		bool seen = false;
		QElapsedTimer timeout;
		timeout.start();
		QTimer driver;
		QObject::connect(&driver, &QTimer::timeout, [&]() {
			auto *modal = QApplication::activeModalWidget();
			if (auto *dialog = qobject_cast<ExportOptionsDialog *>(modal); dialog && !seen) {
				seen = true;
				if (spark) {
					dialog->setSparkTabForTest(true);
				} else {
					dialog->setBatchTabForTest(true);
					dialog->batchListForTest()->item(1)->setCheckState(Qt::Unchecked);
				}
				const QString key = spark ? QStringLiteral("sparkCover")
							  : QStringLiteral("variationCover");
				dialog->findChild<QGroupBox *>(key + "Enabled")->setChecked(true);
				dialog->findChild<QLineEdit *>(key + "Prefix")
					->setText(spark ? QStringLiteral("S ") : QStringLiteral("V "));
				dialog->findChild<QLineEdit *>(key + "Suffix")->setText(QStringLiteral("!"));
				dialog->findChild<QSpinBox *>(key + "Start")->setValue(7);
				dialog->findChild<QCheckBox *>(key + "FirstFrame")->setChecked(false);
				dialog->findChild<QLineEdit *>(QStringLiteral("exportFolder"))->setText(directory);
				for (auto *combo : dialog->findChildren<QComboBox *>()) {
					if (combo->findText(QStringLiteral("MP4 (H.264)")) >= 0) {
						combo->setCurrentIndex(combo->findText(QStringLiteral("MP4 (H.264)")));
					}
				}
				for (auto *box : dialog->findChildren<QCheckBox *>()) {
					if (box->text().startsWith(QStringLiteral("Open the folder"))) {
						box->setChecked(false);
					}
				}
				dialog->accept();
			} else if (auto *message = qobject_cast<QMessageBox *>(modal)) {
				message->accept();
			} else if (timeout.elapsed() > 30000 && modal) {
				if (auto *dialog = qobject_cast<QDialog *>(modal)) {
					dialog->reject();
				}
			}
		});
		driver.start(10);
		QMetaObject::invokeMethod(&window, "onSave", Qt::DirectConnection);
		auto *batch = window.findChild<BatchExporter *>();
		const QString folder = batch ? batch->folder() : QString();
		check(seen && !folder.isEmpty() &&
			      QTest::qWaitFor([&]() { return !window.findChild<BatchExporter *>(); }, 30000),
		      spark ? "real Spark export completes with the optional cover controls"
			    : "real Variations export completes with the optional cover controls");
		const auto files = QDir(folder).entryList({QStringLiteral("*.mp4")}, QDir::Files, QDir::Name);
		check(files.size() == 2, "the editor writes exactly two numbered versions");
		for (int i = 0; i < files.size(); ++i) {
			const QImage cover = decodeJpeg(coverData(QDir(folder).filePath(files[i])));
			const QString prefix = spark ? QStringLiteral("S ") : QStringLiteral("V ");
			const int number = spark ? i + 7 : (i == 0 ? 7 : 9);
			const int otherNumber = i == 0 ? (spark ? 8 : 9) : 7;
			const double correct =
				labelDifference(cover, prefix + QString::number(number) + QLatin1Char('!'));
			const double other =
				labelDifference(cover, prefix + QString::number(otherNumber) + QLatin1Char('!'));
			check(!cover.isNull() && correct < other,
			      spark ? "Spark cover pixels contain that file's chosen prefix, number and suffix"
				    : "Variation cover pixels contain that file's chosen prefix, number and suffix");
		}
		driver.stop();
	}
	window.hide(); // destructor cleanup must not open the unsaved-project prompt
}
} // namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir temporary;
	if (!temporary.isValid()) {
		return 2;
	}
	qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
	QApplication app(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaCoverTests"));
	QCoreApplication::setApplicationName(QStringLiteral("NumberedCover"));
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
	QSettings().setValue(QStringLiteral("export/format"), int(ClipExporter::Format::Mp4));
	registerBuiltinComponents(ComponentRegistry::instance());
	cover::Numbering numbering;
	check(numbering.textFor(1).isEmpty(), "numbered covers are optional and default off");
	numbering.enabled = true;
	numbering.prefix = QStringLiteral("Day ");
	numbering.suffix = QStringLiteral(" — Devlog");
	numbering.start = 7;
	numbering.digits = 3;
	check(numbering.textFor(1) == QStringLiteral("Day 007 — Devlog") &&
		      numbering.textFor(12) == QStringLiteral("Day 018 — Devlog"),
	      "prefix, suffix, start and zero padding format a version number");
	numbering.includeNumber = false;
	check(numbering.textFor(3) == QStringLiteral("Day  — Devlog"), "the number itself is optional");
	numbering.includeNumber = true;
	ExportOptionsDialog::Context context;
	context.defaultName = QStringLiteral("cover-test");
	context.defaultFolder = temporary.path();
	context.sourceSize = QSize(320, 180);
	context.fps = 10;
	context.seconds = 2;
	context.variantLabels = {"One", "Two", "Three"};
	context.allowSpark = true;
	context.sparkBlocks = 2;
	context.sparkUniqueOrders = 2;
	context.sparkSeconds = 2;
	context.previewFrame = QImage(320, 180, QImage::Format_RGB32);
	context.previewFrame.fill(Qt::blue);
	{
		ExportOptionsDialog dialog(context);
		dialog.setBatchTabForTest(true);
		check(!dialog.numberedCover().enabled, "Variations has an off-by-default cover toggle");
		auto *toggle = dialog.findChild<QGroupBox *>(QStringLiteral("variationCoverEnabled"));
		toggle->setChecked(true);
		dialog.findChild<QLineEdit *>(QStringLiteral("variationCoverPrefix"))->setText(numbering.prefix);
		dialog.findChild<QLineEdit *>(QStringLiteral("variationCoverSuffix"))->setText(numbering.suffix);
		dialog.findChild<QSpinBox *>(QStringLiteral("variationCoverStart"))->setValue(numbering.start);
		dialog.findChild<QSpinBox *>(QStringLiteral("variationCoverDigits"))->setValue(numbering.digits);
		check(dialog.numberedCover().textFor(3) == QStringLiteral("Day 009 — Devlog") &&
			      !dialog.numberedCover().firstFrame,
		      "the dialog reports exactly the chosen label and keeps playback unchanged by default");
		dialog.setSparkTabForTest(true);
		check(!dialog.numberedCover().enabled, "Spark and Variations have independent optional cover settings");
		dialog.findChild<QGroupBox *>(QStringLiteral("sparkCoverEnabled"))->setChecked(true);
		dialog.findChild<QLineEdit *>(QStringLiteral("sparkCoverPrefix"))->setText(QStringLiteral("Spark "));
		dialog.findChild<QCheckBox *>(QStringLiteral("sparkCoverFirstFrame"))->setChecked(true);
		check(dialog.numberedCover().textFor(2) == QStringLiteral("Spark 2") &&
			      dialog.numberedCover().firstFrame,
		      "Spark exposes the separate first-frame compatibility choice");
		dialog.show();
		QTest::qWait(20);
		const auto args = QCoreApplication::arguments();
		const int snapshot = args.indexOf(QStringLiteral("--snapshot"));
		if (snapshot >= 0 && snapshot + 1 < args.size()) {
			check(dialog.grab().save(args[snapshot + 1]), "export dialog screenshot saves");
		}
		dialog.accept();
		check(dialog.result() == QDialog::Accepted, "valid cover choices can be accepted");
	}
	{
		ExportOptionsDialog dialog(context);
		dialog.setBatchTabForTest(true);
		check(dialog.numberedCover().textFor(3) == QStringLiteral("Day 009 — Devlog"),
		      "cover preferences survive reopening the export window");
		for (auto *combo : dialog.findChildren<QComboBox *>()) {
			if (combo->findText(QStringLiteral("MOV (H.264)")) >= 0) {
				combo->setCurrentIndex(combo->findText(QStringLiteral("MOV (H.264)")));
				dialog.accept();
				check(dialog.result() != QDialog::Accepted,
				      "unsupported containers cannot silently omit an enabled numbered cover");
				combo->setCurrentIndex(combo->findText(QStringLiteral("MP4 (H.264)")));
			}
		}
		dialog.findChild<QCheckBox *>(QStringLiteral("variationCoverNumber"))->setChecked(false);
		dialog.findChild<QLineEdit *>(QStringLiteral("variationCoverPrefix"))->clear();
		dialog.findChild<QLineEdit *>(QStringLiteral("variationCoverSuffix"))->clear();
		dialog.accept();
		check(dialog.result() != QDialog::Accepted, "an enabled but empty cover label is rejected");
		dialog.findChild<QGroupBox *>(QStringLiteral("variationCoverEnabled"))->setChecked(false);
		dialog.accept();
		check(dialog.result() == QDialog::Accepted, "turning off the feature restores ordinary exporting");
	}
	QSettings().clear();
	const QString red = temporary.filePath("red.mp4"), blue = temporary.filePath("blue.mp4");
	for (const auto &source : {qMakePair(red, QStringLiteral("red")), qMakePair(blue, QStringLiteral("blue"))}) {
		command({"-v", "error", "-f", "lavfi", "-i",
			 QStringLiteral("color=c=%1:s=320x180:r=10:d=1").arg(source.second), "-f", "lavfi", "-i",
			 "sine=frequency=440:sample_rate=48000:duration=1", "-c:v", "libx264", "-pix_fmt", "yuv420p",
			 "-c:a", "aac", "-shortest", "-y", source.first});
	}
	ClipExporter::Options options;
	// Lossless 4:4:4 makes exact pixel comparisons meaningful after frame zero
	// changes the encoder's reference frame. Phone-compatible 4:2:0 is tested below.
	options.videoCrf = 0;
	options.chroma444 = true;
	options.effort = ClipExporter::Options::Effort::Fast;
	options.outFps = 20;
	options.canvasW = 320;
	options.canvasH = 180;
	options.timelineFps = 10;
	options.timelineSources = {{0, red.toStdString()}, {1, blue.toStdString()}};
	TlClip a, b;
	a.srcEndMs = 1000;
	b = a;
	b.sourceId = 1;
	b.outStartMs = 1000;
	TlTrack track;
	track.clips = {a, b};
	options.timeline.tracks = {track};
	const QString plain = temporary.filePath("plain.mp4"), labelled = temporary.filePath("labelled.mp4"),
		      first = temporary.filePath("first.mp4");
	check(exportFile({}, plain, options), "baseline timeline with audio exports");
	if (failures) {
		return 1;
	}
	options.coverText = numbering.textFor(1);
	check(exportFile({}, labelled, options), "numbered MP4 cover exports and survives the timeline audio mix");
	if (failures) {
		return 1;
	}
	const auto image = decodeJpeg(coverData(labelled));
	const QRect text = whiteText(image);
	check(!image.isNull() && !text.isEmpty() && std::abs(text.center().x() - image.width() / 2) < 5 &&
		      std::abs(text.center().y() - image.height() / 2) < 5,
	      "the actual attached JPEG has centered white text");
	check(image.pixelColor(5, 5).blue() > 180 && image.pixelColor(5, 5).red() < 40,
	      "the cover retains a background frame from this video's finished render");
	const auto baselineFrames = frames(plain);
	check(baselineFrames.size() == 40 && frames(labelled) == baselineFrames,
	      "cover-only mode leaves every decoded playback frame and frame count unchanged");
	const auto baselineAudio = audio(plain);
	check(!baselineAudio.isEmpty() && audio(labelled) == baselineAudio,
	      "cover-only mode leaves decoded audio unchanged");
	options.coverFirstFrame = true;
	check(exportFile({}, first, options), "first-frame compatibility fallback exports");
	const auto firstFrames = frames(first);
	check(!firstFrames.isEmpty() && firstFrames.size() == baselineFrames.size() &&
		      firstFrames.first() != baselineFrames.first() && firstFrames.mid(1) == baselineFrames.mid(1),
	      "the fallback changes only frame zero, including repeated CFR source frames");
	check(audio(first) == baselineAudio, "the fallback preserves audio and synchronization");
	options.coverFirstFrame = false;
	options.format = ClipExporter::Format::Mov;
	const QString mov = temporary.filePath("labelled.mov");
	check(!exportFile({}, mov, options),
	      "a container that drops the cover is refused rather than reporting success");
	options.coverText.clear();
	check(exportFile({}, mov, options) && frames(mov) == baselineFrames,
	      "ordinary MOV exporting remains unchanged");
	options.coverText = numbering.textFor(1);
	options.format = ClipExporter::Format::Mp4;
	BatchExporter batch;
	QVector<BatchExporter::Item> items;
	for (int i = 1; i <= 2; ++i) {
		BatchExporter::Item item;
		item.name = QStringLiteral("variant_%1").arg(i);
		item.useTimeline = true;
		item.timeline = options.timeline;
		item.coverText = numbering.textFor(i);
		items.append(item);
	}
	const QString batchFolder = temporary.filePath("batch");
	check(batch.start({}, options, items, batchFolder), "a real batch accepts per-version cover labels");
	check(QTest::qWaitFor([&]() { return !batch.running(); }, 30000), "the numbered batch finishes");
	const auto results = batch.results();
	check(results.size() == 2 && results[0].ok && results[1].ok &&
		      coverData(results[0].path) != coverData(results[1].path) &&
		      frames(results[0].path) == baselineFrames && frames(results[1].path) == baselineFrames,
	      "each batch cover has its own number although the video content stays identical");
	// Legacy CFR uses held source frames; stamping one must not contaminate the next.
	options.timeline = {};
	options.coverText.clear();
	options.coverFirstFrame = false;
	options.startMs = 0;
	options.endMs = 1000;
	const QString trim = temporary.filePath("trim.mp4"), trimFirst = temporary.filePath("trim-first.mp4");
	check(exportFile(red, trim, options), "baseline trim export writes repeated CFR frames");
	options.coverText = QStringLiteral("7");
	options.coverFirstFrame = true;
	check(exportFile(red, trimFirst, options), "trim supports the same explicit first-frame fallback");
	const auto trimFrames = frames(trim), stampedTrimFrames = frames(trimFirst);
	check(trimFrames.size() == 20 && stampedTrimFrames.size() == 20 &&
		      trimFrames.first() != stampedTrimFrames.first() && trimFrames.mid(1) == stampedTrimFrames.mid(1),
	      "held frames after the first are never permanently stamped");
	options.timeline.tracks = {track};
	options.chroma444 = false;
	options.videoCrf = 12;
	const QString phone = temporary.filePath("phone.mp4");
	check(exportFile({}, phone, options) && !coverData(phone).isEmpty(),
	      "phone-compatible export includes the numbered cover and optional first frame");
	AVFormatContext *phoneFormat = nullptr;
	bool phoneCompatible = false;
	if (avformat_open_input(&phoneFormat, phone.toUtf8().constData(), nullptr, nullptr) >= 0) {
		avformat_find_stream_info(phoneFormat, nullptr);
		const int best = av_find_best_stream(phoneFormat, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (best >= 0) {
			const auto *stream = phoneFormat->streams[best];
			phoneCompatible = stream->codecpar->codec_id == AV_CODEC_ID_H264 &&
					  stream->codecpar->format == AV_PIX_FMT_YUV420P &&
					  std::abs(stream->duration * av_q2d(stream->time_base) - 2.0) < 0.01;
		}
	}
	avformat_close_input(&phoneFormat);
	check(phoneCompatible && frames(phone).size() == 40 && audio(phone) == baselineAudio,
	      "the compatible MP4 retains H.264 4:2:0, video duration, frame count and synchronized sound");
	checkEditorExports(temporary.path(), red, blue);
	std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
	return failures ? 1 : 0;
}
