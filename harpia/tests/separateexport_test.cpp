// Real right-click workflow: names and shared quality reach encoded files;
// unrelated layers/music/voiceover stay out and the editor stays untouched.
#include "support/ContextMenu.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/ExportOptionsDialog.hpp"
#include "editor/VoiceoverTrack.hpp"
#include "editor/timeline/TimelineJson.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace harpia;
static int failures;
static void ok(bool pass, const char *why)
{
	std::printf("%s %s\n", pass ? "PASS" : "FAIL", why);
	failures += !pass;
}
static void settle(int ms)
{
	QElapsedTimer timer; timer.start();
	while (timer.elapsed() < ms) { QApplication::processEvents(); QThread::msleep(5); }
}
static QByteArray ffmpeg(const QStringList &args)
{
	QProcess p; p.start(QStringLiteral("ffmpeg"), args); p.waitForFinished(20000);
	return p.readAllStandardOutput();
}
static TlClip clip(int source, qint64 at, qint64 start, qint64 end)
{
	TlClip c; c.sourceId = source; c.outStartMs = at; c.srcStartMs = start; c.srcEndMs = end; return c;
}
static double tonePower(const QByteArray &raw, double hz)
{
	const int start = 4800, count = std::min(8192, int(raw.size() / sizeof(float)) - start);
	if (count <= 0) return 0;
	double re = 0, im = 0;
	for (int i = 0; i < count; ++i) {
		float sample; std::memcpy(&sample, raw.constData() + (start + i) * sizeof(float), sizeof(float));
		const double angle = 2 * 3.141592653589793 * hz * i / 48000.0;
		re += sample * std::cos(angle); im += sample * std::sin(angle);
	}
	return (re * re + im * im) / (count * count);
}
int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QStandardPaths::setTestModeEnabled(true);
	QTemporaryDir work;
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, work.filePath("settings"));
	const QString media = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	const QString linkedPath = work.filePath("linked.wav"), bedPath = work.filePath("background.wav");
	ffmpeg({"-y", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000:duration=4", linkedPath});
	ffmpeg({"-y", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=220:sample_rate=48000:duration=6", bedPath});
	TimelineModel model;
	TlTrack overlay; overlay.clips = {clip(3, 0, 0, 2000)};
	overlay.clips[0].speed = 0.25; // unselected blue covers the selected clips in the editor
	TlTrack pictures;
	TlClip red = clip(1, 1000, 500, 1500); red.speed = 2; red.opacity = 0.5; red.volume = 0; red.link = 7;
	TlClip green = clip(2, 3000, 200, 1700);
	pictures.clips = {red, green}; pictures.locked = true; // export is read-only
	TlTrack audio; audio.kind = TlTrack::Kind::Audio;
	TlClip linked = clip(4, 1000, 100, 600); linked.link = 7;
	audio.clips = {linked};
	TlTrack music; music.kind = TlTrack::Kind::Audio; music.clips = {clip(5, 0, 0, 6000)};
	model.tracks = {overlay, pictures, audio, music};
	model.videoEndMs = 500; // both selected clips are beyond the project boundary
	QJsonArray sources;
	for (int i = 0; i < 3; ++i) sources.append(QJsonObject{{"id", i + 1},
		{"path", media + "/av_" + QStringList{"red", "green", "blue"}[i] + ".mp4"}});
	sources.append(QJsonObject{{"id", 4}, {"path", linkedPath}});
	sources.append(QJsonObject{{"id", 5}, {"path", bedPath}});
	QJsonObject root{{"harpiaProject", 3}, {"editMode", "full"}, {"canvasW", 160}, {"canvasH", 90}, {"fps", 10}, {"sources", sources}};
	timelineToProject(model, root);
	const QString project = work.filePath("library.harpiaproj");
	QFile file(project); file.open(QIODevice::WriteOnly); file.write(QJsonDocument(root).toJson()); file.close();
	VideoEditorWindow editor(QString{}); editor.resize(1400, 1000); editor.show();
	ok(editor.openProjectAt(project), "project opens with selected footage and unrelated layers/audio");
	settle(200);
	auto *view = editor.findChild<TimelineView *>();
	if (!view) return 2;
	view->selectClip(1, 0); view->addToSelection(1, 1); view->zoomToFit();
	auto *voiceovers = editor.findChild<VoiceoverTrack *>();
	VoiceoverClip voice; voice.path = bedPath; voice.durationMs = voice.srcTotalMs = 1000;
	if (voiceovers) voiceovers->setClips({voice});
	const TimelineModel original = view->model();
	const TlSelection selection = view->selectionState();
	QDir(work.path()).mkdir("Database");
	QFile sentinel(work.filePath("Database/keep.txt")); sentinel.open(QIODevice::WriteOnly); sentinel.write("keep"); sentinel.close();
	bool menuFound = false, accepted = false;
	int exported = 0;
	QObject::connect(&editor, &VideoEditorWindow::exported, [&](const QString &) { ++exported; });
	QTimer driver; driver.setInterval(10);
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
			menu = test_menu::openSubmenu(menu, QStringLiteral("Export"));
			for (QAction *action : menu->actions())
				if (action->objectName() == QStringLiteral("exportClipsSeparatelyAction")) {
					menuFound = action->isEnabled() && action->text().contains(QStringLiteral("2 clips"));
					menu->setActiveAction(action);
					QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
					QApplication::sendEvent(menu, &enter);
					return;
				}
			test_menu::dismiss(menu);
		}
		for (QWidget *top : QApplication::topLevelWidgets()) {
			if (auto *dlg = qobject_cast<ExportOptionsDialog *>(top); dlg && dlg->isVisible() && !accepted) {
				auto *table = dlg->findChild<QTableWidget *>(QStringLiteral("separateClipNames"));
				ok(table && table->rowCount() == 2 && !dlg->sparkMode() && !dlg->batchMode(), "right-click opens the dedicated two-clip naming window");
				if (!table) { dlg->reject(); return; }
				dlg->findChild<QLineEdit *>(QStringLiteral("exportBaseName"))->setText("Database");
				table->item(1, 3)->setText("Green hero");
				dlg->findChild<QLineEdit *>(QStringLiteral("exportFolder"))->setText(work.path());
				dlg->findChild<QCheckBox *>(QStringLiteral("separateOpenFolder"))->setChecked(false);
				for (auto *combo : dlg->findChildren<QComboBox *>())
					if (combo->findText(QStringLiteral("MP4 (H.264)")) >= 0)
						combo->setCurrentIndex(combo->findText(QStringLiteral("MP4 (H.264)")));
				auto *res = dlg->findChild<QComboBox *>(QStringLiteral("exportResolution"));
				res->setCurrentIndex(res->findData(-1));
				dlg->findChild<QSpinBox *>(QStringLiteral("exportWidth"))->setValue(128);
				dlg->findChild<QSpinBox *>(QStringLiteral("exportHeight"))->setValue(72);
				auto *fps = dlg->findChild<QComboBox *>(QStringLiteral("exportFps"));
				fps->setCurrentIndex(fps->findData(30.0));
				dlg->findChild<QSlider *>(QStringLiteral("exportQuality"))->setValue(90);
				ok(dlg->videoCrf() < 20 && dlg->outputSize() == QSize(128, 72) && dlg->frameRate() == 30,
				   "batch exposes shared quality, resolution and frame-rate settings");
				accepted = true; dlg->accept();
			} else if (auto *box = qobject_cast<QMessageBox *>(top); box && box->isVisible()) {
				std::printf("Dialog: %s\n", qPrintable(box->text())); box->accept();
			}
		}
	});
	driver.start();
	const QPoint pos = view->clipRectForTest(1, 0).center();
	QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos), view->mapToGlobal(QPointF(pos)), Qt::RightButton, Qt::RightButton, Qt::NoModifier);
	QApplication::sendEvent(view, &press);
	ok(menuFound && accepted, "selected locked clips can use Export clips separately from their context menu");
	QElapsedTimer wait; wait.start();
	while (wait.elapsed() < 60000) { settle(50); if (!editor.findChild<BatchExporter *>()) break; }
	const QDir folder(work.filePath("Database (2)"));
	const QStringList files = folder.entryList({"*.mp4"}, QDir::Files, QDir::Name);
	ok(exported == 2 && files == QStringList{"Database_01.mp4", "Green hero.mp4"}, "numbered and individually edited filenames are written into a new unique folder");
	ok(QFile::exists(work.filePath("Database/keep.txt")), "existing database folders and files are preserved");
	const double durations[] = {0.5, 1.5};
	for (int i = 0; i < files.size() && i < 2; ++i) {
		const QString path = folder.filePath(files[i]);
		QProcess probe;
		probe.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height,r_frame_rate,duration", "-of", "json", path});
		probe.waitForFinished(20000);
		const auto streams = QJsonDocument::fromJson(probe.readAllStandardOutput()).object().value("streams").toArray();
		const QJsonObject video = streams.isEmpty() ? QJsonObject() : streams[0].toObject();
		ok(video.value("width").toInt() == 128 && video.value("height").toInt() == 72 && video.value("r_frame_rate").toString() == "30/1",
		   "encoded clip honors the shared resolution and frame rate");
		ok(std::abs(video.value("duration").toString().toDouble() - durations[i]) < 0.001,
		   "encoded duration follows each clip's trims/speed beyond the project end marker");
		const QByteArray pixel = ffmpeg({"-v", "error", "-ss", "0.25", "-i", path, "-frames:v", "1", "-vf", "scale=1:1", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"});
		const bool redFrame = pixel.size() >= 3 && uchar(pixel[0]) > 50 && uchar(pixel[0]) < 150 && uchar(pixel[2]) < 30;
		const bool greenFrame = pixel.size() >= 3 && uchar(pixel[1]) > 120 && uchar(pixel[0]) < 30 && uchar(pixel[2]) < 30;
		ok(i == 0 ? redFrame : greenFrame, "output contains only the selected clip and preserves its opacity, without the blue overlay");
	}
	if (!files.isEmpty()) {
		const QByteArray pcm = ffmpeg({"-v", "error", "-i", folder.filePath(files[0]), "-vn", "-ac", "1", "-ar", "48000", "-f", "f32le", "-"});
		ok(tonePower(pcm, 880) > 1e-5 && tonePower(pcm, 880) > 10 * tonePower(pcm, 220) && tonePower(pcm, 880) > 10 * tonePower(pcm, 400),
		   "linked audio is included while unrelated background sound and separate voiceover are excluded");
	}
	ok(view->model() == original && view->selectionState() == selection && voiceovers && voiceovers->clips() == QVector<VoiceoverClip>{voice},
	   "export leaves timeline, selection and voiceover unchanged");
	return failures ? 1 : 0;
}
