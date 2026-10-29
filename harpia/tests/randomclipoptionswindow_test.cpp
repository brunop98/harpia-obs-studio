// Each Random clip choice picks its own source and moment while retaining one
// shared timeline slot. Drive the Inspector and the existing Slip dialog.
#include <QtWidgets>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
#define private public
#include "editor/VideoEditorWindow.hpp"
#undef private
#include "editor/SlipDialog.hpp"
#include "editor/InspectorSection.hpp"
#include "editor/timeline/ClipVariations.hpp"
#include "editor/timeline/TextVariations.hpp"

#include <cmath>
#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}
void settle()
{
	QApplication::processEvents();
}
bool video(const QString &path, const QString &colour, int seconds)
{
	QProcess ffmpeg;
	ffmpeg.start(QStringLiteral("ffmpeg"),
		{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
		 QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
		 QStringLiteral("color=c=%1:s=64x48:r=30:d=%2").arg(colour).arg(seconds),
		 QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"),
		 QStringLiteral("yuv420p"), QStringLiteral("-y"), path});
	return ffmpeg.waitForFinished(15000) && ffmpeg.exitStatus() == QProcess::NormalExit && ffmpeg.exitCode() == 0;
}
QAbstractButton *optionButton(VideoEditorWindow &window, const char *name, int option)
{
	for (auto *button : window.findChildren<QAbstractButton *>(QLatin1String(name)))
		if (button->property("optionIndex").toInt() == option)
			return button;
	return nullptr;
}
QAction *sourceAction(QAbstractButton *button, int source)
{
	auto *picker = qobject_cast<QToolButton *>(button);
	if (!picker || !picker->menu())
		return nullptr;
	emit picker->menu()->aboutToShow();
	for (auto *action : picker->menu()->actions())
		if (action->data().isValid() && action->data().toInt() == source)
			return action;
	return nullptr;
}
struct SlipRun {
	bool seen = false;
	bool hasTimeInput = false;
	bool timedOut = false;
	qint64 baseStart = -1;
	qint64 resultingStart = -1;
	int sourceId = -1;
	QImage preview;
};
SlipRun slip(const std::function<void()> &open, qint64 requestedStart, bool accept)
{
	SlipRun result;
	QElapsedTimer elapsed;
	elapsed.start();
	QTimer driver;
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		if (elapsed.elapsed() > 5000) {
			result.timedOut = true;
			if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
				dialog->reject();
			return;
		}
		auto *dialog = dynamic_cast<SlipDialog *>(QApplication::activeModalWidget());
		if (!dialog || result.seen)
			return;
		result.seen = true;
		const auto before = dialog->momentModel(false);
		if (!before.tracks.isEmpty() && !before.tracks.front().clips.isEmpty()) {
			result.baseStart = before.tracks.front().clips.front().srcStartMs;
			result.sourceId = before.tracks.front().clips.front().sourceId;
		}
		if (auto *time = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("slipSourceStart"))) {
			result.hasTimeInput = true;
			if (requestedStart >= 0)
				time->setValue(requestedStart / 1000.0);
			result.resultingStart = qint64(std::llround(time->value() * 1000.0));
		}
		result.preview = dialog->previewImage();
		if (accept)
			dialog->accept();
		else
			dialog->reject();
	});
	driver.start(10);
	open();
	return result;
}
TlClip current(const VideoEditorWindow &window)
{
	return window.timelineView_->model().tracks.front().clips.front();
}
void install(VideoEditorWindow &window, const TimelineModel &model)
{
	window.setEditMode(VideoEditorWindow::EditMode::Full);
	window.timelineView_->setModel(model);
	window.timelineView_->selectClip(0, 0);
	window.syncClipInspector();
	window.commitSnapshot();
	settle();
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
	QTemporaryDir temporary;
	if (!temporary.isValid())
		return 2;
	qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
	qputenv("XDG_CACHE_HOME", temporary.filePath("cache").toUtf8());
	qputenv("XDG_DATA_HOME", temporary.filePath("data").toUtf8());
	QApplication app(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaRandomClipOptionTests"));
	QCoreApplication::setApplicationName(QStringLiteral("RandomClipOptions"));
	for (QSettings::Format format : {QSettings::IniFormat, QSettings::NativeFormat}) {
		QSettings::setPath(format, QSettings::UserScope, temporary.path());
		QSettings::setPath(format, QSettings::SystemScope, temporary.filePath("system"));
	}
	const QString redPath = temporary.filePath("red.mp4"), bluePath = temporary.filePath("blue.mp4"),
		greenPath = temporary.filePath("green.mp4"), shortPath = temporary.filePath("short.mp4");
	check(video(redPath, QStringLiteral("red"), 6) && video(bluePath, QStringLiteral("blue"), 6) &&
	      video(greenPath, QStringLiteral("green"), 6) && video(shortPath, QStringLiteral("white"), 1),
	      "test sources provide three distinct videos and one source shorter than the slot");
	if (failures)
		return 1;
	VideoEditorWindow window{QString()};
	const int red = window.addSource(redPath), blue = window.addSource(bluePath),
		green = window.addSource(greenPath), shortSource = window.addSource(shortPath);
	check(red >= 0 && blue >= 0 && green >= 0 && shortSource >= 0, "all source videos load into the project");
	if (failures)
		return 1;
	TlClip slot;
	slot.sourceId = red;
	slot.srcStartMs = 1000;
	slot.srcEndMs = 3000;
	slot.outStartMs = 1000;
	slot.speed = 2;
	slot.posX = 0.3;
	slot.scale = 1.4;
	slot.crop = QRect(0, 0, 48, 32);
	slot.fadeInMs = 80;
	slot.fadeOutMs = 120;
	slot.link = 71;
	TlKeyframe key;
	key.tMs = 300;
	key.tf.posX = 0.4;
	slot.keys = {key};
	ComponentInstance effect;
	effect.typeId = QStringLiteral("harpia.alwaysRotate");
	effect.instanceId = QStringLiteral("slot-effect");
	effect.enabled = false;
	slot.components = {effect};
	slot.clipOptions = {{red, 1000}, {blue, 500}, {red, 2000}};
	TlTrack track;
	track.clips = {slot};
	TlClip linked = slot;
	linked.clipOptions.clear();
	TlTrack audio;
	audio.kind = TlTrack::Kind::Audio;
	audio.muted = true;
	audio.clips = {linked};
	TimelineModel initial;
	initial.tracks = {track, audio};
	install(window, initial);
	check(optionButton(window, "randomClipVideo", 1) && optionButton(window, "randomClipSlip", 1),
	      "every random choice offers its own video selector and Slip button");
	const auto *picker = qobject_cast<QToolButton *>(optionButton(window, "randomClipVideo", 1));
	check(picker && picker->text().contains(QStringLiteral("blue")) && picker->menu() &&
	      picker->popupMode() == QToolButton::MenuButtonPopup,
	      "one named source control provides both option preview and its source dropdown");
	const auto *section = static_cast<InspectorSection *>(window.randomClipBox_);
	bool visibleIntro = false;
	for (auto *label : section->findChildren<QLabel *>())
		visibleIntro |= label->text().startsWith(QStringLiteral("Each clip is one version"));
	check(!visibleIntro && section->header()->toolTip().contains(QStringLiteral("Each clip is one version")),
	      "the explanation lives in the section header tooltip");
	const auto redFrames = window.sourceById(red)->thumbCache;
	const auto blueFrames = window.sourceById(blue)->thumbCache;
	QImage redFrame(64, 36, QImage::Format_RGB32), greenFrame = redFrame, blueFrame = redFrame;
	redFrame.fill(Qt::red);
	greenFrame.fill(Qt::green);
	blueFrame.fill(Qt::blue);
	window.sourceById(red)->thumbCache = {redFrame, greenFrame, blueFrame};
	window.sourceById(blue)->thumbCache = {blueFrame};
	window.refreshRandomClipThumbnails();
	const auto thumbs = window.randomClipThumbs_;
	check(thumbs.size() == 3 && thumbs[0]->pixmap().toImage().pixelColor(20, 16) == QColor(Qt::red) &&
	      thumbs[1]->pixmap().toImage().pixelColor(20, 16) == QColor(Qt::blue) &&
	      thumbs[2]->pixmap().toImage().pixelColor(20, 16) == QColor(Qt::green),
	      "cached thumbnails follow each option's own source and timestamp, including duplicate sources");
	window.sourceById(red)->thumbCache = redFrames;
	window.sourceById(blue)->thumbCache = blueFrames;
	window.refreshRandomClipThumbnails();
	auto *slipButton = optionButton(window, "randomClipSlip", 1);
	auto *removeButton = optionButton(window, "randomClipRemove", 1);
	auto *row = slipButton->parentWidget();
	row->resize(320, 60);
	row->layout()->activate();
	check(removeButton && std::abs(removeButton->geometry().center().y() - slipButton->geometry().center().y()) <= 1 &&
	      removeButton->x() > slipButton->x(),
	      "Slip and Remove sit beside one another on the same row");
	QAction *tooShort = sourceAction(optionButton(window, "randomClipVideo", 1), shortSource);
	check(tooShort && !tooShort->isEnabled(), "source selector disables videos too short to fill the shared source window");
	QAction *chooseGreen = sourceAction(optionButton(window, "randomClipVideo", 1), green);
	check(chooseGreen && chooseGreen->isEnabled(), "source selector offers a compatible replacement video");
	if (!chooseGreen)
		return 1;
	const auto beforeCancel = window.timelineView_->model();
	const int historyBeforeCancel = window.histIndex_;
	const auto cancel = slip([&]() { chooseGreen->trigger(); }, 1500, false);
	check(cancel.seen && cancel.hasTimeInput && cancel.sourceId == green && !cancel.timedOut,
	      "choosing another video opens that video's Slip preview and exact source-start input");
	check(window.timelineView_->model() == beforeCancel && window.histIndex_ == historyBeforeCancel,
	      "Cancel leaves the chosen source, shown choice and undo history unchanged");
	const auto chosen = slip([&]() { chooseGreen->trigger(); }, 1234, true);
	check(chosen.seen && chosen.resultingStart == 1234, "Slip accepts a millisecond-precision source timestamp");
	if (!chosen.preview.isNull()) {
		const QColor center = chosen.preview.pixelColor(chosen.preview.width() / 2, chosen.preview.height() / 2);
		check(center.green() > center.red() + 30 && center.green() > center.blue() + 30,
		      "Slip preview decodes the replacement video rather than the currently shown choice");
	} else {
		check(false, "Slip preview decodes the replacement video rather than the currently shown choice");
	}
	TlClip expected = slot;
	expected.clipOptions[1] = {green, 1234};
	check(current(window) == expected, "editing a hidden option changes only its file and source start, retaining the shared slot and look");
	check(window.timelineView_->model().tracks[1].clips == initial.tracks[1].clips,
	      "editing a hidden choice leaves detached linked audio untouched");
	check(window.histIndex_ == historyBeforeCancel + 1, "source and timestamp selection commit as one undo step");
	window.undo();
	check(window.timelineView_->model() == beforeCancel, "one Undo restores the original random option");
	window.redo();
	check(current(window) == expected, "one Redo restores the selected source and exact timestamp");
	QAction *chooseBlue = sourceAction(optionButton(window, "randomClipVideo", 1), blue);
	const auto noSlip = slip([&]() { if (chooseBlue) chooseBlue->trigger(); }, -1, true);
	check(noSlip.seen && noSlip.baseStart == noSlip.resultingStart && current(window).clipOptionAt(1).sourceId == blue,
	      "accepting a replacement with zero slip still changes the source video");
	const auto duplicate = slip([&]() { optionButton(window, "randomClipSlip", 2)->click(); }, 3500, true);
	check(duplicate.seen && current(window).clipOptionAt(2).srcStartMs == 3500 &&
	      current(window).clipOptionAt(0).srcStartMs == 1000 && current(window).clipShown == 0,
	      "two options using the same video keep independent timestamps without changing the shown option");
	bool timestampShown = false;
	for (auto *label : window.findChildren<QLabel *>(QStringLiteral("randomClipTime")))
		if (label->property("optionIndex").toInt() == 2)
			timestampShown = label->text().contains(QStringLiteral("3.500")) || label->text().contains(QStringLiteral("03.500"));
	check(timestampShown, "Inspector shows each option's precise selected source time");
	const auto shown = slip([&]() { optionButton(window, "randomClipSlip", 0)->click(); }, 2345, true);
	const TlClip edited = current(window);
	check(shown.seen && edited.sourceId == red && edited.srcStartMs == 2345 && edited.srcEndMs == 4345 &&
	      edited.outStartMs == 1000 && edited.outDurationMs() == 1000 && edited.speed == 2 && edited.clipShown == 0,
	      "slipping the shown option keeps output position, duration and speed while updating its live source window");
	const TlClip &slippedAudio = window.timelineView_->model().tracks[1].clips[0];
	check(slippedAudio.srcStartMs == 2345 && slippedAudio.srcEndMs == 4345 && slippedAudio.outStartMs == linked.outStartMs &&
	      slippedAudio.outDurationMs() == linked.outDurationMs(),
	      "slipping the shown source moves its matching detached audio by the same amount");
	const auto beforeBounds = window.timelineView_->model();
	const auto bounded = slip([&]() { optionButton(window, "randomClipSlip", 2)->click(); }, 999999, false);
	check(bounded.seen && bounded.resultingStart == 4000 && window.timelineView_->model() == beforeBounds,
	      "timestamp input clamps the entire source window within the video and Cancel discards the bounded change");
	window.timelineView_->setSourceThumbs(red, {}, 0);
	const auto withoutThumbnailDuration = slip([&]() { optionButton(window, "randomClipSlip", 0)->click(); }, 999999, false);
	check(withoutThumbnailDuration.seen && withoutThumbnailDuration.resultingStart == 4000 &&
	      window.timelineView_->model() == beforeBounds,
	      "shown option uses known source metadata bounds before asynchronous thumbnails publish their duration");
	TimelineModel constrained = beforeBounds;
	constrained.tracks[0].clips[0].srcStartMs = 0;
	constrained.tracks[0].clips[0].srcEndMs = 2000;
	constrained.tracks[0].clips[0].clipOptions[0] = {red, 0};
	constrained.tracks[1].clips[0].srcStartMs = 1000;
	constrained.tracks[1].clips[0].srcEndMs = 5500;
	install(window, constrained);
	window.timelineView_->setSourceThumbs(red, {}, 0);
	const auto partnerBound = slip([&]() { optionButton(window, "randomClipSlip", 0)->click(); }, 999999, false);
	check(partnerBound.seen && partnerBound.resultingStart == 500 &&
	      window.timelineView_->model() == constrained,
	      "a shown option starting at zero still honors its longer detached audio's source bounds before thumbnails arrive");
	install(window, beforeBounds);
	window.timelineView_->setSourceThumbs(red, {}, 6000);
	const auto variants = text_variations::slotsOf(window.timelineView_->model());
	const auto version = text_variations::applyCombination(window.timelineView_->model(), variants, {2});
	check(variants.size() == 1 && version.tracks[0].clips[0].sourceId == red &&
	      version.tracks[0].clips[0].srcStartMs == 3500 && version.tracks[0].clips[0].srcEndMs == 5500 &&
	      version.tracks[0].clips[0].outDurationMs() == 1000,
	      "export variations use each option's own timestamp for the shared slot duration");
	const QString project = temporary.filePath("random-options.harpiaproj");
	check(window.saveProjectTo(project, true).isEmpty(), "edited random source choices and timestamps save successfully");
	{
		VideoEditorWindow reopened{QString()};
		check(reopened.openProjectAt(project), "saved random options reopen successfully");
		const TlClip loaded = current(reopened);
		check(reopened.sourcePathForTest(loaded.clipOptionAt(0).sourceId) == redPath &&
		      reopened.sourcePathForTest(loaded.clipOptionAt(1).sourceId) == bluePath &&
		      loaded.clipOptionAt(0).srcStartMs == 2345 && loaded.clipOptionAt(1).srcStartMs == noSlip.resultingStart &&
		      loaded.clipOptionAt(2).srcStartMs == 3500 && loaded.speed == 2 && loaded.outDurationMs() == 1000,
		      "reopened options retain the selected videos, independent exact starts and shared duration");
	}
	optionButton(window, "randomClipVideo", 1)->click();
	check(current(window).clipShown == 1 && current(window).sourceId == blue,
	      "clicking the merged source name still previews that random option");
	optionButton(window, "randomClipVideo", 1)->click();
	check(optionButton(window, "randomClipVideo", 1)->isChecked(),
	      "clicking the shown option keeps its selected highlight");
	TimelineModel locked = window.timelineView_->model();
	locked.tracks[0].locked = true;
	install(window, locked);
	check(!optionButton(window, "randomClipVideo", 1)->isEnabled() &&
	      !optionButton(window, "randomClipSlip", 1)->isEnabled(), "locked tracks disable source and timestamp editing controls");
	const auto blocked = slip([&]() { window.editRandomClipOption(1, green); }, 1700, true);
	check(!blocked.seen && window.timelineView_->model() == locked, "direct option edits also respect a locked track");
	std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
	return failures ? 1 : 0;
}
