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
#include "editor/RandomClipCompareDialog.hpp"
#include "editor/InspectorSection.hpp"
#include "editor/EditorWidgets.hpp"
#include "editor/component/ComponentPanel.hpp"
#include "editor/timeline/ClipVariations.hpp"
#include "editor/timeline/TextVariations.hpp"
#include "editor/timeline/TimelineCompositor.hpp"
#include "editor/timeline/FrameSignature.hpp"

#include <cmath>
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
void settle()
{
	QApplication::processEvents();
	QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
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

void checkRenderedPositions()
{
	class Frames final : public TimelineCompositor::FrameProvider {
	public:
		QImage frameFor(int id, qint64) override
		{
			QImage frame(64, 48, QImage::Format_RGBA8888);
			frame.fill(id == 0 ? Qt::red : Qt::blue);
			return frame;
		}
		QSize sourceSize(int) override { return {64, 48}; }
	} frames;
	const auto bounds = [](const QImage &image) {
		QRect result;
		for (int y = 0; y < image.height(); ++y) {
			for (int x = 0; x < image.width(); ++x) {
				const QColor colour = image.pixelColor(x, y);
				if (colour.red() > 100 || colour.blue() > 100) {
					result |= QRect(x, y, 1, 1);
				}
			}
		}
		return result;
	};
	TlClip clip;
	clip.srcEndMs = 1000;
	clip.scale = 0.2;
	clip.clipOptions = {{0, 0, -0.25, 0.0}, {1, 0, 0.25, -0.25}};
	TlTrack track;
	track.clips = {clip};
	TimelineModel model;
	model.tracks = {track};
	const QSize canvas(160, 120);
	const auto choices = text_variations::slotsOf(model);
	const auto first = text_variations::applyCombination(model, choices, {0});
	const auto second = text_variations::applyCombination(model, choices, {1});
	const QImage previewA = TimelineCompositor::compose(first, 300, canvas, frames);
	const QImage previewB = TimelineCompositor::compose(second, 300, canvas, frames);
	const QImage exportB = TimelineCompositor::composeAtTime(second, 300.0, canvas, frames);
	const QRect a = bounds(previewA), b = bounds(previewB);
	check(!a.isEmpty() && !b.isEmpty() && b.x() - a.x() == 80 && b.y() - a.y() == -30 && a.size() == b.size(),
	      "real composited frames place each variation at its independent X/Y offset with identical size");
	check(previewB == exportB && previewA != previewB,
	      "preview and export clocks render the same independently positioned variation");
	const auto reduced = TimelineCompositor::compose(second, 300, QSize(80, 60), frames, nullptr, 30.0, canvas);
	const QRect reducedBounds = bounds(reduced);
	check(std::abs(reducedBounds.x() * 2 - b.x()) <= 1 && std::abs(reducedBounds.y() * 2 - b.y()) <= 1,
	      "option position is consistent between reduced preview and full export resolution");
	check(timelineFrameSignature(first, 300, canvas, canvas, 0) !=
		      timelineFrameSignature(second, 300, canvas, canvas, 0),
	      "preview frame caching distinguishes per-option placement");
	TimelineModel moved = first;
	moved.tracks[0].clips[0].clipOptions[0].offsetX += 0.1;
	check(timelineFrameSignature(first, 300, canvas, canvas, 0) !=
		      timelineFrameSignature(moved, 300, canvas, canvas, 0),
	      "editing the position invalidates the previously cached frame at the same timestamp");
	const TlClip serialized = second.tracks[0].clips[0];
	QJsonObject json = clipToJson(serialized);
	check(clipFromJson(json) == serialized, "project JSON round trips all option positions");
	QJsonArray options = json.value(QStringLiteral("clipOptions")).toArray();
	for (int i = 0; i < options.size(); ++i) {
		QJsonObject option = options[i].toObject();
		option.remove(QStringLiteral("offsetX"));
		option.remove(QStringLiteral("offsetY"));
		options[i] = option;
	}
	json[QStringLiteral("clipOptions")] = options;
	const TlClip legacy = clipFromJson(json);
	check(legacy.optionPositionOffset().isNull() && legacy.clipOptions[0].offsetX == 0.0 &&
	      legacy.posX == serialized.posX && legacy.posY == serialized.posY,
	      "older projects default to zero offsets and keep their original framing");
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
	checkRenderedPositions();
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
	bool comparisonSeen = false, comparisonScoped = false;
	QTimer closeComparison;
	QObject::connect(&closeComparison, &QTimer::timeout, [&]() {
		if (auto *comparison = dynamic_cast<RandomClipCompareDialog *>(QApplication::activeModalWidget())) {
			comparisonSeen = comparison->optionCount() == 3 && comparison->isPlaying();
			comparisonScoped = comparison->parentWidget() == &window &&
					   comparison->windowModality() == Qt::WindowModal;
			comparison->reject();
		}
	});
	const auto beforeComparison = window.timelineView_->model();
	const int historyBeforeComparison = window.histIndex_;
	auto *compare = window.findChild<QPushButton *>(QStringLiteral("compareRandomClipButton"));
	check(compare && compare->isEnabled(),
	      "the Random Clip component offers Compare variations for multiple options");
	closeComparison.start(10);
	if (compare) {
		compare->click();
	}
	closeComparison.stop();
	check(comparisonSeen && comparisonScoped,
	      "Compare opens an editor-scoped grid that starts all options together");
	check(window.timelineView_->model() == beforeComparison && window.histIndex_ == historyBeforeComparison,
	      "opening and closing comparison leaves the project and undo history unchanged");
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
	check(thumbs.size() == 3 && thumbs[0]->icon().pixmap(62, 34).toImage().pixelColor(20, 16) == QColor(Qt::red) &&
		      thumbs[1]->icon().pixmap(62, 34).toImage().pixelColor(20, 16) == QColor(Qt::blue) &&
		      thumbs[2]->icon().pixmap(62, 34).toImage().pixelColor(20, 16) == QColor(Qt::green),
	      "cached thumbnails follow each option's own source and timestamp, including duplicate sources");
	const QIcon redIcon = sourceAction(optionButton(window, "randomClipVideo", 1), red)->icon();
	const QIcon blueIcon = sourceAction(optionButton(window, "randomClipVideo", 1), blue)->icon();
	check(!redIcon.isNull() && !blueIcon.isNull() &&
		      redIcon.pixmap(48, 48).toImage().pixelColor(20, 12) == QColor(Qt::red) &&
		      blueIcon.pixmap(48, 48).toImage().pixelColor(20, 12) == QColor(Qt::blue),
	      "source dropdown actions show distinct video thumbnails alongside their names");
	window.sourceById(blue)->thumbCache = {greenFrame};
	window.refreshRandomClipSourceIcons(blue);
	const QIcon updatedIcon = picker->menu()->actions()[1]->icon();
	check(updatedIcon.pixmap(48, 48).toImage().pixelColor(20, 12) == QColor(Qt::green),
	      "an already populated dropdown updates when asynchronous source thumbnails arrive");
	picker->menu()->popup(QPoint(100, 100));
	settle();
	check(picker->menu()->actionGeometry(picker->menu()->actions().front()).height() >= 36,
	      "the opened source dropdown reserves enough row height for recognizable thumbnails");
	const QStringList arguments = QCoreApplication::arguments();
	const int snapshot = arguments.indexOf(QStringLiteral("--snapshot"));
	if (snapshot >= 0 && snapshot + 1 < arguments.size())
		check(picker->menu()->grab().save(arguments[snapshot + 1]), "source dropdown screenshot saves successfully");
	picker->menu()->close();
	settle();
	emit window.randomClipAdd_->menu()->aboutToShow();
	check(!window.randomClipAdd_->menu()->actions().front()->icon().isNull(),
	      "the Add clip dropdown also shows source thumbnails");
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
	const auto near = [](double a, double b) {
		return std::abs(a - b) < 1e-9;
	};
	const TimelineModel beforePosition = window.timelineView_->model();
	optionButton(window, "randomClipPosition", 1)->click();
	QPointer<QMenu> positionMenu = window.findChild<QMenu *>(QStringLiteral("randomClipPositionMenu"));
	auto *offsetX = positionMenu ? positionMenu->findChild<QDoubleSpinBox *>(QStringLiteral("randomClipOffsetX"))
				     : nullptr;
	auto *offsetY = positionMenu ? positionMenu->findChild<QDoubleSpinBox *>(QStringLiteral("randomClipOffsetY"))
				     : nullptr;
	check(offsetX && offsetY, "each choice exposes its own X and Y position popup");
	if (!offsetX || !offsetY) {
		return 1;
	}
	offsetX->setValue(12.5);
	offsetY->setValue(-7.5);
	positionMenu->close();
	settle();
	const TlClip positioned = current(window);
	check(positioned.clipShown == 0 && positioned.clipOptions[1].offsetX == 0.125 &&
		      positioned.clipOptions[1].offsetY == -0.075 && positioned.clipOptions[0].offsetX == 0 &&
		      positioned.keys == slot.keys && positioned.posX == slot.posX &&
		      window.timelineView_->model().tracks[1] == beforePosition.tracks[1],
	      "positioning a hidden option leaves the current preview, shared keys and linked audio unchanged");
	window.undo();
	check(window.timelineView_->model() == beforePosition, "Undo restores the independent position edit");
	window.redo();
	check(current(window) == positioned, "Redo restores the option's X and Y offsets");
	optionButton(window, "randomClipPosition", 1)->click();
	positionMenu = window.findChild<QMenu *>(QStringLiteral("randomClipPositionMenu"));
	if (positionMenu) {
		positionMenu->findChild<QPushButton *>(QStringLiteral("resetRandomClipPosition"))->click();
		positionMenu->close();
	}
	settle();
	check(current(window).clipOptions[1].offsetX == 0 && current(window).clipOptions[1].offsetY == 0 &&
	      current(window).clipShown == 0,
	      "Reset position clears only the chosen option's offsets");
	window.undo();
	check(current(window) == positioned, "resetting an option position is undoable");
	window.timelineView_->setPlayhead(1300);
	window.autoKeyframe_ = true;
	window.onPreviewTransformDrag(0.125, -0.05);
	TlClip dragged = current(window);
	check(near(dragged.clipOptions[0].offsetX, 0.125) && near(dragged.clipOptions[0].offsetY, -0.05) &&
		      dragged.clipOptions[1] == positioned.clipOptions[1] && dragged.keys == positioned.keys &&
		      dragged.posX == positioned.posX,
	      "preview dragging moves only the shown option even with auto-key enabled");
	window.componentPanel_->pinnedEdited(QStringLiteral("harpia.transform"), QStringLiteral("posX"), 0.7);
	dragged = current(window);
	check(near(dragged.positionedTransformAt(1300).posX, 0.7) && dragged.keys == positioned.keys &&
		      dragged.clipOptions[1] == positioned.clipOptions[1],
	      "the main Position inspector also edits the shown option without altering shared animation");
	const auto offsets = dragged.clipOptions;
	window.componentPanel_->pinnedEdited(QStringLiteral("harpia.transform"), QStringLiteral("scale"), 1.25);
	check(current(window).clipOptions == offsets,
	      "editing shared scale never bakes an option offset into every choice");
	window.commitSnapshot();
	const auto savedOffsets = current(window).clipOptions;
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
		check(loaded.clipOptions == savedOffsets,
		      "saving and reopening preserves every independent position offset");
	}
	optionButton(window, "randomClipVideo", 1)->click();
	check(current(window).clipShown == 1 && current(window).sourceId == blue,
	      "clicking the merged source name still previews that random option");
	optionButton(window, "randomClipVideo", 1)->click();
	check(optionButton(window, "randomClipVideo", 1)->isChecked(),
	      "clicking the shown option keeps its selected highlight");
	const TimelineModel beforeThumbClick = window.timelineView_->model();
	TimelineModel expectedThumbClick = beforeThumbClick;
	clip_variations::show(expectedThumbClick.tracks[0].clips[0], 0);
	QTest::mouseClick(optionButton(window, "randomClipThumbnail", 0), Qt::LeftButton);
	check(window.timelineView_->model() == expectedThumbClick,
	      "clicking a thumbnail selects that footage in the full project without changing other tracks or options");
	const auto previewColour = [&]() {
		const auto &image = window.canvas_->currentFrame();
		if (image.isNull()) {
			return QColor();
		}
		const auto pose = current(window).positionedTransformAt(window.timelinePlayheadMs());
		return image.pixelColor(std::clamp(int(pose.posX * image.width()), 0, image.width() - 1),
					std::clamp(int(pose.posY * image.height()), 0, image.height() - 1));
	};
	check(QTest::qWaitFor([&]() { return previewColour().red() > 180 && previewColour().blue() < 50; }, 2000),
	      "the main timeline renders the footage selected through its thumbnail");
	QTest::mouseClick(optionButton(window, "randomClipThumbnail", 1), Qt::LeftButton);
	check(current(window).clipShown == 1 &&
	      QTest::qWaitFor([&]() { return previewColour().blue() > 180 && previewColour().red() < 50; }, 2000) &&
		      optionButton(window, "randomClipThumbnail", 1)->isChecked() &&
		      !optionButton(window, "randomClipThumbnail", 0)->isChecked() &&
		      optionButton(window, "randomClipVideo", 1)->isChecked(),
	      "another thumbnail switches the rendered video and synchronizes both selection highlights");
	QTest::mouseClick(optionButton(window, "randomClipThumbnail", 1), Qt::LeftButton);
	check(current(window).clipShown == 1 && optionButton(window, "randomClipThumbnail", 1)->isChecked(),
	      "clicking the current thumbnail keeps its option selected");
	QTest::keyClick(optionButton(window, "randomClipThumbnail", 0), Qt::Key_Space);
	check(current(window).clipShown == 0 && optionButton(window, "randomClipThumbnail", 0)->isChecked(),
	      "thumbnail choices also support keyboard activation");
	optionButton(window, "randomClipThumbnail", 1)->click();
	TimelineModel locked = window.timelineView_->model();
	locked.tracks[0].locked = true;
	install(window, locked);
	check(!optionButton(window, "randomClipVideo", 1)->isEnabled() &&
		      !optionButton(window, "randomClipThumbnail", 1)->isEnabled() &&
		      !optionButton(window, "randomClipSlip", 1)->isEnabled() &&
		      !optionButton(window, "randomClipPosition", 1)->isEnabled(),
	      "locked tracks disable source, timestamp and independent position controls");
	QTest::mouseClick(optionButton(window, "randomClipThumbnail", 0), Qt::LeftButton);
	check(window.timelineView_->model() == locked, "thumbnail selection respects the track lock");
	window.showRandomClipPositionMenu(1, QPoint(100, 100));
	settle();
	check(window.timelineView_->model() == locked &&
		      !window.findChild<QMenu *>(QStringLiteral("randomClipPositionMenu")),
	      "direct position editing also respects a locked track");
	const auto blocked = slip([&]() { window.editRandomClipOption(1, green); }, 1700, true);
	check(!blocked.seen && window.timelineView_->model() == locked, "direct option edits also respect a locked track");
	std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
	return failures ? 1 : 0;
}
