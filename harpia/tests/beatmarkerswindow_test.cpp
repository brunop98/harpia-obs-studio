// Musical pulses through the real editor: analysis is a review transaction,
// source trims/rates map into timeline markers, and the whole batch is undoable.
#include "editor/BeatDetectionDialog.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineJson.hpp"
#include "editor/timeline/TimelineView.hpp"
#include "editor/tts/TtsAudio.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

using namespace harpia;

namespace {
int failures = 0;
void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}

bool writePulses(const QString &path)
{
	tts_audio::Pcm audio;
	audio.rate = 22050;
	audio.channels = 1;
	audio.samples.fill(0, audio.rate * 14);
	unsigned random = 0x12345678;
	constexpr double pi = 3.14159265358979323846;
	for (int atMs = 250; atMs < 14000; atMs += 500) {
		const int start = atMs * audio.rate / 1000;
		for (int index = 0; index < audio.rate * 80 / 1000 && start + index < audio.samples.size(); ++index) {
			random = random * 1664525u + 1013904223u;
			const double noise = double((random >> 8) & 0xffff) / 32767.5 - 1.0;
			const double seconds = double(index) / audio.rate;
			const double attack = std::min(1.0, seconds * 1000.0);
			const double kick = 0.75 * std::sin(2 * pi * 110 * seconds) * std::exp(-seconds * 45);
			const double snap = 0.3 * noise * std::exp(-seconds * 140);
			audio.samples[start + index] = qint16(std::lround(std::clamp(attack * (kick + snap), -1.0, 1.0) * 30000));
		}
	}
	return tts_audio::writeWav16(path, audio);
}

TimelineModel fixture()
{
	TlClip picture;
	picture.type = TlClip::Type::Image;
	picture.sourceId = 1;
	picture.srcEndMs = 8000;
	TlTrack video;
	video.kind = TlTrack::Kind::Video;
	video.clips = {picture};
	TlClip music;
	music.type = TlClip::Type::Video; // Sound clips share the media type.
	music.sourceId = 2;
	music.srcStartMs = 1000;
	music.srcEndMs = 11000;
	music.outStartMs = 3000;
	music.speed = 2.0;
	music.fadeInMs = music.fadeOutMs = 0;
	TlTrack audio;
	audio.kind = TlTrack::Kind::Audio;
	audio.clips = {music};
	TimelineModel model;
	model.tracks = {video, audio};
	model.markers = {250, 3375};
	model.markerLabels.insert(250, QStringLiteral("Introduction"));
	model.markerLabels.insert(3375, QStringLiteral("Keep my drop label"));
	model.videoEndMs = 6500;
	return model;
}

bool writeProject(const QString &path, const QString &image, const QString &audio)
{
	QJsonObject root;
	root[QStringLiteral("harpiaProject")] = 3;
	root[QStringLiteral("editMode")] = QStringLiteral("full");
	root[QStringLiteral("canvasW")] = 64;
	root[QStringLiteral("canvasH")] = 64;
	root[QStringLiteral("fps")] = 30;
	QJsonArray sources;
	int id = 1;
	for (const QString &file : {image, audio})
		sources.append(QJsonObject{{QStringLiteral("id"), id++}, {QStringLiteral("path"), file}});
	root[QStringLiteral("sources")] = sources;
	timelineToProject(fixture(), root);
	QFile file(path);
	const QByteArray bytes = QJsonDocument(root).toJson();
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

enum class Response { Add, Cancel, WaitForError };
struct Review {
	bool seen = false;
	bool timedOut = false;
	bool sawError = false;
	QVector<qint64> times;
	QString status;
};

Review review(VideoEditorWindow &editor, TimelineView &timeline, const std::function<void()> &open,
	      Response response = Response::Add, int density = 0)
{
	Review result;
	const TimelineModel before = timeline.model();
	QTimer driver, watchdog;
	driver.setInterval(5);
	watchdog.setSingleShot(true);
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		// Error notifications must not strand an automated review. An early
		// message without our analysis dialog still fails the seen assertion.
		if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
			message->accept();
			return;
		}
		auto *dialog = editor.findChild<BeatDetectionDialog *>(QStringLiteral("beatDetectionDialog"));
		if (!dialog || !dialog->isVisible())
			return;
		result.seen = true;
		if (auto *status = dialog->findChild<QLabel *>(QStringLiteral("beatStatus")))
			result.status = status->text();
		auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("beatCancel"));
		if (response == Response::Cancel) {
			if (cancel)
				cancel->click();
			return;
		}
		auto *add = dialog->findChild<QPushButton *>(QStringLiteral("beatAddMarkers"));
		if (response == Response::WaitForError) {
			auto *progress = dialog->findChild<QProgressBar *>(QStringLiteral("beatProgress"));
			auto *status = dialog->findChild<QLabel *>(QStringLiteral("beatStatus"));
			if (add && !add->isEnabled() && progress && !progress->isVisible() &&
			    status && !status->text().isEmpty()) {
				result.sawError = true;
				if (cancel)
					cancel->click();
			}
			return;
		}
		if (!add || !add->isEnabled())
			return;
		if (auto *choice = dialog->findChild<QComboBox *>(QStringLiteral("beatDensity")))
			choice->setCurrentIndex(density);
		result.times = dialog->selectedBeatTimes();
		check(timeline.model() == before, "reviewing detected beats leaves the project untouched until Add markers");
		add->click();
	});
	QObject::connect(&watchdog, &QTimer::timeout, [&]() {
		result.timedOut = true;
		if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
			dialog->reject();
	});
	driver.start();
	watchdog.start(12000);
	open();
	driver.stop();
	watchdog.stop();
	return result;
}

bool invoke(VideoEditorWindow &editor, const char *method)
{
	return QMetaObject::invokeMethod(&editor, method, Qt::DirectConnection);
}

bool near(qint64 first, qint64 second, qint64 tolerance = 50)
{
	return std::abs(first - second) <= tolerance;
}
} // namespace

int main(int argc, char **argv)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
	// The editor uses explicit native settings. XDG_CONFIG_HOME and setPath
	// cannot isolate the registry/CFPreferences used by an installed app.
	std::fprintf(stderr, "This editor fixture requires a file-based native QSettings store.\n");
	return 77;
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir scratch;
	if (!scratch.isValid())
		return 2;
	qputenv("XDG_CONFIG_HOME", scratch.filePath(QStringLiteral("config")).toUtf8());
	qputenv("XDG_CACHE_HOME", scratch.filePath(QStringLiteral("cache")).toUtf8());
	qputenv("XDG_DATA_HOME", scratch.filePath(QStringLiteral("data")).toUtf8());
	QApplication app(argc, argv);
	QStandardPaths::setTestModeEnabled(true);
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.path());
	QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, scratch.path());
#ifndef Q_OS_MACOS
	QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, scratch.path());
	QSettings::setPath(QSettings::NativeFormat, QSettings::SystemScope, scratch.path());
#endif
	const QString picture = scratch.filePath(QStringLiteral("cover.png"));
	const QString music = scratch.filePath(QStringLiteral("music.wav"));
	const QString project = scratch.filePath(QStringLiteral("beats.harpiaproj"));
	QImage image(64, 64, QImage::Format_RGB32);
	image.fill(Qt::darkBlue);
	check(image.save(picture) && writePulses(music) && writeProject(project, picture, music),
	      "created a local project with known musical pulses and an offset, trimmed, sped-up clip");
	VideoEditorWindow editor{QString()};
	check(editor.openProjectAt(project), "the editor opens the self-contained music project");
	auto *timeline = editor.findChild<TimelineView *>();
	if (!timeline)
		return 1;
	const TimelineModel before = timeline->model();
	timeline->selectClip(1, 0);
	const auto open = [&]() { emit timeline->beatsRequested(1, 0); };
	const Review added = review(editor, *timeline, open);
	check(added.seen && !added.timedOut && added.times.size() >= 10,
	      "the audio clip action analyzes real audio and offers a useful set of beat markers");
	if (!added.seen || added.timedOut || added.times.isEmpty())
		return 1;
	bool aligned = true;
	for (qint64 time : added.times) {
		const qint64 pulse = 3125 + qint64(std::llround(double(time - 3125) / 250.0)) * 250;
		aligned = aligned && time >= 3000 && time < 8000 && near(time, pulse);
	}
	check(aligned && near(added.times.first(), 3125) && near(added.times.last(), 7875),
	      "beat positions follow source trim, output offset and doubled playback speed");
	const TimelineModel after = timeline->model();
	check(after.markers.size() > before.markers.size() && after.tracks == before.tracks &&
	      after.videoEndMs == before.videoEndMs && after.markerLabels.value(250) == QStringLiteral("Introduction") &&
	      after.markerLabels.value(3375) == QStringLiteral("Keep my drop label"),
	      "adding beats preserves clips, manual marker names and the explicit video end");
	bool collisionFree = true;
	for (qint64 marker : after.markers)
		if (marker != 3375 && near(marker, 3375, 20))
			collisionFree = false;
	check(collisionFree && after.markers.last() > after.videoEndMs,
	      "existing markers are not duplicated and beats past the output end remain editable");
	check(invoke(editor, "undo") && timeline->model() == before, "one Undo removes the complete beat-marker batch");
	check(invoke(editor, "redo") && timeline->model() == after, "Redo restores the same beat markers and names");
	const QString saved = scratch.filePath(QStringLiteral("saved.harpiaproj"));
	check(editor.saveProjectTo(saved, true).isEmpty(), "the project with generated markers saves normally");
	VideoEditorWindow reopened{QString()};
	check(reopened.openProjectAt(saved), "the saved beat-marker project reopens");
	auto *loaded = reopened.findChild<TimelineView *>();
	check(loaded && loaded->model().markers == after.markers && loaded->model().markerLabels == after.markerLabels &&
	      loaded->model().videoEndMs == after.videoEndMs, "saved projects retain generated beat markers and the existing end");

	const Review again = review(editor, *timeline, open);
	check(again.seen && !again.timedOut && timeline->model() == after,
	      "analyzing the same clip twice does not add duplicate markers or overwrite labels");
	const Review canceled = review(editor, *timeline, open, Response::Cancel);
	check(canceled.seen && !canceled.timedOut && timeline->model() == after,
	      "Cancel leaves the project unchanged during or after analysis");

	timeline->setModelAndCommit(before);
	timeline->selectClip(1, 0);
	auto *toolsAction = editor.findChild<QAction *>(QStringLiteral("detectBeatsAction"));
	check(toolsAction && toolsAction->isEnabled(), "Tools exposes Detect beats for the selected audio clip");
	const Review sparse = review(editor, *timeline, [&]() { if (toolsAction) toolsAction->trigger(); }, Response::Add, 1);
	check(sparse.seen && !sparse.timedOut && sparse.times.size() > 0 && sparse.times.size() < added.times.size(),
	      "the Tools action can add every second beat for a less crowded marker lane");
	check(invoke(editor, "undo") && timeline->model() == before,
	      "a sparse beat batch is also one reversible timeline change");

	// Saving a portable project may copy sources into its own media folder.
	// Remove the path the editor currently resolves, rather than an old input
	// path which could remain unused after save/relink.
	const QString activeMusic = editor.sourcePathForTest(before.tracks[1].clips[0].sourceId);
	const QString unavailable = activeMusic + QStringLiteral(".temporarily-unavailable");
	const bool renamed = !activeMusic.isEmpty() && QFile::rename(activeMusic, unavailable);
	check(renamed, "the active source is made unavailable after project loading and saving");
	const Review failed = renamed ? review(editor, *timeline, open, Response::WaitForError) : Review{};
	const bool unchanged = timeline->model() == before;
	const bool failedSafely = failed.seen && !failed.timedOut && failed.sawError && unchanged;
	if (!failedSafely)
		std::fprintf(stderr, "Missing-source review: seen=%d timedOut=%d sawError=%d unchanged=%d status=%s source=%s\n",
			int(failed.seen), int(failed.timedOut), int(failed.sawError), int(unchanged),
			failed.status.toUtf8().constData(), activeMusic.toUtf8().constData());
	check(failedSafely,
	      "a missing audio source reports analysis failure and never changes markers");
	if (renamed)
		QFile::rename(unavailable, activeMusic);
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
