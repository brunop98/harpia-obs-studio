// Social editing controls through the real window: caption animation changes
// only the chosen text clips, guides stay out of the project, and silence
// preview is a transaction which becomes one undo action only after Apply.
#include "editor/VideoEditorWindow.hpp"
#include "editor/EditorWidgets.hpp"
#include "editor/SilenceTrimDialog.hpp"
#include "editor/tts/TtsAudio.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <cmath>
#include <cstdio>
#include <functional>

using namespace harpia;

static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

static void settle(int milliseconds)
{
	QElapsedTimer clock;
	clock.start();
	while (clock.elapsed() < milliseconds) {
		QApplication::processEvents();
		QThread::msleep(2);
	}
}

static TlClip caption(int index)
{
	TlClip clip;
	clip.type = TlClip::Type::Text;
	clip.outStartMs = index * 1800;
	clip.srcEndMs = 1500;
	clip.posX = 0.4 + index * 0.05;
	clip.posY = 0.65;
	clip.scale = 0.85;
	clip.rotation = 4;
	clip.text.text = index == 0 ? QStringLiteral("Keep <color=#ff8800>this</color>\ncaption")
				 : QStringLiteral("Different words here");
	clip.text.variations = {QStringLiteral("An alternative"), QStringLiteral("Another\nversion")};
	clip.text.random = index == 1;
	clip.words = {{QStringLiteral("Keep"), 0, 300}, {QStringLiteral("this"), 400, 750},
		     {QStringLiteral("caption"), 900, 1400}};
	TlKeyframe key;
	key.tMs = 200;
	key.tf = clip.baseTransform();
	key.tf.posX = 0.7;
	clip.keys.append(key);
	ComponentInstance motion;
	motion.typeId = QStringLiteral("harpia.alwaysRotate");
	motion.instanceId = QStringLiteral("existing-motion-%1").arg(index);
	motion.props.insert(QStringLiteral("speed"), 12.0);
	clip.components.append(motion);
	if (index == 0) {
		ComponentInstance subtitle;
		subtitle.typeId = QStringLiteral("harpia.subtitle");
		subtitle.instanceId = QStringLiteral("existing-subtitle");
		subtitle.props.insert(QStringLiteral("mode"), 1);
		subtitle.props.insert(QStringLiteral("activeColor"), QColor(0, 220, 220));
		clip.components.append(subtitle);
		ComponentInstance pop;
		pop.typeId = QStringLiteral("harpia.captionPop");
		pop.instanceId = QStringLiteral("existing-pop");
		pop.enabled = false;
		pop.props.insert(QStringLiteral("strength"), 0.22);
		pop.props.insert(QStringLiteral("durationMs"), 180.0);
		clip.components.append(pop);
	}
	return clip;
}

static TimelineModel fixture()
{
	TimelineModel model;
	TlTrack editable;
	editable.clips = {caption(0), caption(1)};
	TlClip image = caption(2);
	image.type = TlClip::Type::Image;
	editable.clips.append(image);
	model.tracks.append(editable);
	TlTrack locked;
	locked.locked = true;
	locked.clips = {caption(3)};
	model.tracks.append(locked);
	return model;
}

static const ComponentInstance *component(const TlClip &clip, const QString &type)
{
	for (const auto &entry : clip.components)
		if (entry.typeId == type)
			return &entry;
	return nullptr;
}

static bool captionDataUnchanged(const TimelineModel &before, TimelineModel after)
{
	if (after.tracks.size() != before.tracks.size())
		return false;
	for (int index : {0, 1}) {
		if (after.tracks[0].clips.size() <= index)
			return false;
		after.tracks[0].clips[index].components = before.tracks[0].clips[index].components;
	}
	return after == before;
}

static bool allCaptionsHaveMode(const TimelineModel &model, int mode)
{
	for (int index : {0, 1}) {
		const auto *entry = component(model.tracks[0].clips[index], QStringLiteral("harpia.subtitle"));
		if (!entry || !entry->enabled || entry->props.value(QStringLiteral("mode")).toInt() != mode)
			return false;
	}
	return true;
}

static bool driveColor(QPushButton *button, const QColor &colour, bool accept)
{
	bool seen = false;
	QTimer driver, watchdog;
	driver.setInterval(10);
	watchdog.setSingleShot(true);
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		if (auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget())) {
			seen = true;
			dialog->setCurrentColor(colour);
			accept ? dialog->accept() : dialog->reject();
		}
	});
	QObject::connect(&watchdog, &QTimer::timeout, [&]() {
		if (auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget()))
			dialog->reject();
	});
	driver.start();
	watchdog.start(4000);
	button->click();
	return seen;
}

struct SilenceReview {
	bool opened = false;
	bool previewed = false;
	bool timedOut = false;
	bool unexpectedMessage = false;
};

static SilenceReview reviewSilence(VideoEditorWindow &window, TimelineView *timeline, QAction *action,
				   bool apply)
{
	SilenceReview result;
	const qint64 originalDuration = timeline->model().durationMs();
	QTimer driver, watchdog;
	driver.setInterval(10);
	watchdog.setSingleShot(true);
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
			result.unexpectedMessage = true;
			box->reject();
			return;
		}
		auto *dialog = window.findChild<SilenceTrimDialog *>();
		if (!dialog || !dialog->isVisible())
			return;
		result.opened = true;
		auto *preview = dialog->findChild<QPushButton *>(QStringLiteral("silencePreview"));
		auto *save = dialog->findChild<QPushButton *>(QStringLiteral("silenceApply"));
		if (!preview || !save || !preview->isEnabled() || !save->isEnabled())
			return;
		if (apply) {
			save->click();
		} else if (!result.previewed) {
			preview->click();
			result.previewed = timeline->model().durationMs() < originalDuration;
			QTimer::singleShot(20, dialog, &SilenceTrimDialog::reject);
		}
	});
	QObject::connect(&watchdog, &QTimer::timeout, [&]() {
		result.timedOut = true;
		if (auto *dialog = window.findChild<SilenceTrimDialog *>())
			dialog->reject();
		if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
			box->reject();
	});
	driver.start();
	watchdog.start(8000);
	action->trigger();
	return result;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir work;
	qputenv("XDG_CONFIG_HOME", (work.path() + QStringLiteral("/config")).toUtf8());
	qputenv("XDG_DATA_HOME", (work.path() + QStringLiteral("/data")).toUtf8());
	QApplication app(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaSocialFeaturesTest"));
	QCoreApplication::setApplicationName(QStringLiteral("Inspector"));
	QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder")).remove(QStringLiteral("editor/socialSafeArea"));

	VideoEditorWindow window{QString()};
	window.resize(1350, 850);
	window.show();
	for (auto *button : window.findChildren<QPushButton *>())
		if (button->text() == QStringLiteral("Full Editing"))
			button->click();
	settle(80);
	auto *timeline = window.findChild<TimelineView *>();
	auto *canvas = window.findChild<PreviewCanvas *>();
	auto *animation = window.findChild<QComboBox *>(QStringLiteral("captionAnimation"));
	auto *pop = window.findChild<QCheckBox *>(QStringLiteral("captionWordPop"));
	auto *highlight = window.findChild<QPushButton *>(QStringLiteral("captionHighlightColor"));
	check(timeline && canvas && animation && pop && highlight, "the Inspector exposes caption animation, word pop and highlight colour");
	if (!timeline || !canvas || !animation || !pop || !highlight)
		return 1;
	check(animation->count() == 5, "the caption picker includes reveal and both word-highlighting modes");

	const TimelineModel before = fixture();
	timeline->setModelAndCommit(before);
	timeline->selectClip(0, 0);
	timeline->addToSelection(0, 1);
	timeline->addToSelection(0, 2);
	timeline->addToSelection(1, 0);
	settle(40);
	animation->setCurrentIndex(3);
	check(allCaptionsHaveMode(timeline->model(), 3), "highlight-current applies to both selected editable captions");
	check(captionDataUnchanged(before, timeline->model()), "animation preserves words, alternatives, timing, transforms and keyframes");
	check(timeline->model().tracks[0].clips[2] == before.tracks[0].clips[2] &&
	      timeline->model().tracks[1].clips[0] == before.tracks[1].clips[0],
	      "selected images and locked captions are not edited");
	check(component(timeline->model().tracks[0].clips[0], QStringLiteral("harpia.alwaysRotate")) &&
	      *component(timeline->model().tracks[0].clips[0], QStringLiteral("harpia.alwaysRotate")) == before.tracks[0].clips[0].components[0],
	      "an unrelated existing component retains its properties and identity");
	check(highlight->isEnabled(), "word highlighting enables the highlight-colour action");
	QMetaObject::invokeMethod(&window, "undo", Qt::DirectConnection);
	check(timeline->model() == before, "one Undo restores all captions after a batch animation choice");
	animation->setCurrentIndex(4);
	check(allCaptionsHaveMode(timeline->model(), 4), "highlight-spoken is available through the same batch control");
	pop->setChecked(true);
	const TimelineModel popped = timeline->model();
	const auto *firstPop = component(popped.tracks[0].clips[0], QStringLiteral("harpia.captionPop"));
	const auto *secondPop = component(popped.tracks[0].clips[1], QStringLiteral("harpia.captionPop"));
	check(firstPop && firstPop->enabled && secondPop && secondPop->enabled,
	      "word pop enables or creates one animation for each selected caption");
	check(firstPop && firstPop->instanceId == QStringLiteral("existing-pop") &&
	      firstPop->props.value(QStringLiteral("strength")).toDouble() == 0.22 &&
	      firstPop->props.value(QStringLiteral("durationMs")).toInt() == 180,
	      "enabling a previously configured pop keeps its strength, duration and identity");
	check(captionDataUnchanged(before, popped), "adding a pop leaves every stored caption and its layout untouched");
	pop->setChecked(false);
	const auto *disabled = component(timeline->model().tracks[0].clips[0], QStringLiteral("harpia.captionPop"));
	check(disabled && !disabled->enabled && disabled->props == firstPop->props,
	      "turning pop off preserves its editable settings");
	QMetaObject::invokeMethod(&window, "undo", Qt::DirectConnection);
	check(timeline->model() == popped, "disabling pop is one reversible batch action");

	const TimelineModel beforeColour = timeline->model();
	check(driveColor(highlight, QColor(240, 0, 200), false), "the highlight colour opens the normal colour picker");
	check(timeline->model() == beforeColour, "canceling the colour picker changes no captions");
	const QColor chosen(240, 20, 180);
	check(driveColor(highlight, chosen, true), "a highlight colour can be accepted");
	bool sameColour = true;
	for (int index : {0, 1}) {
		const auto *entry = component(timeline->model().tracks[0].clips[index], QStringLiteral("harpia.subtitle"));
		sameColour = sameColour && entry && entry->props.value(QStringLiteral("activeColor")).value<QColor>() == chosen;
	}
	check(sameColour, "the accepted colour applies to every selected caption");
	QMetaObject::invokeMethod(&window, "undo", Qt::DirectConnection);
	check(timeline->model() == beforeColour, "one Undo restores different previous highlight colours");

	std::printf("\n-- preview-only safe-area menu --\n");
	const TimelineModel beforeGuides = timeline->model();
	settle(100);
	const QImage cleanFrame = canvas->currentFrame();
	const QStringList objects{QStringLiteral("socialSafeAreaOff"), QStringLiteral("socialSafeAreaTikTok"),
		QStringLiteral("socialSafeAreaReels"), QStringLiteral("socialSafeAreaShorts")};
	bool guideActions = true;
	for (int index = 0; index < objects.size(); ++index) {
		auto *action = window.findChild<QAction *>(objects[index]);
		guideActions = guideActions && action;
		if (!action)
			continue;
		action->trigger();
		check(canvas->socialSafeArea() == SocialSafeArea(index) && action->isChecked(),
		      "the View menu activates its matching platform guide");
	}
	check(guideActions, "all platform safe-area actions are present");
	check(timeline->model() == beforeGuides && canvas->currentFrame() == cleanFrame,
	      "guide selection changes neither the timeline nor the clean rendered frame");
	check(QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"))
	      .value(QStringLiteral("editor/socialSafeArea")).toInt() == 3,
	      "the chosen guide is remembered as a view preference");
	{
		VideoEditorWindow reopened{QString()};
		auto *secondCanvas = reopened.findChild<PreviewCanvas *>();
		check(secondCanvas && secondCanvas->socialSafeArea() == SocialSafeArea::YouTubeShorts,
		      "a new editor restores the saved guide without modifying a project");
	}
	if (auto *off = window.findChild<QAction *>(QStringLiteral("socialSafeAreaOff")))
		off->trigger();
	check(canvas->socialSafeArea() == SocialSafeArea::Off, "Off removes the preview guides");

	std::printf("\n-- silence review preview, cancel, apply and undo --\n");
	tts_audio::Pcm speech;
	speech.samples.resize(48000 * 3 * 2);
	for (int frame = 0; frame < 48000 * 3; ++frame) {
		const bool quiet = frame >= 48000 && frame < 96000;
		const qint16 sample = quiet ? 0 : qint16(9000 * std::sin(frame * 2.0 * 3.141592653589793 * 220 / 48000));
		speech.samples[2 * frame] = speech.samples[2 * frame + 1] = sample;
	}
	const QString wav = work.filePath(QStringLiteral("speech.wav"));
	check(tts_audio::writeWav16(wav, speech), "a real narration fixture contains a one-second pause");
	timeline->setModelAndCommit(TimelineModel{});
	timeline->setPlayhead(0);
	auto *mime = new QMimeData;
	mime->setUrls({QUrl::fromLocalFile(wav)});
	QApplication::clipboard()->setMimeData(mime);
	check(QMetaObject::invokeMethod(&window, "pasteSystemClipboardForTest", Qt::DirectConnection),
	      "the normal paste action imports an audio file from the clipboard");
	settle(80);
	const TimelineModel audioBefore = timeline->model();
	const TlSelection selectionBefore = timeline->selectionState();
	const qint64 playheadBefore = timeline->playhead();
	check(!audioBefore.isEmpty() && audioBefore.durationMs() >= 2900, "the narration reaches the editor through its real audio import path");
	auto *trim = window.findChild<QAction *>(QStringLiteral("trimSilenceAction"));
	check(trim && trim->isEnabled(), "Trim silence is available in Full Editing's Tools menu");
	if (trim) {
		const SilenceReview canceled = reviewSilence(window, timeline, trim, false);
		check(canceled.opened && canceled.previewed && !canceled.timedOut && !canceled.unexpectedMessage,
		      "the silence dialog previews the shorter timeline before applying any edit");
		check(timeline->model() == audioBefore && timeline->selectionState() == selectionBefore &&
		      timeline->playhead() == playheadBefore,
		      "canceling a preview restores the original timeline, selection and playhead");
		const SilenceReview applied = reviewSilence(window, timeline, trim, true);
		check(applied.opened && !applied.timedOut && !applied.unexpectedMessage &&
		      timeline->model().durationMs() < audioBefore.durationMs(),
		      "Apply commits the reviewed silence cuts to the editor");
		QMetaObject::invokeMethod(&window, "undo", Qt::DirectConnection);
		check(timeline->model() == audioBefore, "one Undo restores the complete pre-trim timeline");
	}
	QApplication::clipboard()->clear();
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
