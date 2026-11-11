// Opening a document replaces its media pool; missing files remain distinct
// recoverable references instead of silently becoming another video.
#include <QtWidgets>
#include <map>
#include <memory>
#include <thread>
#include <vector>
#define private public
#include "editor/VideoEditorWindow.hpp"
#undef private
#include "editor/PreviewDecoder.hpp"
#include "editor/TrackEditor.hpp"
#include "editor/EditorWidgets.hpp"
#include "editor/timeline/TimelineJson.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <cstdio>
#include <QTest>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}
bool makeMedia(const QString &path, const QString &filter, bool audio = false)
{
	QProcess ffmpeg;
	QStringList arguments = {"-v", "error", "-f", "lavfi", "-i", filter};
	arguments << "-c:a" << "pcm_s16le";
	if (!audio)
		arguments = {"-v", "error", "-f", "lavfi", "-i", filter, "-c:v", "libx264", "-preset", "ultrafast", "-g", "1"};
	arguments << "-y" << path;
	ffmpeg.start("ffmpeg", arguments);
	return ffmpeg.waitForFinished(30000) && ffmpeg.exitCode() == 0;
}
void write(const QString &path, const QJsonObject &root)
{
	QFile file(path);
	check(file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(root).toJson()) > 0,
	      "the project fixture can be written");
}
QJsonObject read(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};
	return QJsonDocument::fromJson(file.readAll()).object();
}
QJsonObject source(int id, const QString &path, int width, int height, double fps = 0.0)
{
	return {{"id", id}, {"path", path}, {"name", QFileInfo(path).fileName()}, {"durationMs", 1000},
		{"width", width}, {"height", height}, {"fps", fps}};
}
TlClip clip(int sourceId, qint64 start = 0, qint64 length = 1000)
{
	TlClip result;
	result.sourceId = sourceId;
	result.outStartMs = start;
	result.srcEndMs = length;
	return result;
}
QJsonObject project(const QJsonArray &sources, const TimelineModel &timeline, int active, const QString &mode = "full")
{
	QJsonObject root{{"harpiaProject", 3}, {"sources", sources}, {"activeSource", active},
		{"editMode", mode}, {"trimStart", 0}, {"trimEnd", 1000}, {"speed", 1.0}};
	timelineToProject(timeline, root);
	return root;
}
EditorSource *byName(VideoEditorWindow &window, const QString &name)
{
	for (EditorSource &source : window.sources_)
		if (source.name == name)
			return &source;
	return nullptr;
}
bool findFile(const std::function<void()> &open, const QString &replacement)
{
	bool sawPicker = false;
	QElapsedTimer elapsed;
	elapsed.start();
	QTimer driver;
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		if (elapsed.elapsed() > 5000) {
			if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
				dialog->reject();
			}
			if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
				menu->close();
			}
			return;
		}
		if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
			for (auto *action : menu->actions()) {
				if (action->objectName() == QStringLiteral("findMissingMediaAction")) {
					menu->setActiveAction(action);
					QTest::keyClick(menu, Qt::Key_Return);
					return;
				}
			}
			menu->close();
		} else if (auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
			if (sawPicker) {
				return;
			}
			sawPicker = true;
			if (replacement.isEmpty()) {
				picker->reject();
			} else {
				picker->setDirectory(QFileInfo(replacement).absolutePath());
				QTimer::singleShot(50, picker, [picker, replacement]() {
					if (auto *name =
						    picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))) {
						name->setText(replacement);
					} else {
						picker->selectFile(replacement);
					}
					QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
				});
			}
		}
	});
	driver.start(10);
	open();
	return sawPicker;
}
}

int main(int argc, char **argv)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
	Q_UNUSED(argc);
	Q_UNUSED(argv);
	std::printf("SKIP: native application settings cannot be redirected safely on this platform\n");
	return 77;
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QTemporaryDir dir;
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
	QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, dir.path());
	QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());
	QSettings::setPath(QSettings::NativeFormat, QSettings::SystemScope, dir.path());
	QDir().mkpath(dir.filePath("assets"));
	const QString red = dir.filePath("red.mp4"), blue = dir.filePath("assets/blue.mp4");
	const QString tone = dir.filePath("assets/tone.wav"), still = dir.filePath("assets/still.png");
	QImage picture(37, 25, QImage::Format_RGB32);
	picture.fill(Qt::blue);
	check(makeMedia(red, "color=c=red:s=64x48:r=60:d=1") &&
	      makeMedia(blue, "color=c=blue:s=96x64:r=30:d=1") &&
	      makeMedia(tone, "sine=sample_rate=48000:duration=1", true) && picture.save(still),
	      "made video, still and audio fixtures with different document formats");
	VideoEditorWindow window(red, {});
	const int oldVideoId = window.activeSourceId_;
	const int oldStillId = window.addImageSource(still);
	const int oldAudioId = window.addAudioSource(tone);
	TimelineModel before;
	TlTrack beforeTrack;
	beforeTrack.clips = {clip(oldVideoId)};
	before.tracks = {beforeTrack};
	window.timelineView_->setModel(before);
	window.captureSnapshot();
	window.clipboard_ = {{clip(oldVideoId), 0}};
	window.trackClipboard_ = beforeTrack;
	window.haveTrackClipboard_ = true;
	window.trackCopiedFrom_ = 0;
	window.copySeq_ = 1;
	window.trackCopySeq_ = 2;
	window.componentClipboardValid_ = true;
	window.randomClipClipboard_ = TlClipOption{oldVideoId, 0};
	window.componentClipboard_.typeId = QStringLiteral("harpia.blur");
	check(window.timelineFps() == 60 && window.timelineCanvasSize() == QSize(64, 48),
	      "the original document follows its own video dimensions and frame rate");
	TimelineModel next;
	TlTrack pictures, audio;
	pictures.clips = {clip(77)};
	TlClip image = clip(78, 1000);
	image.type = TlClip::Type::Image;
	pictures.clips.append(image);
	audio.kind = TlTrack::Kind::Audio;
	audio.clips = {clip(79)};
	next.tracks = {pictures, audio};
	const QString nextPath = dir.filePath("next.harpiaproj");
	QJsonObject namedAudio = source(79, "assets/tone.wav", 0, 0);
	namedAudio["name"] = "Recorded narration";
	namedAudio["librarySlot"] = "Unconfigured readable test slot";
	QJsonObject videoAlias = source(80, "assets/blue.mp4", 96, 64, 30);
	videoAlias["name"] = "Unrelated alias";
	QJsonObject audioAlias = namedAudio;
	audioAlias["id"] = 81;
	audioAlias["name"] = "Second narration alias";
	write(nextPath, project({source(77, "assets/blue.mp4", 96, 64, 30), source(78, "assets/still.png", 37, 25),
		 namedAudio, videoAlias, audioAlias}, next, 77));
	check(window.openProjectAt(nextPath), "a second project opens in the existing editor window");
	check(window.sources_.size() == 3 && !window.sourceById(oldVideoId) && !window.sourceById(oldStillId) &&
	      !window.sourceById(oldAudioId) && window.timelineCanvasSize() == QSize(96, 64) && window.timelineFps() == 30,
	      "the loaded document owns its media pool and Auto format instead of inheriting the previous project");
	check(window.history_.size() == 1 && window.histIndex_ == 0 && !window.undoBtn_->isEnabled(),
	      "opening a new document cannot undo into clips whose old media pool was released");
	check(window.clipboard_.isEmpty() && !window.haveTrackClipboard_ && window.trackClipboard_.clips.isEmpty() &&
		      window.copySeq_ == 0 && window.trackCopySeq_ == 0 && window.componentClipboardValid_ &&
		      !window.randomClipClipboard_,
	      "document replacement drops clip and track clipboard source IDs while retaining reusable component styles");
	check(window.stillImages_.size() == 1 && !window.stillImages_.contains(oldStillId) &&
	      window.audioProxy_.size() == 1 && !window.audioProxy_.contains(oldAudioId),
	      "still-image and decoded-audio caches contain only the loaded document's sources");
	window.onProxyReady(oldVideoId, dir.filePath("obsolete-proxy.mp4"));
	check(window.previewDecoder_->decodePathFor(oldVideoId).isEmpty() && !window.pendingPlaybackProxy_.contains(oldVideoId),
	      "late proxy completion from the old document cannot repopulate decode state");
	check(QFileInfo(window.sourceById(window.activeSourceId_)->path).absoluteFilePath() == blue,
	      "relative video paths resolve against the project folder");
	check(byName(window, "Recorded narration") && byName(window, "Recorded narration")->origPath == tone,
	      "an audio source retains its friendly saved name instead of adopting the asset filename");
	check(byName(window, "Recorded narration") && byName(window, "Recorded narration")->librarySlot == "Unconfigured readable test slot",
	      "an unconfigured sound-library slot retains its binding when its saved fallback WAV is readable");
	check(byName(window, "blue.mp4") && !byName(window, "Unrelated alias") && !byName(window, "Second narration alias"),
	      "duplicate video and original-audio paths reuse their first saved name instead of overwriting it with another alias");
	const QString namedAgain = dir.filePath("named-again.harpiaproj");
	bool namedSaved = window.saveProjectTo(namedAgain, true).isEmpty();
	bool friendlyName = false;
	bool savedSlot = false;
	for (const QJsonValue &value : read(namedAgain)["sources"].toArray())
		if (value.toObject()["name"].toString() == "Recorded narration") {
			friendlyName = true;
			savedSlot = value.toObject()["librarySlot"].toString() == "Unconfigured readable test slot";
		}
	check(namedSaved && friendlyName && savedSlot, "friendly source names and fallback sound-library bindings survive an actual load and resave");

	QJsonObject missingVideo = source(88, "moved/missing.mp4", 1280, 720, 120);
	missingVideo["durationMs"] = 9000;
	QJsonObject missingAudio = source(100, "moved/missing.wav", 0, 0);
	missingAudio["librarySlot"] = "Unconfigured test sound slot";
	TimelineModel missingModel;
	TlTrack missingPictures;
	missingPictures.clips = {clip(88, 0, 2000), clip(77, 2500)};
	missingPictures.clips[1].clipOptions = {{77, 0}, {99, 100}};
	TlTrack missingSound;
	missingSound.kind = TlTrack::Kind::Audio;
	missingSound.clips = {clip(100, 0, 500)};
	missingModel.tracks = {missingPictures, missingSound};
	TlSoundRule rule;
	rule.id = 1;
	rule.enabled = false;
	rule.sourceId = 100;
	missingModel.soundRules = {rule};
	const QString missingPath = dir.filePath("missing.harpiaproj");
	QJsonObject missingRoot = project({missingVideo, source(77, "assets/blue.mp4", 96, 64, 30),
		source(99, "moved/alternative.mov", 320, 180, 25), missingAudio,
		source(101, "unused/no-longer-used.mp4", 640, 360, 60)}, missingModel, 88, "multicut");
	missingRoot["segments"] = QJsonArray{QJsonObject{{"srcStart", 100}, {"srcEnd", 900}, {"source", 88}, {"speed", 1.0}}};
	write(missingPath, missingRoot);
	bool prompted = false;
	QTimer closePrompts;
	QObject::connect(&closePrompts, &QTimer::timeout, [&]() {
		if (auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
			prompted = true;
			picker->reject();
		} else if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
			prompted = true;
			message->accept();
		}
	});
	closePrompts.start(10);
	window.openProjectWithDialogs(missingPath, false);
	closePrompts.stop();
	check(window.projectPath_ == missingPath && !prompted,
	      "the interactive project-open path never asks to locate or acknowledge missing media");
	EditorSource *missing = byName(window, "missing.mp4"), *alternative = byName(window, "alternative.mov");
	EditorSource *sound = byName(window, "missing.wav"), *available = byName(window, "blue.mp4");
	check(missing && alternative && sound && available && window.sources_.size() == 4 && missing->missing &&
	      alternative->missing && sound->missing && !available->missing,
	      "each used missing video, Random alternative and audio source gets its own placeholder; unused missing files stay omitted");
	if (missing && alternative && sound && available) {
		const TimelineModel &loaded = window.timelineView_->model();
		check(missing->durationMs == 9000 && missing->width == 1280 && missing->height == 720 && missing->fps == 120 &&
		      sound->librarySlot == "Unconfigured test sound slot" && window.timelineCanvasSize() == QSize(1280, 720) &&
		      window.timelineFps() == 120,
		      "placeholders retain original geometry, duration, FPS and sound-library identity");
		check(window.activeSourceId_ == missing->id && !window.seeker_ && window.multiCutSourceCombo_->currentData().toInt() == missing->id &&
		      window.lastPreviewRaw_.isNull(),
		      "a missing active source stays selected without a dangling decoder or another video's picture");
		check(loaded.tracks[0].clips[0].sourceId == missing->id && window.tracks_->segments()[0].sourceId == missing->id &&
		      loaded.tracks[0].clips[1].clipOptions[1].sourceId == alternative->id &&
		      loaded.tracks[1].clips[0].sourceId == sound->id && loaded.soundRules[0].sourceId == sound->id &&
		      missing->id != available->id && alternative->id != available->id,
		      "cuts, timeline clips, Random choices and sound rules preserve missing-file references instead of substituting another source");
		const QString again = dir.filePath("missing-again.harpiaproj");
		check(window.saveProjectTo(again, true).isEmpty(), "a project containing missing references can be saved safely");
		const QJsonArray saved = read(again)["sources"].toArray();
		bool keptPath = false, keptAlternative = false, keptSlot = false;
		for (const QJsonValue &value : saved) {
			const QJsonObject object = value.toObject();
			keptPath |= object["path"].toString() == dir.filePath("moved/missing.mp4") && object["fps"].toDouble() == 120;
			keptAlternative |= object["path"].toString() == dir.filePath("moved/alternative.mov");
			keptSlot |= object["librarySlot"].toString() == "Unconfigured test sound slot";
		}
		check(saved.size() == 4 && keptPath && keptAlternative && keptSlot,
		      "save after a missing-media load retains recoverable paths and metadata");
		const int missingId = missing->id, audioId = sound->id, alternativeId = alternative->id;
		check(window.timelineView_->missingSources_.contains(missingId) &&
			      window.tracks_->missingSources_.contains(missingId) &&
			      window.timelineView_->missingSources_.contains(alternativeId),
		      "Full Editing and Multi-Cut receive missing-source labels, including hidden Random alternatives");
		const TimelineModel beforeRelink = window.timelineView_->model();
		const auto cutsBeforeRelink = window.tracks_->segments();
		const int historyBeforeRelink = window.histIndex_;
		check(findFile(
			      [&]() {
				      window.tracks_->showSegmentMenu(0, QPoint(100, 100),
								      window.tracks_->segmentRects().front().center());
			      },
			      {}) &&
			      window.sourceById(missingId)->missing && window.timelineView_->model() == beforeRelink &&
			      window.histIndex_ == historyBeforeRelink,
		      "cancelling Find missing file leaves every edit and recoverable path unchanged");
		window.setEditMode(VideoEditorWindow::EditMode::Full);
		check(findFile([&]() { window.timelineView_->showClipMenu(0, 0, QPoint(100, 100), 0); }, blue),
		      "a missing timeline clip's right-click action opens the file picker on demand");
		check(window.sources_.size() == 4 && !window.sourceById(missingId)->missing &&
			      window.sourceById(missingId)->path == blue && window.sourceById(missingId)->seeker &&
			      window.timelineView_->model() == beforeRelink &&
			      window.tracks_->segments() == cutsBeforeRelink &&
			      window.histIndex_ == historyBeforeRelink &&
			      !window.timelineView_->missingSources_.contains(missingId) &&
			      !window.tracks_->missingSources_.contains(missingId),
		      "relinking restores the original source ID for all clips and cuts without changing edits or undo history");
		check(QTest::qWaitFor(
			      [&]() {
				      const QImage &frame = window.canvas_->currentFrame();
				      return !frame.isNull() &&
					     frame.pixelColor(frame.width() / 2, frame.height() / 2).blue() > 180;
			      },
			      3000),
		      "the recovered clip renders immediately instead of leaving its missing frame cached");
		window.timelineView_->selectClip(0, 1);
		check(findFile([&]() { window.showRandomClipOptionMenu(1, QPoint(100, 100)); }, red) &&
			      window.sourceById(alternativeId)->seeker && window.timelineView_->model() == beforeRelink,
		      "finding a missing Random alternative repairs its existing option binding without replacing its timestamp");
		check(window.relinkProjectSource(audioId, tone) && !window.sourceById(audioId)->missing &&
			      window.sourceById(audioId)->origPath == tone &&
			      QFileInfo(window.sourceById(audioId)->path).isFile() &&
			      window.sourceById(audioId)->librarySlot == "Unconfigured test sound slot" &&
			      window.timelineView_->model() == beforeRelink,
		      "missing audio recovers through the audio decoder while clips and sound rules keep their stable references");
		const QString relinkedPath = dir.filePath("relinked.harpiaproj");
		check(window.saveProjectTo(relinkedPath, true).isEmpty() && window.openProjectAt(relinkedPath),
		      "recovered source paths survive saving and reopening the project");
		bool relinked = true;
		for (const EditorSource &entry : window.sources_) {
			relinked &= !entry.missing;
		}
		check(relinked, "reopening the repaired project needs no missing placeholders");
		QDir().mkpath(dir.filePath("moved"));
		check(QFile::copy(blue, dir.filePath("moved/missing.mp4")) && QFile::copy(blue, dir.filePath("moved/alternative.mov")) &&
		      QFile::copy(tone, dir.filePath("moved/missing.wav")), "the missing fixture media can be restored to disk");
		check(!window.sourceByPath(dir.filePath("moved/missing.mp4")),
		      "a restored file's undecoded placeholder is never mistaken for a successfully opened source");
		check(window.openProjectAt(again), "the saved project reopens after its files have been restored");
		bool recovered = window.sources_.size() == 4;
		for (const EditorSource &entry : window.sources_)
			recovered &= !entry.missing;
		check(recovered, "restoring the files recovers all original references on the next project open");
	}

	TimelineModel missingImageModel;
	TlTrack imageTrack;
	TlClip missingImage = clip(7);
	missingImage.type = TlClip::Type::Image;
	imageTrack.clips = {missingImage};
	missingImageModel.tracks = {imageTrack};
	const QString missingImageProject = dir.filePath("missing-image.harpiaproj");
	write(missingImageProject, project({source(7, "missing/picture.png", 37, 25)}, missingImageModel, 7));
	check(window.openProjectAt(missingImageProject), "a missing-image project opens normally");
	const int imageId = window.activeSourceId_;
	const TimelineModel imageBeforeRelink = window.timelineView_->model();
	check(window.relinkProjectSource(imageId, still) && window.stillImages_.contains(imageId) &&
		      !window.sourceById(imageId)->missing && window.timelineView_->model() == imageBeforeRelink &&
		      !window.timelineView_->missingSources_.contains(imageId),
	      "an image replacement is decoded as a still and repairs the existing image clip without rewriting it");

	TimelineModel emptyLayout;
	TlTrack authored;
	authored.name = "Empty authored lane";
	authored.rippleSpeed = true;
	emptyLayout.tracks = {authored};
	const QString emptyPath = dir.filePath("empty-layout.harpiaproj");
	write(emptyPath, project({source(77, "assets/blue.mp4", 96, 64, 30)}, emptyLayout, 77));
	check(window.openProjectAt(emptyPath) && window.fullEdit() && window.timelineView_->model().tracks.size() == 1 &&
	      window.timelineView_->model().tracks[0].clips.isEmpty() && window.timelineView_->model().tracks[0].name == authored.name,
	      "an empty authored Full Editing lane survives loading without automatic video seeding");
	check(window.hasUnsavedEdits(), "an authored empty Full Editing layout is protected by the close prompt");
	const QString emptyAgain = dir.filePath("empty-layout-again.harpiaproj");
	check(window.saveProjectTo(emptyAgain, true).isEmpty() && read(emptyAgain)["tracks"].toArray().size() == 1,
	      "an empty authored track layout survives an actual GUI load and resave");
	const QString captions = dir.filePath("captions.harpiaproj");
	TimelineModel textOnly;
	TlClip caption = clip(0);
	caption.type = TlClip::Type::Text;
	TlTrack textTrack;
	textTrack.clips = {caption};
	textOnly.tracks = {textTrack};
	QJsonObject textRoot{{"harpiaProject", 3}, {"editMode", "full"}};
	timelineToProject(textOnly, textRoot);
	write(captions, textRoot);
	check(window.openProjectAt(captions) && window.sources_.empty() && window.timelineView_->model().tracks[0].clips[0].type == TlClip::Type::Text,
	      "a v3 caption-only document without a sources array clears the previous project's media pool");
	TimelineModel markersOnly;
	markersOnly.markers = {250};
	markersOnly.videoEndMs = 1000;
	window.timelineView_->setModel(markersOnly);
	check(window.hasUnsavedEdits(), "markers and a video-end marker alone are protected by the close prompt");
	TimelineModel metadataOnly;
	metadataOnly.tags = {TlTag{1, QStringLiteral("Intro"), QColor(Qt::red)}};
	metadataOnly.soundRules = {rule};
	window.timelineView_->setModel(metadataOnly);
	check(window.hasUnsavedEdits(), "a tag and sound-rule setup alone is protected by the close prompt");
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
