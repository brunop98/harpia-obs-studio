// Real recorder UI and post-stop publishing, with isolated settings and media.
// The backend/keyframe continuity fixture lives in autosplitrecording_test.cpp.
#include <QtWidgets>
#include <QTest>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#define private public
#include "ui/MainWindow.hpp"
#undef private
#include "ui/PresetEditorDialog.hpp"
#include "core/ObsContext.hpp"
#include "core/RecordingParts.hpp"
#include "model/PresetStore.hpp"
#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *message)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}
QByteArray read(const QString &path)
{
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool write(const QString &path, const QByteArray &contents)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}
bool waitFor(const std::function<bool()> &done)
{
	QElapsedTimer clock;
	clock.start();
	while (!done() && clock.elapsed() < 10000)
		QTest::qWait(10);
	return done();
}
void choose(MainWindow &window, bool enabled, int mode, int minutes, int size, bool accept = true)
{
	bool seen = false;
	bool answered = false;
	bool timedOut = false;
	QTimer driver, watchdog;
	driver.setInterval(10);
	watchdog.setSingleShot(true);
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		if (auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
			if (!accept && question->standardButtons().testFlag(QMessageBox::Discard))
				question->button(QMessageBox::Discard)->click();
			return;
		}
		auto *dialog = qobject_cast<PresetEditorDialog *>(QApplication::activeModalWidget());
		if (!dialog || answered)
			return;
		seen = true;
		answered = true;
		auto *checkBox = dialog->findChild<QCheckBox *>(QStringLiteral("autoSplitEnabled"));
		auto *modeCombo = dialog->findChild<QComboBox *>(QStringLiteral("autoSplitMode"));
		auto *minutesSpin = dialog->findChild<QSpinBox *>(QStringLiteral("autoSplitMinutes"));
		auto *sizeSpin = dialog->findChild<QSpinBox *>(QStringLiteral("autoSplitSizeMB"));
		auto *buttons = dialog->findChild<QDialogButtonBox *>();
		check(checkBox && modeCombo && minutesSpin && sizeSpin && buttons,
		      "the profile Recording page exposes Auto Split and both limits");
		if (!checkBox || !modeCombo || !minutesSpin || !sizeSpin || !buttons) {
			dialog->done(QDialog::Rejected);
			return;
		}
		check(checkBox->isVisible() && checkBox->isEnabled(), "Auto Split is available in a video profile");
		check(window.hotkeysSuspended_, "recording hotkeys stay suspended while editing the profile");
		checkBox->setChecked(enabled);
		modeCombo->setCurrentIndex(mode);
		minutesSpin->setValue(minutes);
		sizeSpin->setValue(size);
		if (!accept) {
			QTimer::singleShot(0, []() {
				auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
				if (question && question->standardButtons().testFlag(QMessageBox::Discard))
					question->button(QMessageBox::Discard)->click();
			});
		}
		buttons->button(accept ? QDialogButtonBox::Save : QDialogButtonBox::Cancel)->click();
	});
	QObject::connect(&watchdog, &QTimer::timeout, [&]() {
		timedOut = true;
		if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
			dialog->done(QDialog::Rejected);
		for (auto *dialog : window.findChildren<PresetEditorDialog *>())
			dialog->done(QDialog::Rejected);
	});
	driver.start();
	watchdog.start(4000);
	window.editActivePreset(QStringLiteral("Recording"));
	driver.stop();
	watchdog.stop();
	check(seen && !timedOut, "Auto Split is edited through the normal profile window");
	check(!window.hotkeysSuspended_, "recording hotkeys resume after saving or canceling profile settings");
}
}

int main(int argc, char **argv)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
	// This optional recorder fixture redirects the native INI store on Linux.
	// setPath cannot isolate the Windows registry or macOS CFPreferences.
	std::fprintf(stderr, "This fixture requires a file-based native QSettings store.\n");
	return 77;
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QTemporaryDir temporary;
	check(temporary.isValid(), "private test output directory exists");
	QStandardPaths::setTestModeEnabled(true);
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
	QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, temporary.path());
	QSettings::setPath(QSettings::NativeFormat, QSettings::SystemScope, temporary.path() + QStringLiteral("/system"));
	qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
	ObsContext obs;
	const bool started = obs.startup();
	check(started, "real OBS backend initializes for the recorder UI");
	if (!started)
		return 1;
	QDir().mkpath(temporary.path() + QStringLiteral("/config"));
	PresetStore presets((temporary.path() + QStringLiteral("/config")).toStdString());
	const QString output = temporary.path() + QStringLiteral("/Recordings");
	QDir().mkpath(output);
	presets.load(output.toStdString());
	{
		MainWindow window(obs, presets, output);
		for (QTimer *timer : window.findChildren<QTimer *>())
			timer->stop();
		window.show();
		QCoreApplication::processEvents();
		if (argc > 1)
			check(window.grab().save(QString::fromLocal8Bit(argv[1])), "recorder window snapshot is saved");
		check(!window.findChild<QWidget *>(QStringLiteral("autoSplitButton")),
		      "the main recording view has no Auto Split control");
		check(!window.recordingSplitSettings().enabled, "a new profile does not split recordings by default");
		const std::string firstId = window.activePresetId_;
		choose(window, true, 1, 5, 49);
		check(window.activePreset().autoSplit && window.activePreset().autoSplitMode == 1 &&
		      window.activePreset().autoSplitMinutes == 5 && window.activePreset().autoSplitSizeMB == 49,
		      "saving the profile retains its selected split mode and both limits");
		check(window.recordingSplitSettings().enabled &&
		      window.recordingSplitSettings().mode == AutoSplitSettings::Mode::Size &&
		      window.recordingSplitSettings().sizeMB == 49,
		      "recording uses the active profile's file-size split limit");
		const Preset beforeCancel = window.activePreset();
		choose(window, false, 0, 7, 123, false);
		check(window.activePreset() == beforeCancel,
		      "Cancel discards split edits together with the rest of the profile");
		PresetStore reopened(presets.configDir());
		reopened.load(output.toStdString());
		const Preset *persisted = reopened.find(firstId);
		check(persisted && persisted->autoSplit && persisted->autoSplitMode == 1 &&
		      persisted->autoSplitMinutes == 5 && persisted->autoSplitSizeMB == 49,
		      "profile split limits survive reloading the preset store");

		Preset secondProfile = Preset::makeDefault(output.toStdString());
		secondProfile.id = "second-profile";
		secondProfile.name = "Unsplit profile";
		presets.upsert(secondProfile);
		window.reloadPresetCombo();
		window.presetCombo_->setCurrentIndex(window.presetCombo_->findData(QString::fromStdString(secondProfile.id)));
		check(window.activePresetId_ == secondProfile.id && !window.recordingSplitSettings().enabled,
		      "switching to another profile does not inherit the first profile's split setting");
		choose(window, true, 0, 2, 500);
		check(window.recordingSplitSettings().enabled &&
		      window.recordingSplitSettings().mode == AutoSplitSettings::Mode::Time &&
		      window.recordingSplitSettings().minutes == 2 &&
		      presets.find(firstId)->autoSplitSizeMB == 49,
		      "another profile can use time splitting without changing the first profile");
		window.presetCombo_->setCurrentIndex(window.presetCombo_->findData(QString::fromStdString(firstId)));
		check(window.recordingSplitSettings().mode == AutoSplitSettings::Mode::Size &&
		      window.recordingSplitSettings().sizeMB == 49,
		      "switching back restores the original profile's recording split limit");
		const auto captureMode = window.captureMode_;
		window.captureMode_ = MainWindow::CaptureMode::AudioOnly;
		check(!window.recordingSplitSettings().enabled && window.activePreset().autoSplit,
		      "audio-only capture suppresses splitting without erasing the profile setting");
		window.captureMode_ = captureMode;
		Preset gif = window.activePreset();
		gif.format = RecordingFormat::GIF;
		presets.upsert(gif);
		check(!window.recordingSplitSettings().enabled && window.activePreset().autoSplit,
		      "GIF capture suppresses splitting without erasing the profile setting");
		presets.upsert(beforeCancel);
		window.starting_ = true;
		window.updateButtons();
		check(!window.editPresetButton_->isEnabled(), "profile settings are locked while starting a take");
		window.starting_ = false;
		window.countingDown_ = true;
		window.updateButtons();
		check(!window.editPresetButton_->isEnabled(), "profile settings are locked during the countdown");
		window.countingDown_ = false;
		window.updateButtons();
		check(window.editPresetButton_->isEnabled(), "profile settings unlock after canceling the countdown");
		check(ClipLibrary::videoExtensions().contains(QStringLiteral("avi")), "AVI recording parts are included in the library");

		// Same-container publishing maps every native split file to user names,
		// retaining both an existing recording and the short final part.
		const QString tempTake = output + QStringLiteral("/.harpia_tmp/take-unique");
		const QString first = tempTake + QStringLiteral("initial_part001.mkv");
		const QString second = tempTake + QStringLiteral("part.mkv");
		const QString tail = tempTake + QStringLiteral("part (2).mkv");
		write(first, "first recording part");
		write(second, "second recording part");
		write(tail, "short tail");
		const QString final = output + QStringLiteral("/Tutorial.mkv");
		const QString occupied = recording_parts::numberedPath(final, 1);
		write(occupied, "existing recording");
		window.lastRecordingFiles_ = {first, second, tail};
		window.lastScreenPath_ = final;
		window.lastAutoSplit_ = true;
		window.lastMinSeconds_ = 10;
		window.lastContentMs_ = 61000;
		window.stopHandled_ = false;
		window.finalizeAfterStop();
		check(read(occupied) == "existing recording" && read(output + QStringLiteral("/Tutorial_part001 (2).mkv")) == "first recording part",
		      "part 1 is published without overwriting an existing recording");
		check(read(recording_parts::numberedPath(final, 2)) == "second recording part" &&
		      read(recording_parts::numberedPath(final, 3)) == "short tail",
		      "all later parts including a short tail reach the output folder");
		check(!QFileInfo::exists(first) && !QFileInfo::exists(second) && !QFileInfo::exists(tail),
		      "published sources leave the temporary folder");
		window.finalizeAfterStop();
		check(!QFileInfo::exists(output + QStringLiteral("/Tutorial_part001 (3).mkv")),
		      "repeated stop notifications do not publish duplicate parts");

		// Exercise the real background MKV -> MP4 path and its name reservations.
		const QString fixture = temporary.path() + QStringLiteral("/fixture.mkv");
		QProcess ffmpeg;
		ffmpeg.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
			QStringLiteral("-i"), QStringLiteral("testsrc2=size=160x90:rate=10:duration=0.3"),
			QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-y"), fixture});
		check(ffmpeg.waitForFinished(10000) && ffmpeg.exitCode() == 0, "real video fixture is encoded for MP4 finalization");
		const QString src1 = tempTake + QStringLiteral("/copy1.mkv");
		const QString src2 = tempTake + QStringLiteral("/copy2.mkv");
		QDir().mkpath(tempTake);
		QFile::copy(fixture, src1);
		QFile::copy(fixture, src2);
		const QString mp4 = output + QStringLiteral("/same-name.mp4");
		write(mp4, "previous MP4");
		window.remuxInBackground(src1, mp4);
		window.remuxInBackground(src2, mp4);
		check(window.remuxActive_ == 2 && window.pendingRecordingTargets_.size() == 2,
		      "simultaneous finalizations reserve two different output paths");
		check(waitFor([&]() { return window.remuxActive_ == 0; }), "both background remuxes finish");
		check(read(mp4) == "previous MP4" && QFileInfo(output + QStringLiteral("/same-name (2).mp4")).size() > 100 &&
		      QFileInfo(output + QStringLiteral("/same-name (3).mp4")).size() > 100,
		      "both playable MP4s are saved while the occupied destination survives");
		check(!QFileInfo::exists(src1) && !QFileInfo::exists(src2) && window.pendingRecordingTargets_.isEmpty(),
		      "successful remux removes sources and releases reservations");
		const QString broken = tempTake + QStringLiteral("/broken.mkv");
		write(broken, "incomplete recording bytes");
		const QString failedTarget = output + QStringLiteral("/failed.mp4");
		window.remuxInBackground(broken, failedTarget);
		check(waitFor([&]() { return window.remuxActive_ == 0; }), "an unsuccessful remux finishes without hanging");
		check(!QFileInfo::exists(failedTarget) && read(output + QStringLiteral("/failed.mkv")) == "incomplete recording bytes",
		      "remux failure preserves the captured source and publishes no partial MP4");
		QThreadPool::globalInstance()->waitForDone();
		QCoreApplication::processEvents();
	}
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
