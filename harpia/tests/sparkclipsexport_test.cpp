// A real Spark batch with a three-moment Random Clip and one ordinary clip:
// two orders x three choices must write six distinct, correctly framed videos.
#include "editor/VideoEditorWindow.hpp"
#include "editor/ExportOptionsDialog.hpp"
#include "editor/FrameSeeker.hpp"
#include "editor/timeline/TimelineJson.hpp"

#include <QtWidgets>
#include <QTest>
#include <cstdio>

using namespace harpia;
namespace {
int failures;
void check(bool pass, const char *why)
{
	std::printf("%s %s\n", pass ? "PASS" : "FAIL", why);
	failures += !pass;
}
bool encode(const QStringList &args)
{
	QProcess process;
	process.start(QStringLiteral("ffmpeg"), args);
	return process.waitForFinished(20000) && process.exitCode() == 0;
}
char colour(const QImage &image)
{
	if (image.isNull()) {
		return '?';
	}
	const auto c = image.pixelColor(image.width() / 2, image.height() / 2);
	if (c.red() > 180 && c.green() > 180 && c.blue() < 60) {
		return 'Y';
	}
	if (c.red() > c.green() + 40 && c.red() > c.blue() + 40) {
		return 'R';
	}
	if (c.green() > c.red() + 40 && c.green() > c.blue() + 40) {
		return 'G';
	}
	if (c.blue() > c.red() + 40 && c.blue() > c.green() + 40) {
		return 'B';
	}
	return '?';
}
} // namespace
int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir temporary;
	qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
	QApplication app(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaSparkClipsTests"));
	QCoreApplication::setApplicationName(QStringLiteral("SparkClipExports"));
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
	QSettings().setValue(QStringLiteral("export/format"), int(ClipExporter::Format::Mp4));
	const QString moments = temporary.filePath("moments.mp4"), yellow = temporary.filePath("yellow.mp4");
	check(encode({"-v",
		      "error",
		      "-f",
		      "lavfi",
		      "-i",
		      "color=c=red:s=160x90:r=10:d=1",
		      "-f",
		      "lavfi",
		      "-i",
		      "color=c=lime:s=160x90:r=10:d=1",
		      "-f",
		      "lavfi",
		      "-i",
		      "color=c=blue:s=160x90:r=10:d=1",
		      "-filter_complex",
		      "[0:v][1:v][2:v]concat=n=3:v=1:a=0",
		      "-c:v",
		      "libx264",
		      "-pix_fmt",
		      "yuv420p",
		      "-y",
		      moments}) &&
		      encode({"-v", "error", "-f", "lavfi", "-i", "color=c=yellow:s=160x90:r=10:d=1", "-c:v", "libx264",
			      "-pix_fmt", "yuv420p", "-y", yellow}),
	      "fixtures contain three distinct moments in one source and an ordinary yellow video");
	if (failures) {
		return 1;
	}
	TlClip random, ordinary;
	random.srcEndMs = 1000;
	random.clipOptions = {{0, 0, 0, 0}, {0, 1000, 0.15, 0}, {0, 2000, -0.15, 0}};
	ordinary.sourceId = 1;
	ordinary.srcEndMs = 1000;
	ordinary.outStartMs = 1000;
	TlTrack track;
	track.clips = {random, ordinary};
	TimelineModel model;
	model.tracks = {track};
	QJsonObject root{{"harpiaProject", 3}, {"editMode", "full"}, {"canvasW", 160}, {"canvasH", 90}, {"fps", 10}};
	root[QStringLiteral("sources")] =
		QJsonArray{QJsonObject{{"id", 0}, {"path", moments}}, QJsonObject{{"id", 1}, {"path", yellow}}};
	timelineToProject(model, root);
	const QString project = temporary.filePath("spark-clips.harpia");
	QFile file(project);
	file.open(QIODevice::WriteOnly);
	file.write(QJsonDocument(root).toJson());
	file.close();
	VideoEditorWindow window{QString()};
	window.show();
	check(window.openProjectAt(project), "real editor opens the Random Clip project");
	auto *view = window.findChild<TimelineView *>();
	if (!view) {
		return 2;
	}
	const auto original = view->model();
	bool seen = false;
	QElapsedTimer timeout;
	timeout.start();
	QTimer driver;
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		auto *modal = QApplication::activeModalWidget();
		if (auto *dialog = qobject_cast<ExportOptionsDialog *>(modal); dialog && !seen) {
			seen = true;
			dialog->setSparkTabForTest(true);
			dialog->findChild<QSpinBox *>(QStringLiteral("sparkRequested"))->setValue(100);
			check(dialog->sparkCount() == 6 && dialog->findChild<QLabel *>(QStringLiteral("sparkStats"))
								   ->text()
								   .contains("6 unique videos"),
			      "Spark's actual dialog offers six unique videos, not just two orders");
			dialog->findChild<QLineEdit *>(QStringLiteral("sparkPrefix"))
				->setText(QStringLiteral("choices"));
			dialog->findChild<QLineEdit *>(QStringLiteral("exportFolder"))->setText(temporary.path());
			dialog->findChild<QCheckBox *>(QStringLiteral("sparkOpenFolder"))->setChecked(false);
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
	check(seen && QTest::qWaitFor([&]() { return !window.findChild<BatchExporter *>(); }, 30000),
	      "the real Spark batch finishes");
	const QDir folder(temporary.filePath("choices"));
	const auto files = folder.entryList({QStringLiteral("*.mp4")}, QDir::Files, QDir::Name);
	check(files.size() == 6, "Spark writes all six exports");
	QSet<QByteArray> sequences;
	bool framed = true;
	for (const QString &name : files) {
		FrameSeeker seeker;
		if (!seeker.open(folder.filePath(name))) {
			framed = false;
			continue;
		}
		const QImage first = seeker.frameAt(250, 160, 90), second = seeker.frameAt(1250, 160, 90);
		QByteArray sequence;
		sequence += colour(first);
		sequence += colour(second);
		sequences.insert(sequence);
		const QImage chosen = colour(first) == 'Y' ? second : first;
		const char selected = colour(chosen);
		const auto dark = [](const QColor &c) {
			return c.red() < 30 && c.green() < 30 && c.blue() < 30;
		};
		if (selected == 'G') {
			framed &= dark(chosen.pixelColor(4, 45));
		} else if (selected == 'B') {
			framed &= dark(chosen.pixelColor(155, 45));
		} else {
			framed &= selected == 'R' && !dark(chosen.pixelColor(4, 45));
		}
		const QImage later = seeker.frameAt(colour(first) == 'Y' ? 1750 : 750, 160, 90);
		framed &= colour(later) == selected;
	}
	check(sequences == QSet<QByteArray>{"RY", "GY", "BY", "YR", "YG", "YB"},
	      "encoded exports contain every option's correct source timestamp in both block orders");
	check(framed, "each option retains its own position and stays selected throughout its clip");
	check(view->model() == original, "the editor's preview choice and full project remain unchanged after export");
	driver.stop();
	window.hide();
	std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
	return failures ? 1 : 0;
}
