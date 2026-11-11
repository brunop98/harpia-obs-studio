// Drive the real Variations dialog and render only three combinations from a
// pool exceeding the old 2000-row limit. Decode each file to check its footage.
#include "editor/VideoEditorWindow.hpp"
#include "editor/ExportOptionsDialog.hpp"
#include "editor/VariationSampling.hpp"
#include "editor/FrameSeeker.hpp"
#include "editor/timeline/TimelineJson.hpp"

#include <QtWidgets>
#include <QTest>
#include <cstdio>

using namespace harpia;
namespace {
int failures;
void check(bool passed, const char *message)
{
	std::printf("%s %s\n", passed ? "PASS" : "FAIL", message);
	failures += !passed;
}
void checkDialog()
{
	ExportOptionsDialog::Context context;
	context.seconds = 2;
	context.sourceSize = QSize(160, 90);
	context.variantOptionCounts = {1000, 1000};
	int labels = 0;
	context.variantLabel = [&labels](const QVector<int> &pick) {
		++labels;
		return QStringLiteral("%1 / %2").arg(pick[0]).arg(pick[1]);
	};
	ExportOptionsDialog dialog(context);
	dialog.setBatchTabForTest(true);
	check(labels == 10 && dialog.batchListForTest()->count() == 10 && dialog.exportSeconds() == 20,
	      "large pools initially materialize only ten sampled rows and estimate only their duration");
	auto *count = dialog.findChild<QSpinBox *>(QStringLiteral("variationRandomCount"));
	count->setValue(25);
	QSet<QVector<int>> choices;
	bool beyond = false;
	for (const auto &version : dialog.variationSelection()) {
		choices.insert(version.choices);
		beyond |= version.number > 2000;
	}
	check(dialog.variationSelection().size() == 25 && choices.size() == 25 && labels == 35 && beyond &&
		      dialog.exportSeconds() == 50,
	      "changing the count immediately selects exactly 25 unique versions across the full pool");
	dialog.findChild<QPushButton *>(QStringLiteral("variationPickAgain"))->click();
	QSet<QVector<int>> rerolled;
	for (const auto &version : dialog.variationSelection()) {
		rerolled.insert(version.choices);
	}
	check(rerolled.size() == 25 && rerolled != choices, "Pick again draws a fresh unique selection");
	context.variantOptionCounts = {3, 4};
	context.variantLabel = [](const QVector<int> &) {
		return QStringLiteral("small combination");
	};
	ExportOptionsDialog small(context);
	small.setBatchTabForTest(true);
	check(small.variationSelection().size() == 12, "small pools retain the existing complete checklist by default");
	auto *mode = small.findChild<QComboBox *>(QStringLiteral("variationSelectionMode"));
	small.findChild<QSpinBox *>(QStringLiteral("variationRandomCount"))->setValue(3);
	mode->setCurrentIndex(mode->findData(true));
	check(small.variationSelection().size() == 3 && small.exportSeconds() == 6,
	      "small pools also support an optional random subset");
	mode->setCurrentIndex(mode->findData(false));
	check(small.variationSelection().size() == 12, "switching back restores the complete checklist");
}
} // namespace
int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir temporary;
	qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
	QApplication app(argc, argv);
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
	QSettings().setValue(QStringLiteral("export/format"), int(ClipExporter::Format::Mp4));
	checkDialog();
	const QString source = temporary.filePath("moments.mp4");
	QProcess encode;
	encode.start(QStringLiteral("ffmpeg"), {"-v",
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
						source});
	check(encode.waitForFinished(20000) && encode.exitCode() == 0, "generated three distinct source moments");
	if (failures) {
		return 1;
	}
	TimelineModel model;
	for (int i = 0; i < 2; ++i) {
		TlClip caption;
		caption.type = TlClip::Type::Text;
		caption.srcEndMs = 1000;
		caption.text.random = true;
		caption.text.text = QStringLiteral("%1-0").arg(i);
		caption.text.fontPx = 8;
		caption.posY = i ? 0.2 : 0.1;
		for (int k = 1; k < 64; ++k) {
			caption.text.variations.append(QStringLiteral("%1-%2").arg(i).arg(k));
		}
		TlTrack words;
		words.clips = {caption};
		model.tracks.append(words);
	}
	TlClip footage;
	footage.srcEndMs = 1000;
	footage.clipOptions = {{0, 0}, {0, 1000}, {0, 2000}};
	TlTrack video;
	video.clips = {footage};
	model.tracks.append(video);
	QJsonObject root{{"harpiaProject", 3}, {"editMode", "full"},
			 {"canvasW", 160},     {"canvasH", 90},
			 {"fps", 10},          {"sources", QJsonArray{QJsonObject{{"id", 0}, {"path", source}}}}};
	timelineToProject(model, root);
	const QString project = temporary.filePath("sample.harpia");
	QFile file(project);
	file.open(QIODevice::WriteOnly);
	file.write(QJsonDocument(root).toJson());
	file.close();
	VideoEditorWindow window{QString()};
	check(window.openProjectAt(project), "the real editor loads 12288 possible variations");
	auto *view = window.findChild<TimelineView *>();
	const auto before = view->model();
	QVector<ExportOptionsDialog::Variation> selected;
	bool seen = false, unexpectedPrompt = false;
	QTimer driver;
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		auto *modal = QApplication::activeModalWidget();
		if (auto *dialog = qobject_cast<ExportOptionsDialog *>(modal); dialog && !seen) {
			seen = true;
			dialog->setBatchTabForTest(true);
			dialog->findChild<QSpinBox *>(QStringLiteral("variationRandomCount"))->setValue(3);
			selected = dialog->variationSelection();
			check(selected.size() == 3 && dialog->exportSeconds() == 3,
			      "the actual export dialog selects only three one-second videos");
			dialog->findChild<QLineEdit *>(QStringLiteral("batchPrefix"))
				->setText(QStringLiteral("sampled"));
			dialog->findChild<QLineEdit *>(QStringLiteral("exportFolder"))->setText(temporary.path());
			dialog->findChild<QCheckBox *>(QStringLiteral("batchOpenFolder"))->setChecked(false);
			dialog->accept();
		} else if (auto *message = qobject_cast<QMessageBox *>(modal)) {
			unexpectedPrompt |= !seen;
			message->accept();
		}
	});
	driver.start(10);
	QMetaObject::invokeMethod(&window, "onSave", Qt::DirectConnection);
	check(seen && !unexpectedPrompt &&
		      QTest::qWaitFor([&]() { return !window.findChild<BatchExporter *>(); }, 30000),
	      "the sampled batch finishes without the old first-2000 warning");
	const QDir folder(temporary.filePath("sampled"));
	const QStringList files = folder.entryList({"*.mp4"}, QDir::Files, QDir::Name);
	check(files.size() == 3, "only the three requested variations are rendered");
	bool correct = files.size() == selected.size();
	for (int i = 0; i < files.size() && i < selected.size(); ++i) {
		const auto &version = selected[i];
		correct &=
			files[i].startsWith(QStringLiteral("sampled_%1_").arg(version.number, 5, 10, QLatin1Char('0')));
		FrameSeeker seeker;
		if (!seeker.open(folder.filePath(files[i]))) {
			correct = false;
			continue;
		}
		const QImage frame = seeker.frameAt(250, 160, 90);
		if (frame.isNull()) {
			correct = false;
			continue;
		}
		const QColor pixel = frame.pixelColor(80, 80);
		const int choice = version.choices[2];
		correct &= choice == 0   ? pixel.red() > 180 && pixel.green() < 60
			   : choice == 1 ? pixel.green() > 180 && pixel.red() < 60
					 : pixel.blue() > 180 && pixel.red() < 60;
	}
	check(correct, "encoded filenames and footage match exactly the selected combinations");
	check(view->model() == before, "sampling and export leave the editor's original project unchanged");
	driver.stop();
	window.hide();
	return failures ? 1 : 0;
}
