// Optional native recorder fixture: real MainWindow and a pausable raw OBS
// output. No encoder/muxer plugin or file recording is needed to exercise the
// capture area's movement and recording lifecycle. Run under a real/Xvfb
// display with the same renderer as the recorder frontend.
#include <QtWidgets>
#include <QTest>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#define private public
#include "ui/MainWindow.hpp"
#undef private
#include "ui/RecorderControlsOverlay.hpp"
#include "ui/RegionTool.hpp"
#include "core/ObsContext.hpp"
#include "model/PresetStore.hpp"
#include <obs.h>
#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}

void registerTestOutput()
{
	obs_output_info output = {};
	output.id = "harpia_region_lock_test_output";
	output.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_CAN_PAUSE;
	output.get_name = [](void *) { return "Region lock test"; };
	output.create = [](obs_data_t *, obs_output_t *self) -> void * { return self; };
	output.destroy = [](void *) {};
	output.start = [](void *self) { return obs_output_begin_data_capture(static_cast<obs_output_t *>(self), 0); };
	output.stop = [](void *self, uint64_t) { obs_output_end_data_capture(static_cast<obs_output_t *>(self)); };
	output.raw_video = [](void *, video_data *) {};
	obs_register_output(&output);
}

bool startTake(MainWindow &window)
{
	window.recorder_.output_ = obs_output_create("harpia_region_lock_test_output", "region-lock-take", nullptr, nullptr);
	return window.recorder_.output_ && obs_output_start(window.recorder_.output_);
}

bool endTake(MainWindow &window)
{
	obs_output_t *output = window.recorder_.output_;
	if (!output)
		return true;
	obs_output_stop(output);
	QElapsedTimer timeout;
	timeout.start();
	while (obs_output_active(output) && timeout.elapsed() < 2000)
		QTest::qWait(10);
	const bool ended = !obs_output_active(output);
	window.syncFollowMouse();
	window.updateFloatingControls();
	if (ended) {
		obs_output_release(output);
		window.recorder_.output_ = nullptr;
	}
	return ended;
}
}

int main(int argc, char **argv)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
	// As with autosplitwindow_test, native QSettings isolation below is Linux
	// only. Never write into a user's registry or CFPreferences for a fixture.
	std::fprintf(stderr, "This fixture requires a file-based native QSettings store.\n");
	return 77;
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir temporary;
	if (!temporary.isValid())
		return 2;
	qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
	qputenv("XDG_CACHE_HOME", temporary.filePath("cache").toUtf8());
	QApplication app(argc, argv);
	QStandardPaths::setTestModeEnabled(true);
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
	QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, temporary.path());
	QSettings::setPath(QSettings::NativeFormat, QSettings::SystemScope, temporary.filePath("system"));
	ObsContext obs;
	check(obs.startup() && obs.resetVideo(320, 180, 30) == OBS_VIDEO_SUCCESS,
	      "real OBS video initializes for the recorder lock fixture");
	if (failures)
		return 1;
	registerTestOutput();
	const QString outputFolder = temporary.filePath("Recordings");
	QDir().mkpath(outputFolder);
	QDir().mkpath(temporary.filePath("config"));
	PresetStore presets(temporary.filePath("config").toStdString());
	presets.load(outputFolder.toStdString());
	{
		MainWindow window(obs, presets, outputFolder);
		// Keep the real handlers but drive their timing explicitly. In
		// particular do not run the deferred camera/desktop startup probes.
		for (QTimer *timer : window.findChildren<QTimer *>())
			timer->stop();
		Preset profile = window.activePreset();
		profile.followMouse = true;
		profile.followSmoothness = 0;
		profile.followPaddingPct = 0;
		profile.followAxis = 0;
		profile.idleTimeoutSeconds = 0;
		profile.regionLeavePauseSeconds = 0;
		presets.upsert(profile);
		window.captureMode_ = MainWindow::CaptureMode::Region;
		window.currentRegion_ = CaptureRegion{true, 40, 40, 200, 140};
		check(!window.canLockRecordingRegion(), "the region cannot be locked before recording starts");
		check(startTake(window) && window.recorder_.isRecording(), "a real pausable OBS output is active");
		if (!window.recorder_.isRecording())
			return 1;
		window.syncFollowMouse();
		window.followTimer_->stop();
		window.updateFloatingControls();
		auto *lock = window.floatingControls_->findChild<QPushButton *>(QStringLiteral("recordingRegionLock"));
		check(window.canLockRecordingRegion() && lock && !lock->isHidden() && lock->isEnabled(),
		      "a following Custom Region take exposes the floating lock button");
		if (!lock) {
			endTake(window);
			return 1;
		}
		QScreen *screen = QGuiApplication::primaryScreen();
		check(screen && screen->geometry().width() >= 600 && screen->geometry().height() >= 400,
		      "the display has room for a region to follow the cursor");
		if (!screen) {
			endTake(window);
			return 1;
		}
		const QPoint screenOrigin = screen->geometry().topLeft();
		const QPoint farCorner = screenOrigin + QPoint(screen->geometry().width() - 10, screen->geometry().height() - 10);
		QCursor::setPos(farCorner);
		const CaptureRegion home = window.currentRegion_;
		window.tickFollowMouse();
		window.followTimer_->stop();
		check(window.currentRegion_ != home, "Follow Mouse first moves the recording away from its home framing");
		const CaptureRegion frozen = window.currentRegion_;
		lock->click();
		check(window.regionFollowLocked_ && lock->isChecked() && !window.followTimer_->isActive(),
		      "clicking the lock button parks following immediately");
		check(window.currentRegion_ == frozen && !window.followMouse_.returning(),
		      "locking keeps the exact current area instead of returning home");
		QCursor::setPos(screenOrigin + QPoint(5, 5));
		for (int tick = 0; tick < 20; ++tick)
			window.tickFollowMouse();
		check(window.currentRegion_ == frozen && window.capture_.region_ == frozen,
		      "far-away cursor updates cannot move the locked crop");
		window.tickRegionWatch();
		check(!window.recorder_.isPaused() && !window.regionAutoPaused_,
		      "moving to controls outside a locked area does not activate leave-region auto-pause");

		QTest::qWait(80); // allow a raw frame timestamp before pausing
		check(window.recorder_.pause(true), "the actual output can pause while its area is locked");
		window.syncFollowMouse();
		window.updateFloatingControls();
		check(window.regionFollowLocked_ && !window.followMouse_.armed() && lock->isChecked(),
		      "the area lock survives pausing even though the follow stepper disarms");
		lock->click();
		check(!window.regionFollowLocked_ && !window.followTimer_->isActive() && window.currentRegion_ == frozen,
		      "unlocking while paused updates the toggle without moving the paused area");
		lock->click();
		check(window.regionFollowLocked_, "the area can also be locked while the take is paused");
		window.recorder_.pause(false);
		window.syncFollowMouse();
		check(window.regionFollowLocked_ && window.followMouse_.armed() && !window.followTimer_->isActive(),
		      "resuming re-arms following while keeping the locked area parked");
		window.tickFollowMouse();
		check(window.currentRegion_ == frozen, "a resumed locked take does not follow the remote cursor");

		profile.followSmoothness = 50;
		presets.upsert(profile);
		QTest::qWait(800);
		lock->click();
		window.followTimer_->stop();
		window.tickFollowMouse();
		const CaptureRegion firstResumed = window.currentRegion_;
		check(!window.regionFollowLocked_ && firstResumed.x < frozen.x && firstResumed.x > frozen.x * 3 / 4,
		      "unlocking resumes smoothly without treating the whole locked interval as one follow step");
		check(firstResumed.width == frozen.width && firstResumed.height == frozen.height,
		      "the recording dimensions stay fixed while locking and unlocking");

		window.onFollowToggle();
		window.followTimer_->stop();
		check(window.followSuspended_ && window.followMouse_.returning(),
		      "the existing follow shortcut can still start its return-home glide");
		const CaptureRegion midway = window.currentRegion_;
		lock->click();
		window.tickFollowMouse();
		check(window.regionFollowLocked_ && !window.followSuspended_ && !window.followMouse_.returning() &&
		      window.currentRegion_ == midway,
		      "locking cancels an in-progress return-home glide at its current position");
		window.onFollowToggle();
		check(!window.regionFollowLocked_ && !window.followSuspended_ && window.followTimer_->isActive(),
		      "the follow shortcut unlocks a parked area and resumes following");
		window.followTimer_->stop();

		for (bool *busy : {&window.starting_, &window.stopping_, &window.countingDown_}) {
			*busy = true;
			window.setRecordingRegionLocked(true);
			check(!window.canLockRecordingRegion() && !window.regionFollowLocked_,
			      "start, stop and countdown transitions cannot enable the region lock");
			*busy = false;
		}
		window.captureMode_ = MainWindow::CaptureMode::Monitor;
		window.setRecordingRegionLocked(true);
		check(!window.canLockRecordingRegion() && !window.regionFollowLocked_,
		      "Entire Monitor recording does not acquire a following-region lock");
		window.captureMode_ = MainWindow::CaptureMode::Region;
		window.currentRegion_.enabled = false;
		window.setRecordingRegionLocked(true);
		check(!window.canLockRecordingRegion() && !window.regionFollowLocked_,
		      "a disabled region cannot be locked");
		window.currentRegion_.enabled = true;
		Preset fixedProfile = profile;
		fixedProfile.followMouse = false;
		presets.upsert(fixedProfile);
		window.setRecordingRegionLocked(true);
		check(!window.canLockRecordingRegion() && !window.regionFollowLocked_,
		      "a profile without Follow Mouse does not expose this temporary lock");
		presets.upsert(profile);

		window.syncFollowMouse();
		window.followTimer_->stop();
		window.setRecordingRegionLocked(true);
		QTest::qWait(80);
		window.recorder_.pause(true);
		window.syncFollowMouse();
		check(window.regionFollowLocked_ && !window.followMouse_.armed(),
		      "the stop lifecycle starts with a locked, already-disarmed paused take");
		check(endTake(window), "the native output stops cleanly");
		check(!window.regionFollowLocked_ && !lock->isChecked() && lock->isHidden(),
		      "stopping clears the lock even when following had already disarmed");
		check(window.activePreset() == profile, "temporary locking never changes the saved profile");
		check(startTake(window), "a new take can start after a locked take ends");
		window.syncFollowMouse();
		check(!window.regionFollowLocked_ && window.followMouse_.armed() && window.followTimer_->isActive(),
		      "the next take follows normally rather than inheriting a previous lock");
		window.followTimer_->stop();
		check(endTake(window), "the second test take also stops cleanly");
	}
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
