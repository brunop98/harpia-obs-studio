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
#include "ui/AutoSplitDialog.hpp"
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
void choose(MainWindow &window, bool enabled, bool always, int size, bool accept = true)
{
	QTimer::singleShot(0, [&]() {
		auto *dialog = dynamic_cast<AutoSplitDialog *>(window.findChild<QDialog *>(QStringLiteral("autoSplitDialog")));
		check(dialog && dialog->isVisible(), "Auto Split opens from the recorder's button");
		check(window.hotkeysSuspended_, "recording hotkeys are suspended while choosing split settings");
		if (!dialog)
			return;
		dialog->findChild<QCheckBox *>(QStringLiteral("autoSplitEnabled"))->setChecked(enabled);
		dialog->findChild<QRadioButton *>(QStringLiteral("autoSplitSize"))->setChecked(true);
		dialog->findChild<QSpinBox *>(QStringLiteral("autoSplitSizeMB"))->setValue(size);
		dialog->findChild<QRadioButton *>(always ? QStringLiteral("autoSplitAlways") : QStringLiteral("autoSplitOnce"))->setChecked(true);
		if (accept)
			dialog->accept();
		else
			dialog->reject();
	});
	window.autoSplitButton_->click();
	check(!window.hotkeysSuspended_, "recording hotkeys resume after accepting or canceling split settings");
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
		check(window.autoSplitButton_ && window.autoSplitButton_->isEnabled() && window.autoSplitButton_->text().contains(QStringLiteral("Off")),
		      "Auto Split is accessible and off by default");
		choose(window, true, false, 49);
		check(window.autoSplit_.hasOverride() && window.autoSplit_.next().sizeMB == 49,
		      "Only this time sets the next recording's size limit");
		QSettings saved(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		check(!auto_split_settings::load(saved).enabled, "one-time settings do not change the saved default");
		choose(window, true, true, 123, false);
		check(window.autoSplit_.hasOverride() && window.autoSplit_.next().sizeMB == 49,
		      "Cancel retains the previous one-time choice");
		choose(window, true, true, 500);
		check(!window.autoSplit_.hasOverride() && auto_split_settings::load(saved).enabled && auto_split_settings::load(saved).sizeMB == 500,
		      "Always persists settings and replaces the one-time choice");
		window.starting_ = true;
		window.updateButtons();
		check(!window.autoSplitButton_->isEnabled(), "split settings are locked while starting a take");
		window.starting_ = false;
		window.countingDown_ = true;
		window.updateButtons();
		check(!window.autoSplitButton_->isEnabled(), "split settings are locked during the countdown");
		window.countingDown_ = false;
		window.updateButtons();
		check(window.autoSplitButton_->isEnabled(), "split settings unlock after the countdown is canceled");
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
