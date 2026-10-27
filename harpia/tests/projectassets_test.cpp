// The saved project and its narration are one unit: repeated saves, reordered
// legacy takes, or a failed save must never remove/replace another take's input.
#include <QtWidgets>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
#define private public
#include "editor/VideoEditorWindow.hpp"
#undef private
#include "editor/VoiceoverTrack.hpp"

#include <cmath>
#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool value, const char *description)
{
	std::printf("%s %s\n", value ? "PASS" : "FAIL", description);
	failures += !value;
}
QByteArray read(const QString &path)
{
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool write(const QString &path, const QByteArray &bytes)
{
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QMap<QString, QByteArray> contents(const QString &directory)
{
	QMap<QString, QByteArray> result;
	for (const QFileInfo &file : QDir(directory).entryInfoList(QDir::Files | QDir::Hidden))
		result.insert(file.fileName(), read(file.absoluteFilePath()));
	return result;
}
bool tone(const QString &path, double frequency)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	QDataStream stream(&file);
	stream.setByteOrder(QDataStream::LittleEndian);
	stream.writeRawData("RIFF", 4);
	stream << quint32(36 + 48000 * 2);
	stream.writeRawData("WAVEfmt ", 8);
	stream << quint32(16) << quint16(1) << quint16(1) << quint32(48000) << quint32(96000) << quint16(2) << quint16(16);
	stream.writeRawData("data", 4);
	stream << quint32(48000 * 2);
	for (int i = 0; i < 48000; ++i)
		stream << qint16(std::sin(i * 2.0 * 3.141592653589793 * frequency / 48000.0) * 10000.0);
	return stream.status() == QDataStream::Ok;
}
VoiceoverClip take(const QString &path, int position)
{
	VoiceoverClip clip;
	clip.path = path;
	clip.outStartMs = position;
	clip.durationMs = clip.srcTotalMs = 1000;
	return clip;
}
QString assetsDirectory(const QString &project)
{
	const QFileInfo file(project);
	return QDir(file.absolutePath()).filePath(file.completeBaseName() + QStringLiteral("_assets"));
}
}

int main(int argc, char **argv)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
	// setPath cannot redirect the registry/CFPreferences used by explicit
	// native settings in the real editor. This optional UI fixture is Linux-only.
	std::fprintf(stderr, "This fixture requires a file-based native QSettings store.\n");
	return 77;
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir temporary;
	if (!temporary.isValid())
		return 2;
	qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
	qputenv("XDG_CACHE_HOME", temporary.filePath("cache").toUtf8());
	QApplication app(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaProjectAssetsTest"));
	QCoreApplication::setApplicationName(QStringLiteral("ProjectAssets"));
	QStandardPaths::setTestModeEnabled(true);
	QSettings::setDefaultFormat(QSettings::IniFormat);
	for (QSettings::Format format : {QSettings::IniFormat, QSettings::NativeFormat}) {
		QSettings::setPath(format, QSettings::UserScope, temporary.path());
		QSettings::setPath(format, QSettings::SystemScope, temporary.filePath("system"));
	}
	const QString a = temporary.filePath("take-a.wav"), b = temporary.filePath("take-b.wav"),
		      c = temporary.filePath("take-c.wav");
	check(tone(a, 220) && tone(b, 660) && tone(c, 330), "distinct valid narration WAVs are created");
	const QByteArray bytesA = read(a), bytesB = read(b);
	for (bool legacy : {false, true}) {
		const QString project = temporary.filePath(legacy ? "legacy.harpiaproj" : "repeated.harpiaproj");
		{
			VideoEditorWindow original{QString()};
			original.voTrack_->setClips({take(a, 0), take(b, 1000), take(a, 2000)});
			check(original.saveProjectTo(project, true).isEmpty(), "the initial project saves successfully");
		}
		const QString assets = assetsDirectory(project);
		check(contents(assets).size() == 2, "split clips sharing the same take reuse one immutable asset");
		if (legacy) {
			QJsonObject root = QJsonDocument::fromJson(read(project)).object();
			QJsonArray voices = root.value(QStringLiteral("voiceovers")).toArray();
			for (int i = 0; i < voices.size(); ++i) {
				const QString name = QStringLiteral("vo_%1.wav").arg(i, 3, 10, QLatin1Char('0'));
				check(write(QDir(assets).filePath(name), i == 1 ? bytesB : bytesA), "a numbered legacy narration asset is prepared");
				QJsonObject voice = voices[i].toObject();
				voice[QStringLiteral("file")] = QFileInfo(assets).fileName() + QLatin1Char('/') + name;
				voices[i] = voice;
			}
			root[QStringLiteral("voiceovers")] = voices;
			check(write(project, QJsonDocument(root).toJson()), "a legacy numbered-assets project is prepared");
		}
		VideoEditorWindow loaded{QString()};
		check(loaded.openProjectAt(project), "the editor opens the saved narration project");
		QVector<VoiceoverClip> clips = loaded.voTrack_->clips();
		check(clips.size() == 3, "all saved narration clips load");
		if (clips.size() != 3)
			continue;
		check(read(clips[0].path) == bytesA && read(clips[1].path) == bytesB, "both original takes load intact");
		const auto existingAssets = contents(assets);
		if (legacy) {
			std::swap(clips[0], clips[1]);
			loaded.voTrack_->setClips(clips);
		}
		check(loaded.saveProjectTo(project, true).isEmpty() && loaded.saveProjectTo(project, true).isEmpty(),
		      "repeated same-path saves succeed");
		check(contents(assets) == existingAssets, "reordering/resaving never replaces existing asset inputs or adds duplicate assets");
		VideoEditorWindow reopened{QString()};
		check(reopened.openProjectAt(project), "the resaved project reopens");
		const auto again = reopened.voTrack_->clips();
		check(again.size() == 3 && read(again[0].path) == (legacy ? bytesB : bytesA) &&
		      read(again[1].path) == (legacy ? bytesA : bytesB), "each clip retains its intended audio after Save and Open");
		const QString sidecar = temporary.filePath(legacy ? "legacy_autosave.harpiaproj" : "repeated_autosave.harpiaproj");
		check(loaded.saveProjectTo(sidecar, true).isEmpty(), "an autosave sidecar saves its own portable assets");
		const auto autosaveAssets = contents(assetsDirectory(sidecar));
		check(loaded.saveProjectTo(sidecar, true).isEmpty() && contents(assetsDirectory(sidecar)) == autosaveAssets,
		      "repeated autosaves reuse the same assets");
		// A first new take can be copied before a later missing take fails. The
		// whole save must roll back rather than damage the last good project.
		loaded.voTrack_->setClips({take(c, 0), take(temporary.filePath("missing.wav"), 1000)});
		const QByteArray previousProject = read(project);
		const QDateTime previousAutosave = loaded.lastAutosave_;
		check(!loaded.saveProjectTo(project, true).isEmpty(), "quiet saves report missing narration instead of claiming success");
		check(read(project) == previousProject && contents(assets) == existingAssets,
		      "a failed asset save preserves the old project and rolls back newly copied assets");
		check(loaded.lastAutosave_ == previousAutosave && loaded.projectPath_ == project,
		      "a failed quiet save leaves the autosave timestamp and adopted project unchanged");
		const QString rejected = temporary.filePath("failed-save-as.harpiaproj");
		check(!loaded.saveProjectTo(rejected, false).isEmpty() && loaded.projectPath_ == project &&
		      !QFileInfo::exists(rejected) && !QFileInfo::exists(assetsDirectory(rejected)),
		      "failed Save As adopts no path and leaves no partial project or assets");
	}
	{
		const QString project = temporary.filePath("full-editing-narration.harpiaproj");
		QString session;
		QByteArray recordedBytes;
		{
			VideoEditorWindow recording{QString()};
			session = recording.voiceoverTempDir();
			check(!session.isEmpty(), "the editor creates its narration session directory");
			if (session.isEmpty())
				return 1;
			const QString original = QDir(session).filePath("recorded-take.wav");
			check(tone(original, 440), "a microphone take is prepared in the real editor session folder");
			recordedBytes = read(original);
			const int id = recording.addAudioSource(original); // finishVoiceover's Full-editing path
			check(id >= 0, "the microphone take becomes a Full-editing media source");
			TlClip clip;
			clip.sourceId = id;
			clip.srcEndMs = 1000;
			TlTrack track;
			track.kind = TlTrack::Kind::Audio;
			track.clips = {clip};
			TimelineModel model;
			model.tracks = {track};
			recording.timelineView_->setModel(model);
			recording.timelineSeeded_ = true;
			recording.setEditMode(VideoEditorWindow::EditMode::Full);
			check(recording.voTrack_->isEmpty() && recording.saveProjectTo(project, true).isEmpty(),
			      "Full-editing narration saves even though it is outside VoiceoverTrack");
			const QJsonArray savedSources = QJsonDocument::fromJson(read(project)).object()
				.value(QStringLiteral("sources")).toArray();
			const QString savedPath = savedSources.isEmpty() ? QString()
				: savedSources.first().toObject().value(QStringLiteral("path")).toString();
			check(!savedPath.isEmpty() && QDir::isRelativePath(savedPath) &&
			      read(QDir(temporary.path()).filePath(savedPath)) == recordedBytes,
			      "the recorded source points at a portable project-owned copy");
			const QByteArray goodProject = read(project);
			check(QFile::remove(original), "the original session take is removed to exercise save failure");
			check(!recording.saveProjectTo(project, true).isEmpty() && read(project) == goodProject,
			      "a missing session-owned source fails the save without overwriting the last good project");
		}
		check(!QFileInfo::exists(session), "closing the recording editor deletes its session directory");
		const QString copy = temporary.filePath("narration-copy.harpiaproj");
		{
			VideoEditorWindow reopened{QString()};
			check(reopened.openProjectAt(project), "the narration project reopens after session cleanup");
			const TimelineModel &model = reopened.timelineView_->model();
			const EditorSource *source = model.tracks.isEmpty() || model.tracks[0].clips.isEmpty()
				? nullptr : reopened.sourceById(model.tracks[0].clips[0].sourceId);
			check(source && read(source->origPath) == recordedBytes && source->durationMs > 0,
			      "the Full-editing audio source still contains the original recorded take");
			check(reopened.saveProjectTo(project, true).isEmpty(), "a reopened recorded source resaves successfully");
			const QJsonArray savedSources = QJsonDocument::fromJson(read(project)).object()
				.value(QStringLiteral("sources")).toArray();
			check(!savedSources.isEmpty() && QDir::isRelativePath(savedSources.first().toObject()
				.value(QStringLiteral("path")).toString()), "resaving keeps recorded source references project-relative");
			QTimer dismiss;
			dismiss.setInterval(10);
			QObject::connect(&dismiss, &QTimer::timeout, []() {
				if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
					box->accept();
			});
			dismiss.start();
			const QString error = reopened.saveProjectTo(copy, false);
			dismiss.stop();
			check(error.isEmpty() && reopened.projectPath_ == copy, "Save As copies the reopened narration into a new project");
			check(reopened.saveProjectTo(copy, true).isEmpty(), "the editor can save again after Save As");
			const QJsonArray copiedSources = QJsonDocument::fromJson(read(copy)).object()
				.value(QStringLiteral("sources")).toArray();
			check(!copiedSources.isEmpty() && QDir::isRelativePath(copiedSources.first().toObject()
				.value(QStringLiteral("path")).toString()), "saving again after Save As keeps the new asset reference portable");
		}
		check(QDir(assetsDirectory(project)).removeRecursively() && QFile::remove(project),
		      "the old narration project and assets are removed after closing its editor");
		VideoEditorWindow copied{QString()};
		check(copied.openProjectAt(copy), "the saved-as narration project opens without the old assets");
		const TimelineModel &model = copied.timelineView_->model();
		const EditorSource *source = model.tracks.isEmpty() || model.tracks[0].clips.isEmpty()
			? nullptr : copied.sourceById(model.tracks[0].clips[0].sourceId);
		check(source && read(source->origPath) == recordedBytes,
		      "Save As preserves recorded audio independently of the previous project");
	}
	{
		auto first = std::make_unique<VideoEditorWindow>(QString());
		auto second = std::make_unique<VideoEditorWindow>(QString());
		const QString firstSession = first->voiceoverTempDir(), secondSession = second->voiceoverTempDir();
		check(!firstSession.isEmpty() && !secondSession.isEmpty() && firstSession != secondSession,
		      "two editor windows receive distinct narration session directories");
		if (secondSession.isEmpty())
			return 1;
		const QString otherTake = QDir(secondSession).filePath("other-window.wav");
		check(tone(otherTake, 880), "a take is created in the second editor session");
		const QByteArray otherBytes = read(otherTake);
		first.reset();
		check(read(otherTake) == otherBytes && QFileInfo::exists(otherTake),
		      "closing one editor preserves another editor's recorded audio");
	}
	{
		VideoEditorWindow editor{QString()};
		check(editor.addAudioSource(a) >= 0, "an ordinary external audio file is imported");
		const QString project = temporary.filePath("external-audio.harpiaproj");
		check(editor.saveProjectTo(project, true).isEmpty(), "the external audio project saves");
		const QJsonArray savedSources = QJsonDocument::fromJson(read(project)).object()
			.value(QStringLiteral("sources")).toArray();
		check(savedSources.size() == 1 && savedSources.first().toObject().value(QStringLiteral("path")).toString() ==
		      QDir::toNativeSeparators(a) && !QFileInfo::exists(assetsDirectory(project)),
		      "external audio keeps its original reference without copying its temporary decode");
	}
	{
		const QString project = temporary.filePath("image-owner.harpiaproj");
		QDir().mkpath(assetsDirectory(project));
		const QString imagePath = QDir(assetsDirectory(project)).filePath("user-imported.png");
		QImage image(16, 16, QImage::Format_RGB32);
		image.fill(Qt::blue);
		check(image.save(imagePath), "a user-imported PNG is placed in a project's assets directory");
		{
			VideoEditorWindow editor{QString()};
			check(editor.addImageSource(imagePath) >= 0 && editor.saveProjectTo(project, true).isEmpty(),
			      "the project references the imported image");
		}
		VideoEditorWindow reopened{QString()};
		check(reopened.openProjectAt(project), "the image project reopens");
		const QString copy = temporary.filePath("image-copy.harpiaproj");
		check(reopened.saveProjectTo(copy, true).isEmpty(), "the image project saves to another path");
		const QJsonArray sources = QJsonDocument::fromJson(read(copy)).object().value(QStringLiteral("sources")).toArray();
		check(sources.size() == 1 && sources.first().toObject().value(QStringLiteral("path")).toString() ==
		      QDir::toNativeSeparators(imagePath) && !QFileInfo::exists(assetsDirectory(copy)),
		      "an imported PNG inside assets stays an external image reference rather than becoming WAV audio");
		VideoEditorWindow copied{QString()};
		check(copied.openProjectAt(copy) && !copied.stillImages_.isEmpty(), "the copied project still loads the PNG as an image");
	}
	{
		VideoEditorWindow editor{QString()};
		editor.voTrack_->setClips({take(c, 0)});
		const QString blocked = temporary.filePath("blocked.harpiaproj");
		QDir().mkpath(blocked); // a directory cannot be replaced by the JSON file
		const QString assets = assetsDirectory(blocked);
		QDir().mkpath(assets);
		write(QDir(assets).filePath("existing.wav"), bytesA);
		const auto existingAssets = contents(assets);
		check(!editor.saveProjectTo(blocked, true).isEmpty() && contents(assets) == existingAssets,
		      "a JSON write failure rolls back new assets and preserves existing files");
		check(editor.projectPath_.isEmpty() && !editor.projectCreated_.isValid() && !editor.lastAutosave_.isValid(),
		      "a failed first save adopts no project metadata or autosave timestamp");
	}
	{
		VideoEditorWindow editor{QString()};
		editor.voTrack_->setClips({take(a, 0)});
		const QString project = temporary.filePath("verify.harpiaproj");
		check(editor.saveProjectTo(project, true).isEmpty(), "the asset verification project saves");
		const auto before = contents(assetsDirectory(project));
		if (before.isEmpty()) {
			check(false, "the verification fixture contains a saved asset");
			return 1;
		}
		const QString asset = QDir(assetsDirectory(project)).filePath(before.firstKey());
		check(write(asset, "corrupted asset"), "a corrupted existing asset is simulated");
		const QByteArray previousProject = read(project);
		check(!editor.saveProjectTo(project, true).isEmpty() && read(project) == previousProject && read(asset) == "corrupted asset",
		      "an existing content-hash asset is verified and never silently overwritten");
	}
	std::printf("%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
