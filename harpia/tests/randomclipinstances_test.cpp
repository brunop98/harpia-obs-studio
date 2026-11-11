// Linked Random Clips: bidirectional footage pools, independent editing,
// persistence/undo, and Variations that never reuse a source moment.
#include <QtWidgets>
#include <QTest>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
#define private public
#include "editor/VideoEditorWindow.hpp"
#undef private
#include "editor/ExportOptionsDialog.hpp"
#include "editor/FrameSeeker.hpp"
#include "editor/ShortcutRegistry.hpp"
#include "editor/VariationSampling.hpp"
#include "editor/timeline/ClipVariations.hpp"
#include "editor/timeline/TimelineJson.hpp"
#include "editor/timeline/TextVariations.hpp"

#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool pass, const char *message)
{
	std::printf("%s %s\n", pass ? "PASS" : "FAIL", message);
	failures += !pass;
}
TlClip randomClip()
{
	TlClip clip;
	clip.srcStartMs = 1000;
	clip.srcEndMs = 2500;
	clip.clipOptions = {{0, 1000}, {0, 5000, 0.1, -0.2}, {0, 9000}};
	return clip;
}
void checkEditing()
{
	TimelineModel model;
	TlTrack track;
	track.clips = {randomClip()};
	model.tracks = {track};
	TimelineView view;
	view.setSourceThumbs(0, {}, 20000);
	view.setSourceThumbs(1, {}, 20000);
	view.setModel(model);
	view.selectClip(0, 0);
	auto copied = view.copySelection();
	// A copy is a reference to the live original, not to its old track/index.
	view.editModel([](TimelineModel &m) {
		m.tracks[0].clips[0].outStartMs = 2500;
		m.tracks[0].clips[0].clipOptions[1].srcStartMs = 6000;
	});
	int commits = 0;
	QObject::connect(&view, &TimelineView::editCommitted, [&]() { ++commits; });
	QString why;
	check(view.pasteInstancesAt(copied, 7000, &why) && why.isEmpty() && commits == 1,
	      "Paste as instance locates a moved original and commits one edit");
	const auto linked = view.model();
	const auto &a = linked.tracks[0].clips[0];
	const auto &b = linked.tracks[0].clips[1];
	check(!a.randomClipFamily.isEmpty() && a.randomClipFamily == b.randomClipFamily && a.objectId != b.objectId &&
		      b.outStartMs == 7000 && b.clipOptions[1].srcStartMs == 6000,
	      "instances use the live choices and share a family, with separate timeline identities");
	view.editClips({{0, 1}}, [](TlClip &clip) {
		clip.srcEndMs += 1000;
		clip.speed = 2;
		clip.rotation = 22;
		clip.posX = 0.2;
		clip.opacity = 0.4;
		clip.crop = QRect(2, 3, 40, 30);
		clip.fx.enabled = true;
		clip.fx.params.insert(QStringLiteral("amount"), 0.3);
		TlKeyframe key;
		key.tMs = 100;
		key.tf.posX = 0.3;
		clip.keys = {key};
	});
	const TlClip local = view.model().tracks[0].clips[1];
	bool listenerSawShared = false;
	QObject::connect(&view, &TimelineView::clipsChanged, [&]() {
		const auto &clips = view.model().tracks[0].clips;
		if (clips.size() >= 2 && !clips[0].randomClipFamily.isEmpty() &&
		    clips[0].randomClipFamily == clips[1].randomClipFamily) {
			listenerSawShared = random_clip_instances::options(clips[0]) ==
					    random_clip_instances::options(clips[1]);
		}
	});
	view.selectClip(0, 1);
	view.editClips({{0, 1}}, [](TlClip &clip) {
		clip_variations::setOption(clip, 0, 0, 2000, 20000);
		clip.clipOptions[0].offsetX = -0.25;
		clip.clipOptions[0].offsetY = 0.15;
	});
	const TlClip updated = view.model().tracks[0].clips[0];
	check(updated.srcStartMs == 2000 && updated.srcLenMs() == 1500 && updated.clipOptions[0].offsetX == -0.25 &&
		      listenerSawShared,
	      "editing an instance updates the original before preview/Inspector listeners run");
	view.selectClip(0, 0);
	view.editClips({{0, 0}}, [](TlClip &clip) {
		clip.clipOptions[1].srcStartMs = 6500;
		clip.clipOptions[1].offsetY = 0.35;
	});
	const TlClip after = view.model().tracks[0].clips[1];
	check(after.clipOptions[1].srcStartMs == 6500 && after.clipOptions[1].offsetY == 0.35 &&
		      after.outStartMs == local.outStartMs && after.srcLenMs() == local.srcLenMs() &&
		      after.speed == local.speed && after.rotation == local.rotation && after.posX == local.posX &&
		      after.opacity == local.opacity && after.crop == local.crop && after.fx == local.fx &&
		      after.keys == local.keys,
	      "original edits propagate while each copy keeps its placement, duration, speed, effects and keys");
	view.editClips({{0, 1}}, [](TlClip &clip) { clip_variations::setOption(clip, 0, 1, 3000, 20000); });
	check(view.model().tracks[0].clips[0].sourceId == 1 && view.model().tracks[0].clips[1].sourceId == 1 &&
		      view.model().tracks[0].clips[0].srcStartMs == 3000,
	      "replacing the shown video on an instance also changes the original's live footage");
	view.editClips({{0, 1}}, [](TlClip &clip) { clip_variations::show(clip, 2); });
	check(view.model().tracks[0].clips[0].clipShown == 0, "previewing a choice is local to that timeline instance");
	view.editClips({{0, 0}}, [](TlClip &clip) { clip_variations::duplicate(clip, 0); });
	check(view.model().tracks[0].clips[1].clipOptions.size() == 4 &&
		      view.model().tracks[0].clips[1].clipShown == 3 &&
		      view.model().tracks[0].clips[1].srcStartMs == 9000,
	      "adding a choice propagates without switching another copy's preview");
	view.editClips({{0, 1}}, [](TlClip &clip) { clip_variations::remove(clip, 1); });
	check(view.model().tracks[0].clips[0].clipOptions.size() == 3 && view.model().tracks[0].clips[1].clipShown == 2,
	      "removing a choice from any copy updates the shared list");
	const auto beforeInvalid = view.model();
	QString rejected;
	QObject::connect(&view, &TimelineView::instanceEditRejected, [&](const QString &s) { rejected = s; });
	view.editClips({{0, 0}}, [](TlClip &clip) { clip_variations::setOption(clip, 0, 0, 18000, 20000); });
	check(!rejected.isEmpty() && view.model() == beforeInvalid,
	      "source-time edits that cannot fill a longer instance are rejected without partial changes");
	view.pasteAt(copied, 12000);
	check(view.model().tracks[0].clips[2].randomClipFamily.isEmpty() &&
		      view.model().tracks[0].clips[2].objectId != copied[0].clip.objectId,
	      "ordinary Paste still creates an independent copy");
	check(view.makeRandomClipIndependent(0, 1), "Make independent disconnects just that instance");
	const TlClip independent = view.model().tracks[0].clips[1];
	view.editClips({{0, 0}}, [](TlClip &clip) { clip.clipOptions[1].srcStartMs = 7000; });
	check(view.model().tracks[0].clips[1] == independent, "independent copies stop receiving shared edits");
	view.setModel(linked);
	view.selectClip(0, 0);
	copied = view.copySelection();
	view.deleteSelected();
	check(view.pasteInstancesAt(copied, 14000, &why) && view.model().tracks[0].clips.size() == 2 &&
		      view.model().tracks[0].clips[0].randomClipFamily ==
			      view.model().tracks[0].clips[1].randomClipFamily,
	      "a family survives deleting the first original; clipboard can instance a surviving member");
	TimelineModel unrelated = model;
	unrelated.tracks[0].clips[0].objectId = TlClip::nextObjectId();
	view.setModel(unrelated);
	const auto beforeMissing = view.model();
	check(!view.pasteInstancesAt(copied, 0, &why) && !why.isEmpty() && view.model() == beforeMissing,
	      "pasting an absent family fails clearly without changing the project");
	const TlClip loaded = clipFromJson(clipToJson(a));
	check(loaded == a && loaded.randomClipFamily == a.randomClipFamily && loaded.objectId != a.objectId,
	      "project JSON preserves family linkage but creates fresh session identities");
	view.setCanPasteInstances(true);
	view.resize(900, 300);
	view.show();
	qint64 requestedAt = -1;
	QObject::connect(&view, &TimelineView::pasteInstanceRequested, [&](qint64 at) { requestedAt = at; });
	QTimer choose;
	QObject::connect(&choose, &QTimer::timeout, [&]() {
		if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
			if (auto *action =
				    menu->findChild<QAction *>(QStringLiteral("pasteRandomClipInstanceAction"))) {
				menu->setActiveAction(action);
				QTest::keyClick(menu, Qt::Key_Return);
			}
		}
	});
	choose.start(10);
	view.showTrackMenu(0, QPoint(20, 20), 16000);
	check(requestedAt == 16000, "empty-lane context menu pastes instances at the clicked time");
	view.showClipMenu(0, 0, QPoint(20, 20), 17000);
	check(requestedAt == 17000, "clip context menu offers the same Paste as instance action");
	choose.stop();
	view.hide();
}

void checkVariations()
{
	TimelineModel model;
	TlTrack track;
	TlClip first = randomClip();
	first.randomClipFamily = QStringLiteral("family");
	TlClip second = first;
	second.objectId = TlClip::nextObjectId();
	second.outStartMs = 2000;
	second.srcEndMs += 500;
	second.rotation = 12;
	track.clips = {first, second};
	model.tracks = {track};
	using namespace text_variations;
	auto variants = slotsOf(model);
	check(choiceSizes(variants) == QVector<int>{3, 2} && combinationCount(variants) == 6 &&
		      instanceError(variants).isEmpty(),
	      "two linked copies with three moments produce six valid assignments");
	QSet<QVector<int>> seen;
	bool valid = true;
	for (int i = 0; i < 6; ++i) {
		const auto decisions = combinationAt(variants, i);
		const auto indices = optionIndices(variants, decisions);
		seen.insert(indices);
		const auto version = applyCombination(model, variants, decisions);
		const TlClip &a = version.tracks[0].clips[0], &b = version.tracks[0].clips[1];
		valid &= a.srcStartMs != b.srcStartMs && a.srcLenMs() == first.srcLenMs() &&
			 b.srcLenMs() == second.srcLenMs() && b.rotation == 12 &&
			 textsAt(variants, decisions).value(1) == variants[1].options[indices[1]];
	}
	check(valid && seen.size() == 6, "all assignments are unique, never repeat a moment, and retain local edits");
	const auto sample = variation_sampling::sample(choiceSizes(variants), 4, 42);
	seen.clear();
	for (const auto &pick : sample) {
		seen.insert(optionIndices(variants, pick));
	}
	check(sample.size() == 4 && seen.size() == 4, "random subsets sample the valid assignments without repeats");
	for (TlClip &clip : model.tracks[0].clips) {
		clip.clipOptions.append({0, 1000, 0.8, -0.3});
	}
	variants = slotsOf(model);
	check(choiceSizes(variants) == QVector<int>{3, 2} && combinationCount(variants) == 6,
	      "duplicate video/start choices do not disguise repeated moments as different XY variants");
	for (TlClip &clip : model.tracks[0].clips) {
		clip.clipOptions.append({1, 1000});
	}
	const auto aliased = slotsOf(model, {}, [](int) { return QStringLiteral("same-source.mp4"); });
	check(combinationCount(aliased) == 6,
	      "importing the same file under another source ID does not create a false distinct moment");
	for (TlClip &clip : model.tracks[0].clips) {
		clip.clipOptions.removeLast();
	}
	model.tracks[0].clips.append(first);
	model.tracks[0].clips.append(second);
	variants = slotsOf(model);
	check(!instanceError(variants).isEmpty() && combinationCount(variants) == 0 &&
		      optionIndices(variants, QVector<int>(variants.size(), 0)).isEmpty(),
	      "four linked copies with only three moments cannot produce a repeated-footage export");
	ExportOptionsDialog::Context context;
	context.seconds = 6;
	context.sourceSize = QSize(160, 90);
	context.variantOptionCounts = choiceSizes(variants);
	context.variantError = instanceError(variants);
	ExportOptionsDialog dialog(context);
	dialog.setBatchTabForTest(true);
	dialog.accept();
	check(dialog.findChild<QLabel *>(QStringLiteral("variationInstanceError")) &&
		      dialog.variationSelection().isEmpty() && dialog.result() != QDialog::Accepted,
	      "Variations shows the actionable error and refuses export when moments are insufficient");
	dialog.setBatchTabForTest(false);
	dialog.accept();
	check(dialog.result() == QDialog::Accepted, "the current preview can still export as a Single video");
	model.videoEndMs = 3500;
	model.tracks[0].clips[2].outStartMs = 4000;
	model.tracks[0].clips[3].outStartMs = 6000;
	check(instanceError(slotsOf(model)).isEmpty(), "copies beyond the video end do not block an export");
	model.tracks[0].clips = {first, second};
	model.tracks[0].clips[1].randomClipFamily.clear();
	check(combinationCount(slotsOf(model)) == 9,
	      "ordinary independent copies keep their Cartesian-product variations");
}

bool generateSource(const QString &path)
{
	QProcess process;
	process.start(QStringLiteral("ffmpeg"), {"-v",
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
						 path});
	return process.waitForFinished(20000) && process.exitCode() == 0;
}
void checkWindowAndExport(const QTemporaryDir &temporary)
{
	const QString source = temporary.filePath("moments.mp4");
	check(generateSource(source), "generated distinct red, green and blue source moments");
	if (!QFileInfo::exists(source)) {
		return;
	}
	TlClip clip;
	clip.srcEndMs = 400;
	clip.clipOptions = {{0, 0}, {0, 1000}, {0, 2000}};
	TlTrack track;
	track.clips = {clip};
	TimelineModel model;
	model.tracks = {track};
	QJsonObject root{{"harpiaProject", 3}, {"editMode", "full"},
			 {"canvasW", 160},     {"canvasH", 90},
			 {"fps", 10},          {"sources", QJsonArray{QJsonObject{{"id", 0}, {"path", source}}}}};
	timelineToProject(model, root);
	const QString project = temporary.filePath("instances.harpia");
	QFile file(project);
	file.open(QIODevice::WriteOnly);
	file.write(QJsonDocument(root).toJson());
	file.close();
	VideoEditorWindow window{QString()};
	check(window.openProjectAt(project), "editor opens the Random Clip test project");
	auto *view = window.timelineView_;
	view->selectClip(0, 0);
	window.copySelectedClips(false);
	const auto original = view->model();
	const int historyBefore = window.histIndex_;
	view->setPlayhead(400);
	const ShortcutCommand *paste = window.shortcuts_->command(QStringLiteral("edit.pasteInstance"));
	check(paste && paste->defaults.contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V)),
	      "Paste as instance is registered as a rebindable Ctrl+Shift+V command");
	if (paste) {
		paste->run();
	}
	const auto linked = view->model();
	check(linked.tracks[0].clips.size() == 2 && !linked.tracks[0].clips[0].randomClipFamily.isEmpty() &&
		      window.histIndex_ == historyBefore + 1 &&
		      window.randomClipIndependent_->isVisibleTo(window.randomClipBox_),
	      "editor Paste as instance creates one undo entry and shows the linked-copy control");
	window.undo();
	check(view->model() == original, "Undo removes the pasted instance and the original's new family tag together");
	window.redo();
	check(view->model() == linked, "Redo restores the whole family");
	view->selectClip(0, 1);
	window.removeRandomClipOption(2);
	check(view->model().tracks[0].clips[0].clipOptions.size() == 2,
	      "real Inspector removal propagates to the original");
	window.undo();
	check(view->model() == linked, "Undo restores both copies' option lists in one step");
	window.redo();
	check(view->model().tracks[0].clips[0].clipOptions.size() == 2 &&
		      view->model().tracks[0].clips[1].clipOptions.size() == 2,
	      "Redo restores shared edits without an old cache undoing them");
	window.undo();
	const QString saved = temporary.filePath("linked.harpia");
	check(window.saveProjectTo(saved, true).isEmpty() && window.openProjectAt(saved),
	      "linked project saves and reopens");
	view->selectClip(0, 1);
	window.removeRandomClipOption(2);
	check(view->model().tracks[0].clips[0].clipOptions.size() == 2,
	      "linking remains bidirectional after reopening the project");
	window.undo();
	const auto beforeExport = view->model();
	QVector<ExportOptionsDialog::Variation> selected;
	bool shown = false;
	QTimer driver;
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		if (auto *dialog = qobject_cast<ExportOptionsDialog *>(QApplication::activeModalWidget());
		    dialog && !shown) {
			shown = true;
			dialog->setBatchTabForTest(true);
			check(dialog->variationSelection().size() == 6,
			      "actual Variations dialog lists exactly six no-repeat exports");
			dialog->findChild<QSpinBox *>(QStringLiteral("variationRandomCount"))->setValue(3);
			auto *mode = dialog->findChild<QComboBox *>(QStringLiteral("variationSelectionMode"));
			mode->setCurrentIndex(mode->findData(true));
			selected = dialog->variationSelection();
			dialog->findChild<QLineEdit *>(QStringLiteral("batchPrefix"))->setText(QStringLiteral("linked"));
			dialog->findChild<QLineEdit *>(QStringLiteral("exportFolder"))->setText(temporary.path());
			dialog->findChild<QCheckBox *>(QStringLiteral("batchOpenFolder"))->setChecked(false);
			dialog->accept();
		} else if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
			message->accept();
		}
	});
	driver.start(10);
	QMetaObject::invokeMethod(&window, "onSave", Qt::DirectConnection);
	check(shown && QTest::qWaitFor([&]() { return !window.findChild<BatchExporter *>(); }, 30000),
	      "real editor renders the selected linked-copy variations");
	driver.stop();
	const QDir folder(temporary.filePath("linked"));
	const auto files = folder.entryList({"*.mp4"}, QDir::Files, QDir::Name);
	bool valid = files.size() == 3 && selected.size() == 3;
	const auto variants = text_variations::slotsOf(beforeExport);
	for (int i = 0; i < files.size() && i < selected.size(); ++i) {
		FrameSeeker seeker;
		if (!seeker.open(folder.filePath(files[i]))) {
			valid = false;
			continue;
		}
		const auto choices = text_variations::optionIndices(variants, selected[i].choices);
		for (int j = 0; j < 2; ++j) {
			const QImage frame = seeker.frameAt(j * 400 + 100, 160, 90);
			if (frame.isNull()) {
				valid = false;
				continue;
			}
			const QColor pixel = frame.pixelColor(80, 45);
			valid &= choices[j] == 0   ? pixel.red() > 180 && pixel.green() < 60
				 : choices[j] == 1 ? pixel.green() > 180 && pixel.red() < 60
						   : pixel.blue() > 180 && pixel.red() < 60;
		}
		valid &= choices[0] != choices[1];
	}
	check(valid, "decoded exports play two different source moments matching each selected assignment");
	check(view->model() == beforeExport, "export leaves the original preview choices and linked project unchanged");
	window.hide();
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
	checkEditing();
	checkVariations();
	checkWindowAndExport(temporary);
	return failures ? 1 : 0;
}
