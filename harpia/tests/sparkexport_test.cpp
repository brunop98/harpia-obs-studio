// Drive the real editor's Export dialog, then inspect the encoded MP4 frames.
#include "editor/VideoEditorWindow.hpp"
#include "editor/ExportOptionsDialog.hpp"
#include "editor/timeline/TimelineJson.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>

using namespace harpia;
static int failures;
static void ok(bool pass, const char *why) { std::printf("%s %s\n", pass ? "PASS" : "FAIL", why); failures += !pass; }
static void settle(int ms)
{
	QElapsedTimer timer; timer.start();
	while (timer.elapsed() < ms) { QApplication::processEvents(); QThread::msleep(5); }
}
static QShortcut *shortcut(VideoEditorWindow &w, QKeySequence key)
{
	for (auto *s : w.findChildren<QShortcut *>()) if (s->key() == key) return s;
	return nullptr;
}
static char colorAt(const QString &file, int ms)
{
	QProcess p;
	p.start(QStringLiteral("ffmpeg"), {"-v", "error", "-ss", QString::number(ms / 1000.0), "-i", file,
		"-frames:v", "1", "-vf", "crop=32:32:0:29,scale=1:1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"});
	p.waitForFinished(20000);
	const QByteArray raw = p.readAllStandardOutput();
	if (raw.size() < 3) return '?';
	const auto r = uchar(raw[0]), g = uchar(raw[1]), b = uchar(raw[2]);
	return b > r + 30 && b > g + 30 ? 'B' : g > r + 30 && g > b + 30 ? 'G' : r > g + 30 && r > b + 30 ? 'R' : '?';
}
int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QTemporaryDir work;
	const QString media = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	TimelineModel m; TlTrack track; track.kind = TlTrack::Kind::Video;
	QJsonArray sources;
	for (int i = 0; i < 3; ++i) {
		const QString name = QStringList{"red", "green", "blue"}[i];
		sources.append(QJsonObject{{"id", i + 1}, {"path", media + "/av_" + name + ".mp4"}});
		TlClip c; c.sourceId = i + 1; c.srcEndMs = 1000; c.outStartMs = i * 2000;
		track.clips.append(c);
	}
	// Two Random Text clips make six text combinations in Variations. Spark
	// still writes just its two footage orders, with one sampled text per slot.
	for (int i = 0; i < 2; ++i) {
		TlClip caption;
		caption.type = TlClip::Type::Text;
		caption.srcEndMs = 6000;
		caption.text.random = true;
		caption.text.text = QStringLiteral("Caption %1 A").arg(i);
		caption.text.variations = {QStringLiteral("Caption %1 B").arg(i)};
		if (i == 0) caption.text.variations.append(QStringLiteral("Caption 0 C"));
		caption.text.fontPx = 48;
		caption.posY = i == 0 ? 0.1 : 0.9;
		TlTrack captions;
		captions.clips = {caption};
		m.tracks.append(captions);
	}
	m.tracks.append(track); // captions above footage; no end marker in this pass
	constexpr int footageTrack = 2;
	QJsonObject root{{"harpiaProject", 3}, {"editMode", "full"}, {"canvasW", 160}, {"canvasH", 90}, {"fps", 10}, {"sources", sources}};
	timelineToProject(m, root);
	const QString project = work.filePath("spark.harpia");
	QFile f(project); f.open(QIODevice::WriteOnly); f.write(QJsonDocument(root).toJson()); f.close();
	VideoEditorWindow w(QString{});
	w.resize(1400, 900); w.show();
	ok(w.openProjectAt(project), "loaded red, green and blue footage in real editor");
	settle(300);
	auto *view = w.findChild<TimelineView *>();
	if (!view) return 2;
	view->toggleFavorite(footageTrack, 2); settle(100);
	ok(view->model().tracks[footageTrack].clips[2].favorite && !view->model().tracks[footageTrack].clips[0].favorite, "star belongs only to edited blue clip");
	auto *undo = shortcut(w, QKeySequence::Undo); auto *redo = shortcut(w, QKeySequence(Qt::CTRL | Qt::Key_Y));
	if (!undo || !redo) return 2;
	emit undo->activated(); settle(100);
	ok(!view->model().tracks[footageTrack].clips[2].favorite, "star toggle is one undoable edit");
	emit redo->activated(); settle(100);
	ok(view->model().tracks[footageTrack].clips[2].favorite, "star Redo restores favorite");
	view->selectClip(footageTrack, 0); // offer a one-second Selection scope
	const TimelineModel original = view->model();
	bool accepted = false;
	QString prefix = QStringLiteral("my_video_spark");
	bool markerExport = false;
	QTimer dialogs;
	dialogs.setInterval(20);
	QObject::connect(&dialogs, &QTimer::timeout, [&]() {
		for (QWidget *top : QApplication::topLevelWidgets()) {
			if (auto *dlg = qobject_cast<ExportOptionsDialog *>(top); dlg && dlg->isVisible() && !accepted) {
				dlg->setSparkTabForTest(true);
				dlg->findChild<QSpinBox *>(QStringLiteral("sparkRequested"))->setValue(100);
				dlg->findChild<QLineEdit *>(QStringLiteral("sparkPrefix"))->setText(prefix);
				dlg->findChild<QLineEdit *>(QStringLiteral("exportFolder"))->setText(work.path());
				dlg->findChild<QCheckBox *>(QStringLiteral("sparkOpenFolder"))->setChecked(false);
				for (auto *combo : dlg->findChildren<QComboBox *>())
					if (combo->findText(QStringLiteral("MP4 (H.264)")) >= 0)
						combo->setCurrentIndex(combo->findText(QStringLiteral("MP4 (H.264)")));
				ok(dlg->sparkCount() == 2 && !dlg->batchMode() && !dlg->exportRange(), "requesting 100 chooses only two unique Spark exports");
				if (markerExport) {
					ok(std::abs(dlg->exportSeconds() - 3.0) < 0.001,
					   "Spark estimate uses two exports of the 1.5-second marked range");
					ok(dlg->findChild<QLabel *>(QStringLiteral("sparkStats"))->text().startsWith(
					   QStringLiteral("2 footage blocks · 0 favorites")),
					   "dialog excludes starred footage beyond the end marker");
				}
				accepted = true; dlg->accept();
			} else if (auto *box = qobject_cast<QMessageBox *>(top); box && box->isVisible()) {
				std::printf("Dialog: %s\n", qPrintable(box->text())); box->accept();
			}
		}
	});
	dialogs.start();
	QMetaObject::invokeMethod(&w, "onSave", Qt::DirectConnection);
	const QString folder = work.filePath("my_video_spark");
	QElapsedTimer deadline; deadline.start();
	while (deadline.elapsed() < 60000) {
		settle(50);
		if (!w.findChild<BatchExporter *>()) break;
	}
	const QStringList files = QDir(folder).entryList({"*.mp4"}, QDir::Files, QDir::Name);
	ok(accepted && files == QStringList{"my_video_spark_01.mp4", "my_video_spark_02.mp4"}, "Random Text options still produce only two numbered Spark MP4 files, not every combination");
	QSet<QByteArray> encoded;
	for (const QString &file : files) {
		QByteArray order;
		for (int ms : {500, 1500, 2500}) order.append(colorAt(QDir(folder).filePath(file), ms));
		encoded.insert(order);
		std::printf("Encoded order: %s\n", order.constData());
	}
	ok(encoded.contains("BRG"), "encoded MP4 contains Blue then Red then Green");
	ok(encoded.contains("BGR") && encoded.size() == 2, "encoded MP4 contains Blue then Green then Red without duplicates");
	ok(view->model() == original, "export preserves original timeline");
	TimelineModel marked = original;
	marked.tracks[footageTrack].clips[1].outStartMs = 1000;
	marked.videoEndMs = 1500; // cut green in half; exclude starred blue entirely
	TlSoundRule sound;
	sound.id = 1;
	sound.sourceId = marked.tracks[footageTrack].clips[0].sourceId;
	sound.trigger = TlSoundRule::Trigger::VideoStarts;
	marked.soundRules.append(sound);
	view->setModelAndCommit(marked);
	settle(100);
	view->selectClip(footageTrack, 0);
	const TimelineModel markedOriginal = view->model();
	prefix = QStringLiteral("marker_spark"); markerExport = true; accepted = false;
	QMetaObject::invokeMethod(&w, "onSave", Qt::DirectConnection);
	deadline.restart();
	while (deadline.elapsed() < 60000) {
		settle(50);
		if (!w.findChild<BatchExporter *>()) break;
	}
	const QDir markedFolder(work.filePath(prefix));
	const QStringList markedFiles = markedFolder.entryList({"*.mp4"}, QDir::Files, QDir::Name);
	ok(accepted && markedFiles.size() == 2, "marked-range export writes only its two unique orders");
	QSet<QByteArray> markedOrders;
	for (const QString &file : markedFiles) {
		const QString path = markedFolder.filePath(file);
		QProcess probe;
		probe.start(QStringLiteral("ffprobe"), {"-v", "error", "-select_streams", "v:0",
			"-show_entries", "stream=duration", "-of", "json", path});
		probe.waitForFinished(20000);
		const auto streams = QJsonDocument::fromJson(probe.readAllStandardOutput()).object()
			.value(QStringLiteral("streams")).toArray();
		const double duration = streams.isEmpty() ? -1 : streams[0].toObject()
			.value(QStringLiteral("duration")).toString().toDouble();
		ok(std::abs(duration - 1.5) < 0.001, "encoded picture stops at the 1.5-second marker despite event-sound tails");
		QByteArray order;
		for (int ms : {250, 750, 1250}) order.append(colorAt(path, ms));
		markedOrders.insert(order);
	}
	ok(markedOrders == QSet<QByteArray>{"RRG", "GRR"},
	   "only pre-marker red and trimmed green are shuffled; blue never enters an export");
	ok(view->model() == markedOriginal, "marked-range export preserves original uncut clips and marker");
	return failures ? 1 : 0;
}
