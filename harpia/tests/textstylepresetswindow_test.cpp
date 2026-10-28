// Built-in social styles through the real Inspector: applying a look changes
// every selected caption in one undo step, while custom styles and caption
// content retain their own identities.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TextStyleJson.hpp"
#include "editor/timeline/TextStylePresets.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

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
	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < milliseconds) {
		QApplication::processEvents();
		QThread::msleep(2);
	}
}

static QByteArray readFile(const QString &path)
{
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static TlClip caption(int index)
{
	TlClip clip;
	clip.type = TlClip::Type::Text;
	clip.srcStartMs = 100;
	clip.srcEndMs = 1400 + index * 100;
	clip.outStartMs = 200 + index * 1500;
	clip.speed = index == 0 ? 1.25 : 0.75;
	clip.posX = 0.3 + index * 0.1;
	clip.posY = 0.65;
	clip.scale = 0.8;
	clip.rotation = 7;
	clip.opacity = 0.9;
	clip.text.text = index == 0 ? QStringLiteral("Hello <color=#ff8800>world</color>\nKeep my spacing")
				 : QStringLiteral("A different caption");
	clip.text.variations = {QStringLiteral("Alternative one"), QStringLiteral("Alternative two\nSecond line")};
	clip.text.random = index == 1;
	clip.text.fontPx = 37 + index * 20;
	clip.text.color = index == 0 ? QColor(200, 30, 30) : QColor(20, 180, 40);
	clip.words = {{QStringLiteral("Hello"), 0, 200}, {QStringLiteral("world"), 200, 700}};
	clip.fadeInMs = 50;
	clip.fadeOutMs = 90;
	TlKeyframe key;
	key.tMs = 300;
	key.tf = clip.baseTransform();
	key.tf.posX = 0.6;
	key.curvedPath = true;
	key.handlesManual = true;
	key.outX = 0.15;
	clip.keys.append(key);
	return clip;
}

static TimelineModel fixture()
{
	TimelineModel model;
	TlTrack captions;
	captions.kind = TlTrack::Kind::Video;
	captions.clips = {caption(0), caption(1)};
	TlClip picture = caption(2);
	picture.type = TlClip::Type::Image;
	captions.clips.append(picture);
	model.tracks.append(captions);
	TlTrack locked;
	locked.kind = TlTrack::Kind::Video;
	locked.locked = true;
	locked.clips = {caption(3)};
	model.tracks.append(locked);
	return model;
}

static bool choose(QComboBox *combo, const QString &key)
{
	const int index = combo->findData(key);
	if (index < 0)
		return false;
	combo->setCurrentIndex(index);
	return QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, index));
}

static TimelineModel styled(TimelineModel model, const TlText &style, bool both = true)
{
	text_style_presets::applyStyle(style, model.tracks[0].clips[0].text);
	if (both)
		text_style_presets::applyStyle(style, model.tracks[0].clips[1].text);
	return model;
}

struct DialogResult {
	bool seen = false;
	bool timedOut = false;
};

static DialogResult runDialog(const std::function<void()> &action,
			      const std::function<bool(QWidget *)> &respond)
{
	DialogResult result;
	QTimer driver, watchdog;
	driver.setInterval(10);
	watchdog.setSingleShot(true);
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		QWidget *dialog = QApplication::activeModalWidget();
		if (dialog && respond(dialog))
			result.seen = true;
	});
	QObject::connect(&watchdog, &QTimer::timeout, [&]() {
		result.timedOut = true;
		if (auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
			input->reject();
		else if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
			message->reject();
	});
	driver.start();
	watchdog.start(4000);
	action();
	driver.stop();
	watchdog.stop();
	return result;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir settings;
	qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
	qputenv("XDG_DATA_HOME", (settings.path() + QStringLiteral("/data")).toUtf8());
	// The automation responds through QWidget buttons; native macOS sheets
	// are driven by the OS and do not expose those buttons to this test.
	QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
	QApplication app(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaTextStylesTest"));
	QCoreApplication::setApplicationName(QStringLiteral("Inspector"));
	const QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
			     QStringLiteral("/harpia/text-presets.json");
	QDir().mkpath(QFileInfo(path).absolutePath());
	TlText saved;
	saved.fontPx = 93;
	saved.color = QColor(0, 220, 220);
	saved.boxEnabled = true;
	saved.boxColor = QColor(40, 10, 70);
	QJsonObject seed;
	seed.insert(QStringLiteral("Instagram Classic"), textStyleToJson(saved));
	{
		QFile file(path);
		if (!file.open(QIODevice::WriteOnly)) {
			std::fprintf(stderr, "Could not create isolated style fixture.\n");
			return 2;
		}
		file.write(QJsonDocument(seed).toJson());
	}
	const QByteArray originalFile = readFile(path);

	VideoEditorWindow window{QString()};
	window.resize(1350, 850);
	window.show();
	for (auto *button : window.findChildren<QPushButton *>())
		if (button->text() == QStringLiteral("Full Editing"))
			button->click();
	settle(100);
	auto *timeline = window.findChild<TimelineView *>();
	auto *combo = window.findChild<QComboBox *>(QStringLiteral("textStylePreset"));
	auto *save = window.findChild<QPushButton *>(QStringLiteral("saveTextStylePreset"));
	auto *remove = window.findChild<QPushButton *>(QStringLiteral("deleteTextStylePreset"));
	check(timeline && combo && save && remove, "the real Inspector exposes the style picker and its actions");
	if (!timeline || !combo || !save || !remove)
		return 1;

	int builtins = 0;
	for (int index = 0; index < combo->count(); ++index)
		builtins += combo->itemData(index).toString().startsWith(QStringLiteral("builtin/"));
	check(builtins == 6, "all six built-in social styles are immediately available");
	check(combo->findData(QStringLiteral("saved/Instagram Classic")) >= 0 &&
	      combo->findText(QStringLiteral("Instagram Classic (Saved)")) >= 0,
	      "a saved style with a built-in name has its own distinct picker entry");
	check(!remove->isEnabled(), "the placeholder cannot be deleted");

	const TimelineModel before = fixture();
	timeline->setModelAndCommit(before);
	timeline->selectClip(0, 0);
	timeline->addToSelection(0, 1);
	timeline->addToSelection(0, 2); // a selected non-text clip must be ignored
	timeline->addToSelection(1, 0); // selected locked captions must be ignored
	settle(50);
	const auto *tiktok = text_style_presets::find(QStringLiteral("tiktok-classic"));
	const auto *instagramBox = text_style_presets::find(QStringLiteral("instagram-classic-box"));
	const auto *youtube = text_style_presets::find(QStringLiteral("youtube-bold"));
	check(tiktok && instagramBox && youtube, "the required social style identities resolve");
	if (!tiktok || !instagramBox || !youtube)
		return 1;

	check(choose(combo, QStringLiteral("builtin/tiktok-classic")), "TikTok Classic can be selected through the real picker");
	check(timeline->model() == styled(before, tiktok->style),
	      "the preset changes both editable captions while preserving content, timing, transforms and keys");
	check(timeline->model().tracks[0].clips[2] == before.tracks[0].clips[2] &&
	      timeline->model().tracks[1].clips[0] == before.tracks[1].clips[0],
	      "selected non-text clips and locked captions remain unchanged");
	check(combo->currentData().toString() == QStringLiteral("builtin/tiktok-classic"),
	      "Inspector refresh retains the chosen built-in identity");
	check(!remove->isEnabled(), "a built-in preset cannot be deleted");
	remove->click();
	check(readFile(path) == originalFile && combo->findData(QStringLiteral("builtin/tiktok-classic")) >= 0,
	      "using a built-in neither writes custom presets nor removes built-in choices");
	QMetaObject::invokeMethod(&window, "undo", Qt::DirectConnection);
	check(timeline->model() == before, "one Undo restores the complete selected caption set");

	choose(combo, QStringLiteral("builtin/instagram-classic-box"));
	choose(combo, QStringLiteral("builtin/tiktok-classic"));
	QMetaObject::invokeMethod(&window, "undo", Qt::DirectConnection);
	check(timeline->model() == styled(before, instagramBox->style),
	      "two rapid preset choices remain separate undo steps");
	QMetaObject::invokeMethod(&window, "undo", Qt::DirectConnection);
	check(timeline->model() == before, "a second Undo restores the original styles and caption data");

	timeline->selectClip(0, 0);
	choose(combo, QStringLiteral("saved/Instagram Classic"));
	check(timeline->model() == styled(before, saved, false),
	      "the same-named saved style applies its own appearance to a single caption");
	check(combo->currentData().toString() == QStringLiteral("saved/Instagram Classic") && remove->isEnabled(),
	      "the saved style retains its identity and can be deleted");
	check(readFile(path) == originalFile, "applying built-in and saved looks does not rewrite the custom style file");

	choose(combo, QStringLiteral("builtin/youtube-bold"));
	const DialogResult savedCopy = runDialog([&]() { save->click(); }, [](QWidget *widget) {
		auto *dialog = qobject_cast<QInputDialog *>(widget);
		if (!dialog || dialog->windowTitle() != QStringLiteral("Save text style"))
			return false;
		dialog->setTextValue(QStringLiteral("My YouTube Copy"));
		dialog->accept();
		return true;
	});
	check(savedCopy.seen && !savedCopy.timedOut, "Save opens the normal naming dialog for an edited or built-in style");
	const QJsonObject afterSave = QJsonDocument::fromJson(readFile(path)).object();
	check(afterSave.size() == 2 && afterSave.value(QStringLiteral("Instagram Classic")) == seed.value(QStringLiteral("Instagram Classic")) &&
	      afterSave.value(QStringLiteral("My YouTube Copy")).toObject() == textStyleToJson(youtube->style),
	      "saving a copy preserves old custom styles and stores only the new appearance");
	check(combo->currentData().toString() == QStringLiteral("saved/My YouTube Copy") && remove->isEnabled(),
	      "the saved copy becomes the active removable preset");
	const DialogResult deletedCopy = runDialog([&]() { remove->click(); }, [](QWidget *widget) {
		auto *dialog = qobject_cast<QMessageBox *>(widget);
		if (!dialog || dialog->windowTitle() != QStringLiteral("Delete style"))
			return false;
		if (auto *yes = dialog->button(QMessageBox::Yes))
			yes->click();
		else
			dialog->reject();
		return true;
	});
	check(deletedCopy.seen && !deletedCopy.timedOut, "deleting a saved copy asks for confirmation through the normal dialog");
	check(QJsonDocument::fromJson(readFile(path)).object() == seed &&
	      combo->findData(QStringLiteral("saved/My YouTube Copy")) < 0 &&
	      combo->findData(QStringLiteral("builtin/youtube-bold")) >= 0,
	      "deleting a custom copy preserves the original saved style and every built-in");
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
