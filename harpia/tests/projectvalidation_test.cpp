// A rejected project must leave the current edit intact. In particular, an
// older app must not open and resave a newer document while dropping its data.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineJson.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
	std::printf("SKIP: native editor settings cannot be redirected safely on this platform\n");
	return 77;
#else
	QTemporaryDir dir;
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
	QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path());
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaTest"));
	QCoreApplication::setApplicationName(QStringLiteral("projectvalidation"));
	int failures = 0;
	const auto check = [&](bool okay, const char *why) {
		std::printf("%s %s\n", okay ? "PASS" : "FAIL", why);
		failures += !okay;
	};
	const QString path = dir.filePath(QStringLiteral("project.harpiaproj"));
	const auto write = [&](const QByteArray &bytes) {
		QFile file(path);
		return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
	};
	TlClip caption;
	caption.type = TlClip::Type::Text;
	caption.srcEndMs = 2000;
	caption.text.text = QStringLiteral("Keep this edit");
	TlTrack lane;
	lane.clips = {caption};
	TimelineModel model;
	model.tracks = {lane};
	model.markers = {500};
	QJsonObject valid{{QStringLiteral("harpiaProject"), 3},
			  {QStringLiteral("sources"), QJsonArray()},
			  {QStringLiteral("editMode"), QStringLiteral("full")},
			  {QStringLiteral("canvasW"), 640}, {QStringLiteral("canvasH"), 360},
			  {QStringLiteral("fps"), 60.0}};
	timelineToProject(model, valid);
	check(write(QJsonDocument(valid).toJson()), "prepared a valid caption project");
	VideoEditorWindow window{QString()};
	check(window.openProjectAt(path), "the current project format opens");
	TimelineView *timeline = window.findChild<TimelineView *>();
	if (!timeline)
		return 1;
	const TimelineModel before = timeline->model();
	const auto preserved = [&]() {
		return timeline->model() == before && window.timelineCanvasSize() == QSize(640, 360) &&
		       window.timelineFps() == 60.0;
	};
	for (const QJsonValue version : {QJsonValue(0), QJsonValue(-1), QJsonValue(2.5),
					QJsonValue(QStringLiteral("3")), QJsonValue(QJsonValue::Null),
					QJsonValue(4), QJsonValue(1000000000)}) {
		QJsonObject invalid = valid;
		invalid[QStringLiteral("harpiaProject")] = version;
		check(write(QJsonDocument(invalid).toJson()) && !window.openProjectAt(path) && preserved(),
		      "invalid or newer project version is rejected without changing the current edit");
	}
	for (const QByteArray &bytes : {QByteArray("{broken"), QByteArray("[]"), QByteArray("{}")})
		check(write(bytes) && !window.openProjectAt(path) && preserved(),
		      "malformed or unrelated JSON leaves the current edit intact");
	for (const char *section : {"sources", "tracks", "segments", "voiceovers", "markers", "tags", "soundRules"}) {
		QJsonObject invalid = valid;
		invalid[QLatin1String(section)] = QStringLiteral("invalid array");
		check(write(QJsonDocument(invalid).toJson()) && !window.openProjectAt(path) && preserved(),
		      "an invalid document section is rejected before any active project data is replaced");
	}
	check(!window.openProjectAt(dir.filePath(QStringLiteral("missing.harpiaproj"))) && preserved(),
	      "a missing project leaves the current edit intact");
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
#endif
}
