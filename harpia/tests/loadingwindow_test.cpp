// Loading feedback through the real editor: opening each media kind, restoring
// a saved project and cleaning up unsuccessful/partial loads. The observer runs
// on the GUI thread, so seeing it during an import also checks that worker
// preparation leaves the event loop available to paint and update the dialog.
#include <QtWidgets>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

// Preload the window's dependencies before exposing only its private state.
// The production API need not grow test-only entry points for source imports.
#include "editor/BatchExport.hpp"
#include "editor/ClipExporter.hpp"
#include "editor/DevPanel.hpp"
#include "editor/SendToFull.hpp"
#include "editor/TrackEditor.hpp"
#include "editor/VoiceoverTrack.hpp"
#include "editor/component/ComponentPanel.hpp"
#include "editor/shader/ShaderEffect.hpp"
#include "editor/subtitles/Transcript.hpp"
#include "editor/timeline/SoundRules.hpp"
#include "editor/timeline/TimelineModel.hpp"
#include "editor/timeline/TimelineView.hpp"
#include "editor/timeline/ZoomKeyframes.hpp"
#define private public
#include "editor/VideoEditorWindow.hpp"
#undef private
#include "ui/LoadingDialog.hpp"

#include <cstdio>

using namespace harpia;

namespace {
int failures = 0;
void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}

struct Feedback {
	QString stage;
	QString detail;
	int completed = -1;
	int total = 0;
};

class LoadingObserver : public QObject {
public:
	LoadingObserver()
	{
		qApp->installEventFilter(this);
		timer.setInterval(0);
		QObject::connect(&timer, &QTimer::timeout, [&] { sample(); });
		timer.start();
	}

	void clear()
	{
		feedback.clear();
		busyTicks = 0;
		messageBoxes = 0;
		fileDialogs = 0;
		modal = true;
		sawApplicationModal = false;
		loadingDuringError = false;
	}

	bool saw(const QString &activity, const QString &filename = QString()) const
	{
		for (const Feedback &f : feedback)
			if (f.stage.contains(activity, Qt::CaseInsensitive) &&
			    (filename.isEmpty() || f.detail.contains(filename)))
				return true;
		return false;
	}

	bool sawItemCount(int total) const
	{
		for (const Feedback &f : feedback)
			if (f.total == total && f.completed >= 0 && f.completed <= total)
				return true;
		return false;
	}

	bool sawSourceCount(const QString &filename, int completed, int total) const
	{
		for (const Feedback &f : feedback)
			if (f.detail.contains(filename) && f.completed == completed && f.total == total)
				return true;
		return false;
	}

	bool countsInRange() const
	{
		for (const Feedback &f : feedback)
			if (f.total > 0 && (f.completed < 0 || f.completed > f.total)) {
				std::printf("    invalid item feedback: stage=%s detail=%s completed=%d total=%d\n",
					    qPrintable(f.stage), qPrintable(f.detail), f.completed, f.total);
				return false;
			}
		return true;
	}

	QVector<Feedback> feedback;
	int busyTicks = 0;
	int messageBoxes = 0;
	int fileDialogs = 0;
	bool modal = true;
	bool sawApplicationModal = false;
	bool loadingDuringError = false;

private:
	bool eventFilter(QObject *object, QEvent *event) override
	{
		// Final GUI-only stages repaint without dispatching queued work in the
		// middle of a project mutation. Observe that paint as well as the timer.
		if (event->type() == QEvent::Paint)
			if (auto *dialog = qobject_cast<LoadingDialog *>(object); dialog && dialog->isVisible())
				capture(dialog);
		return false;
	}

	void capture(LoadingDialog *dialog)
	{
		modal = modal && dialog->windowModality() != Qt::NonModal;
		sawApplicationModal = sawApplicationModal || dialog->windowModality() == Qt::ApplicationModal;
		auto *stage = dialog->findChild<QLabel *>(QStringLiteral("loadingStage"));
		auto *detail = dialog->findChild<QLineEdit *>(QStringLiteral("loadingDetail"));
		auto *progress = dialog->findChild<QProgressBar *>(QStringLiteral("loadingProgress"));
		Feedback current{stage ? stage->text() : QString(), detail ? detail->text() : QString(),
				 progress ? progress->value() : -1, progress ? progress->maximum() : 0};
		if (feedback.isEmpty() || feedback.last().stage != current.stage ||
		    feedback.last().detail != current.detail || feedback.last().completed != current.completed ||
		    feedback.last().total != current.total)
			feedback.append(current);
	}

	void sample()
	{
		bool loadingVisible = false;
		for (QWidget *widget : QApplication::topLevelWidgets()) {
			auto *dialog = qobject_cast<LoadingDialog *>(widget);
			if (!dialog || !dialog->isVisible())
				continue;
			loadingVisible = true;
			++busyTicks;
			capture(dialog);
		}
		// Individual failed imports report a normal error after loading ends.
		// Unexpected quiet-load dialogs are dismissed too, then fail the check
		// instead of hanging this regression test in a nested event loop.
		for (QWidget *widget : QApplication::topLevelWidgets()) {
			if (!widget->isVisible())
				continue;
			if (auto *box = qobject_cast<QMessageBox *>(widget)) {
				++messageBoxes;
				loadingDuringError = loadingDuringError || loadingVisible;
				box->done(QMessageBox::Ok);
			} else if (auto *picker = qobject_cast<QFileDialog *>(widget)) {
				++fileDialogs;
				picker->reject();
			}
		}
	}
	QTimer timer;
};

bool noLoadingDialog(const VideoEditorWindow &editor)
{
	if (editor.loadingDialog_)
		return false;
	for (QWidget *widget : QApplication::topLevelWidgets())
		if (qobject_cast<LoadingDialog *>(widget) && widget->isVisible())
			return false;
	return true;
}

bool hasCaption(const TimelineModel &model, const QString &text)
{
	for (const TlTrack &track : model.tracks)
		for (const TlClip &clip : track.clips)
			if (clip.type == TlClip::Type::Text && clip.text.text == text)
				return true;
	return false;
}

void settleEvents()
{
	QEventLoop loop;
	QTimer::singleShot(25, &loop, &QEventLoop::quit);
	loop.exec();
}

bool writeTone(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	// Ten seconds gives the worker real decoding work without a large fixture.
	constexpr quint32 rate = 48000, samples = rate * 10, dataBytes = samples * 2;
	QDataStream out(&file);
	out.setByteOrder(QDataStream::LittleEndian);
	out.writeRawData("RIFF", 4);
	out << quint32(36 + dataBytes);
	out.writeRawData("WAVEfmt ", 8);
	out << quint32(16) << quint16(1) << quint16(1) << rate << quint32(rate * 2)
	    << quint16(2) << quint16(16);
	out.writeRawData("data", 4);
	out << dataBytes;
	for (quint32 i = 0; i < samples; ++i)
		out << qint16(std::sin(double(i) * 6.283185307179586 * 440.0 / rate) * 10000);
	return out.status() == QDataStream::Ok;
}

bool writeJson(const QString &path, const QJsonObject &object)
{
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(object).toJson()) > 0;
}

QJsonObject readJson(const QString &path)
{
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

QString fixtureVideo(int argc, char **argv)
{
	QString path;
	if (argc > 1) {
		path = QString::fromLocal8Bit(argv[1]);
		if (QFileInfo(path).isDir())
			path = QDir(path).filePath(QStringLiteral("av_red.mp4"));
	}
#ifdef SRC_MEDIA
	if (path.isEmpty())
		path = QStringLiteral(SRC_MEDIA);
#endif
	return path;
}
} // namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QTemporaryDir temp;
	if (!temp.isValid())
		return 2;
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.filePath(QStringLiteral("settings")));
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaLoadingTest"));
	QCoreApplication::setApplicationName(QStringLiteral("Editor"));
	const QString original = fixtureVideo(argc, argv);
	if (!QFileInfo::exists(original)) {
		std::printf("usage: loadingwindow_test <video file or directory containing av_red.mp4>\n");
		return 2;
	}
	const QString video = temp.filePath(QStringLiteral("source & clip.mp4"));
	const QString image = temp.filePath(QStringLiteral("image & plate.png"));
	const QString audio = temp.filePath(QStringLiteral("audio & tone.wav"));
	QImage pixels(320, 180, QImage::Format_RGB32);
	pixels.fill(QColor(60, 130, 210));
	if (!QFile::copy(original, video) || !pixels.save(image) || !writeTone(audio)) {
		std::printf("could not create loading fixtures\n");
		return 2;
	}
	LoadingObserver observer;
	VideoEditorWindow editor{QString()};
	editor.show();
	QApplication::processEvents();
	check(editor.isValid() && observer.feedback.isEmpty() && noLoadingDialog(editor),
	      "a blank editor opens without an unnecessary loading dialog");

	observer.clear();
	TlClip pendingCaption;
	pendingCaption.type = TlClip::Type::Text;
	pendingCaption.srcEndMs = 1000;
	pendingCaption.text.text = QStringLiteral("caption completed while importing media");
	bool captionCallbackRan = false, captionCallbackSawLoader = false, captionWasDeferred = false;
	QTimer::singleShot(0, &editor, [&] {
		captionCallbackRan = true;
		captionCallbackSawLoader = editor.loadingDialog_ != nullptr;
		editor.addSubtitleClips({pendingCaption}, QStringLiteral("Prepared captions"));
		captionWasDeferred = !hasCaption(editor.timelineView_->model(), pendingCaption.text.text);
	});
	const int videoId = editor.addSource(video);
	check(videoId >= 0 && observer.saw(QStringLiteral("video"), QFileInfo(video).fileName()),
	      "opening video shows the activity and the actual source filename");
	check(observer.busyTicks > 0 && observer.modal, "the video worker keeps GUI feedback running with a modal owner");
	check(noLoadingDialog(editor), "video preparation closes its dialog before source activation");
	check(captionCallbackRan && captionCallbackSawLoader && captionWasDeferred &&
	      !hasCaption(editor.timelineView_->model(), pendingCaption.text.text),
	      "a modeless caption result cannot mutate the timeline during video preparation");
	settleEvents();
	check(hasCaption(editor.timelineView_->model(), pendingCaption.text.text) && noLoadingDialog(editor),
	      "the deferred caption result is applied after media loading returns to the same project");
	if (videoId < 0)
		return 1;
	editor.setActiveSource(videoId);
	check(editor.activeSourceIdForTest() == videoId && noLoadingDialog(editor),
	      "the prepared video becomes the active source normally");

	// A proxy becoming ready during a media wait must be applied before the
	// next playback, rather than remaining stranded in the deferred map.
	QTimer::singleShot(0, &editor, [&]() { editor.applyProxyToPlayback(videoId, video); });
	observer.clear();
	const int imageId = editor.addImageSource(image);
	check(imageId >= 0 && observer.saw(QStringLiteral("image"), QFileInfo(image).fileName()),
	      "reading a still image shows the filename during worker preparation");
	check(imageId >= 0 && editor.stillImages_.contains(imageId) && noLoadingDialog(editor),
	      "image pixels are registered and loading is cleaned up");
	check(editor.pendingPlaybackProxy_.isEmpty() && editor.playbackPath_.value(videoId) == video,
	      "a proxy arriving during loading is applied when the loading scope finishes");

	observer.clear();
	const int audioId = editor.addAudioSource(audio);
	check(audioId >= 0 && observer.saw(QStringLiteral("audio"), QFileInfo(audio).fileName()),
	      "audio decoding explains which original file is being prepared");
	check(observer.busyTicks > 1 && observer.modal, "GUI heartbeat continues while the audio worker decodes");
	check(audioId >= 0 && QFileInfo::exists(editor.sourcePathForTest(audioId)) &&
	      editor.sourceFileForTest(audioId) == audio && noLoadingDialog(editor),
	      "decoded audio is usable while the project retains the original path");
	if (imageId < 0 || audioId < 0)
		return 1;

	TimelineModel timeline;
	TlTrack picture;
	picture.name = QStringLiteral("Pictures");
	TlClip clip;
	clip.sourceId = videoId;
	clip.srcEndMs = 1000;
	picture.clips.append(clip);
	clip.type = TlClip::Type::Image;
	clip.sourceId = imageId;
	clip.outStartMs = 1000;
	picture.clips.append(clip);
	TlTrack sound;
	sound.name = QStringLiteral("Audio");
	sound.kind = TlTrack::Kind::Audio;
	clip.type = TlClip::Type::Video;
	clip.sourceId = audioId;
	clip.outStartMs = 0;
	clip.srcEndMs = 2000;
	sound.clips.append(clip);
	timeline.tracks = {picture, sound};
	editor.timelineView_->setModel(timeline);
	const QString project = temp.filePath(QStringLiteral("all media.harpiaproj"));
	check(editor.saveProjectTo(project, true).isEmpty(), "a real project containing video, image and audio is saved");

	VideoEditorWindow reopened{QString()};
	reopened.show();
	QApplication::processEvents();
	observer.clear();
	TlClip obsoleteCaption = pendingCaption;
	obsoleteCaption.text.text = QStringLiteral("obsolete caption from the previous project");
	bool obsoleteCallbackRan = false, obsoleteCallbackSawLoader = false, obsoleteWasDeferred = false;
	QTimer::singleShot(0, &reopened, [&] {
		obsoleteCallbackRan = true;
		obsoleteCallbackSawLoader = reopened.loadingDialog_ != nullptr;
		reopened.addSubtitleClips({obsoleteCaption}, QStringLiteral("Previous-project captions"));
		obsoleteWasDeferred = !hasCaption(reopened.timelineView_->model(), obsoleteCaption.text.text);
	});
	const bool opened = reopened.openProjectAt(project);
	check(opened && observer.saw(QStringLiteral("project")), "opening a saved project displays its reading activity");
	check(observer.saw(QStringLiteral("video"), QFileInfo(video).fileName()) &&
	      observer.saw(QStringLiteral("image"), QFileInfo(image).fileName()) &&
	      observer.saw(QStringLiteral("audio"), QFileInfo(audio).fileName()),
	      "a fresh project reports every media kind while rebuilding its source pool");
	check(observer.sawItemCount(3) && observer.countsInRange(),
	      "project media progress uses the actual three-item total and completed-item bounds");
	check(observer.sawSourceCount(QFileInfo(video).fileName(), 0, 3) &&
	      observer.sawSourceCount(QFileInfo(image).fileName(), 1, 3) &&
	      observer.sawSourceCount(QFileInfo(audio).fileName(), 2, 3),
	      "each source reports how many earlier files are complete, without counting itself early");
	check(observer.saw(QStringLiteral("waveform")) && observer.saw(QStringLiteral("timeline")),
	      "waveform building and timeline restoration have their own activity messages");
	check(observer.busyTicks > 1 && observer.modal && observer.messageBoxes == 0 && observer.fileDialogs == 0,
	      "quiet project loading keeps GUI feedback alive without extra dialogs");
	const TimelineModel restored = reopened.timelineView_->model();
	const bool restoredAudio = restored.tracks.size() == 2 && restored.tracks[1].clips.size() == 1 &&
		!restored.tracks[1].clips.first().peaks.isEmpty();
	check(reopened.sources_.size() == 3 && restoredAudio && !reopened.loadingProject_ && noLoadingDialog(reopened),
	      "project sources and waveforms finish intact and the loading guard is released");
	check(obsoleteCallbackRan && obsoleteCallbackSawLoader && obsoleteWasDeferred,
	      "a caption result arriving during project reading cannot alter the timeline being replaced");
	settleEvents();
	check(!hasCaption(reopened.timelineView_->model(), obsoleteCaption.text.text) &&
	      reopened.timelineView_->model().tracks.size() == restored.tracks.size() && noLoadingDialog(reopened),
	      "a deferred result from the previous project is discarded after restoration");

	const QString malformed = temp.filePath(QStringLiteral("broken.harpiaproj"));
	{
		QFile file(malformed);
		if (!file.open(QIODevice::WriteOnly))
			qFatal("Could not open the test fixture file.");
		file.write("{ definitely not JSON");
	}
	observer.clear();
	check(!reopened.openProjectAt(malformed) && !reopened.loadingProject_ && noLoadingDialog(reopened),
	      "malformed project JSON returns failure and leaves no loading state behind");
	check(observer.messageBoxes == 0 && observer.fileDialogs == 0 && reopened.sources_.size() == 3,
	      "quiet parse failure leaves the current sources intact without a warning popup");
	check(!reopened.openProjectAt(temp.filePath(QStringLiteral("absent.harpiaproj"))) &&
	      !reopened.loadingProject_ && noLoadingDialog(reopened),
	      "an unreadable project also releases loading feedback");

	QJsonObject partial = readJson(project);
	QJsonArray sources = partial.value(QStringLiteral("sources")).toArray();
	for (int i = 0; i < sources.size(); ++i) {
		QJsonObject source = sources[i].toObject();
		source[QStringLiteral("path")] = temp.filePath(QStringLiteral("missing_%1.%2").arg(i)
			.arg(QFileInfo(source.value(QStringLiteral("path")).toString()).suffix()));
		sources[i] = source;
	}
	partial[QStringLiteral("sources")] = sources;
	const QString missing = temp.filePath(QStringLiteral("missing media.harpiaproj"));
	check(writeJson(missing, partial), "the missing-media project fixture is written");
	VideoEditorWindow missingEditor{QString()};
	missingEditor.show();
	QApplication::processEvents();
	observer.clear();
	check(missingEditor.openProjectAt(missing), "valid project JSON still opens when its media is missing");
	check(observer.messageBoxes == 0 && observer.fileDialogs == 0,
	      "missing media stays quiet during an automated project open");
	check(!missingEditor.timelineView_->model().isEmpty() && !missingEditor.loadingProject_ &&
	      noLoadingDialog(missingEditor), "the partial timeline survives and releases every loading guard");

	observer.clear();
	check(missingEditor.addSource(temp.filePath(QStringLiteral("bad video.mp4"))) < 0,
	      "an individual unreadable video fails cleanly");
	check(observer.messageBoxes == 1 && !observer.loadingDuringError && noLoadingDialog(missingEditor),
	      "the normal video error is shown after its loading dialog has closed");
	observer.clear();
	check(missingEditor.addImageSource(temp.filePath(QStringLiteral("bad image.png"))) < 0,
	      "an individual unreadable image fails cleanly");
	check(observer.messageBoxes == 1 && !observer.loadingDuringError && noLoadingDialog(missingEditor),
	      "the normal image error releases loading feedback first");
	observer.clear();
	check(missingEditor.addAudioSource(temp.filePath(QStringLiteral("bad audio.wav"))) < 0,
	      "an individual unreadable audio source fails cleanly");
	check(observer.messageBoxes == 1 && !observer.loadingDuringError && noLoadingDialog(missingEditor) &&
	      !missingEditor.loadingProject_, "the normal audio error releases loading feedback and project state");

	VideoEditorWindow pasted{QString()};
	pasted.show();
	QApplication::processEvents();
	observer.clear();
	check(pasted.pasteMediaFiles({video, image, audio}, 0), "a mixed media paste imports all three files together");
	check(observer.sawSourceCount(QFileInfo(video).fileName(), 0, 3) &&
	      observer.sawSourceCount(QFileInfo(image).fileName(), 1, 3) &&
	      observer.sawSourceCount(QFileInfo(audio).fileName(), 2, 3) && observer.countsInRange(),
	      "a mixed media paste reports the real zero-based completed counts for each file");
	bool pastedVideo = false, pastedImage = false, pastedAudio = false;
	int pastedClips = 0;
	for (const TlTrack &track : pasted.timelineView_->model().tracks)
		for (const TlClip &item : track.clips) {
			++pastedClips;
			const QString file = pasted.sourceFileForTest(item.sourceId);
			pastedVideo = pastedVideo || (file == video && item.type == TlClip::Type::Video);
			pastedImage = pastedImage || (file == image && item.type == TlClip::Type::Image);
			pastedAudio = pastedAudio || (file == audio && track.kind == TlTrack::Kind::Audio);
		}
	check(pasted.sources_.size() == 3 && pastedClips == 3 && pastedVideo && pastedImage && pastedAudio &&
	      !pasted.loadingProject_ && noLoadingDialog(pasted),
	      "a batch paste leaves three complete sources and their picture/audio clips with no loading dialog");

	QJsonObject legacy = readJson(project);
	legacy[QStringLiteral("harpiaProject")] = 1;
	legacy[QStringLiteral("sourceName")] = QStringLiteral("a different recording.mp4");
	const QString priorProjectPath = editor.projectPath_;
	observer.clear();
	// The observer answers Ok, which is not Yes to the legacy mismatch prompt.
	editor.applyProjectJson(legacy, temp.filePath(QStringLiteral("legacy.harpiaproj")), false, false);
	check(observer.messageBoxes == 1 && editor.projectPath_ == priorProjectPath &&
	      !editor.loadingProject_ && noLoadingDialog(editor),
	      "declining a legacy source mismatch releases the project loading guard without applying it");

	observer.clear();
	{
		VideoEditorWindow launched(video);
		check(launched.isValid() && observer.saw(QStringLiteral("video"), QFileInfo(video).fileName()) &&
		      observer.sawApplicationModal && noLoadingDialog(launched),
		      "opening a Library video shows application-modal loading before the editor itself is visible");
	}

	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
