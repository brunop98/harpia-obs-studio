#include "Version.hpp"
#include "core/CrashGuard.hpp"
#include "core/Logger.hpp"
#include "core/ObsContext.hpp"
#include "model/PresetStore.hpp"
#include "ui/MainWindow.hpp"
#include "ui/SingleInstance.hpp"
#include "ui/StartupSplash.hpp"
#include "ui/UiText.hpp"

#include <util/base.h> // base_set_crash_handler
#include <util/bmem.h>
#include <util/platform.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include <QApplication>
#include <QElapsedTimer>
#include <QIcon>
#include <QDir>
#include <QMessageBox>
#include <QPalette>
#include <QSettings>
#include "editor/ProjectPaths.hpp"

#include <QStandardPaths>
#include <QStyleFactory>
#include <string>
#include <vector>

namespace {

// Apply a clean, modern dark theme app-wide (so dialogs match the main window).
void applyDarkPalette(QApplication &app)
{
	app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

	QPalette p;
	const QColor base(0x1e, 0x1f, 0x22);
	const QColor panel(0x2b, 0x2d, 0x31);
	const QColor text(0xe6, 0xe6, 0xe6);
	const QColor accent(0xe5, 0x48, 0x4d);

	p.setColor(QPalette::Window, base);
	p.setColor(QPalette::WindowText, text);
	p.setColor(QPalette::Base, QColor(0x20, 0x22, 0x25));
	p.setColor(QPalette::AlternateBase, panel);
	p.setColor(QPalette::Text, text);
	p.setColor(QPalette::Button, panel);
	p.setColor(QPalette::ButtonText, text);
	p.setColor(QPalette::ToolTipBase, panel);
	p.setColor(QPalette::ToolTipText, text);
	p.setColor(QPalette::Highlight, accent);
	p.setColor(QPalette::HighlightedText, Qt::white);
	p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x6b, 0x6f, 0x76));
	p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x6b, 0x6f, 0x76));
	app.setPalette(p);
}

// Resolve the log directory (created before obs starts so early logs are kept).
std::string resolveLogDir()
{
	char *path = os_get_config_path_ptr("harpia-recorder/logs");
	std::string dir = path ? path : "";
	bfree(path);
	return dir;
}

std::string resolveCrashDir()
{
	char *path = os_get_config_path_ptr("harpia-recorder/crash");
	std::string dir = path ? path : "";
	bfree(path);
	return dir;
}

// libobs's own fatal path (bcrash): a graphics-subsystem failure and friends
// arrive here as printf-style text. Feed it into the same report the OS-level
// handlers write, then die the way libobs expects.
void obsCrashToReport(const char *fmt, va_list args, void *)
{
	char buf[2048];
	std::vsnprintf(buf, sizeof(buf), fmt ? fmt : "libobs fatal error", args);
	harpia::CrashGuard::writeReport(buf);
	std::abort(); // the SIGABRT handler is a no-op re-report; the first text wins
}

// Route Qt's own messages into the same session log.
void qtMessageToLogger(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
	harpia::LogLevel level = harpia::LogLevel::Info;
	switch (type) {
	case QtDebugMsg:
		level = harpia::LogLevel::Debug;
		break;
	case QtInfoMsg:
		level = harpia::LogLevel::Info;
		break;
	case QtWarningMsg:
		level = harpia::LogLevel::Warning;
		break;
	case QtCriticalMsg:
	case QtFatalMsg:
		level = harpia::LogLevel::Error;
		break;
	}
	harpia::Logger::instance().log(level, ("Qt: " + msg).toStdString());
	// qFatal aborts the process right after this handler returns -- capture
	// its message as the crash description before that happens. (The SIGABRT
	// handler that follows is a no-op: the first description wins.)
	if (type == QtFatalMsg)
		harpia::CrashGuard::writeReport(("Qt fatal: " + msg).toUtf8().constData());
}

// Last run's phase durations, which weight this run's progress bar. Stored
// under the app's own QSettings so a fresh install simply falls back to the
// built-in shape.
constexpr char kTimingGroup[] = "startup/phase/";

QMap<QString, int> loadStartupTimings()
{
	QSettings s;
	QMap<QString, int> out;
	for (int i = 0; i < harpia::startupPhaseCount(); ++i) {
		const QString key = QString::fromLatin1(harpia::startupPhaseKey(harpia::startupPhaseAt(i)));
		const QVariant v = s.value(QLatin1String(kTimingGroup) + key);
		if (v.isValid())
			out.insert(key, v.toInt());
	}
	return out;
}

void saveStartupTimings(const QMap<QString, int> &measured)
{
	QSettings s;
	for (auto it = measured.cbegin(); it != measured.cend(); ++it)
		s.setValue(QLatin1String(kTimingGroup) + it.key(), it.value());
}

} // namespace

// Entry point for the Harpia recorder. Brings up the libobs backend in the
// canonical order (startup -> audio -> video -> modules), then shows the
// minimal recorder window.
int main(int argc, char *argv[])
{
	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("Harpia Recorder"));
	QApplication::setOrganizationName(QStringLiteral("Harpia"));
	// Window/taskbar/Alt-Tab icon (all platforms). The Windows .exe icon itself
	// comes from resources/harpia.rc compiled into the binary.
	QApplication::setWindowIcon(QIcon(QStringLiteral(":/harpia.png")));
	applyDarkPalette(app);
	// Before any window exists: the role sizes in UiText are read off the base
	// font, so a widget built first would keep the platform default.
	harpia::applyTextScale(app);

	// One instance, and before ANY of the backend exists.
	//
	// Two Harpias cannot coexist: they would fight over the capture devices,
	// the audio graph, the output folder and the preset file, and the second
	// obs_startup() fails in a way nobody can read. This has to come before the
	// logger too -- a second process opening the session log is itself a way to
	// corrupt the first one's.
	harpia::SingleInstance instance(
		harpia::SingleInstance::userScopedKey(QStringLiteral("harpia-recorder")));
	if (!instance.acquire())
		return 0; // the running copy has been asked to come forward

	// Crash handlers before anything that can crash: fatal signals, unhandled
	// C++ exceptions and (on Windows) SEH all write a description to a pending
	// report -- shown as a native message box at crash time on Windows, and as
	// a proper dialog on the next start. libobs's own fatal path joins in
	// below via base_set_crash_handler.
	harpia::CrashGuard::install(resolveCrashDir());
	base_set_crash_handler(obsCrashToReport, nullptr);

	// Start logging before anything else so startup and any early crash are
	// captured to disk (flushed per line).
	harpia::Logger::instance().init(resolveLogDir());
	qInstallMessageHandler(qtMessageToLogger);
	harpia::Logger::instance().log(harpia::LogLevel::Info,
				       std::string("Harpia Recorder v") + HARPIA_VERSION_STRING + " starting");

	// Something on screen for the rest of this. Startup is dominated by libobs
	// work that has to finish before the window can exist -- the graphics device
	// and every capture/encoder plugin -- so the app used to be an empty taskbar
	// entry for the whole of it. The bar is weighted by the last run's real
	// timings; see StartupSplash.hpp.
	harpia::StartupTimeline timeline(loadStartupTimings());
	QElapsedTimer clock;
	clock.start();
	harpia::StartupSplash splash(
		QStringLiteral("v%1").arg(QString::fromUtf8(HARPIA_VERSION_STRING)));
	splash.show();
	const auto step = [&](harpia::StartupPhase p) {
		timeline.begin(p, clock.elapsed());
		splash.showPhase(timeline.label(), timeline.percent());
	};
	step(harpia::StartupPhase::Backend);

	harpia::ObsContext obs;
	if (!obs.startup()) {
		splash.hide();
		QMessageBox::critical(nullptr, QStringLiteral("Harpia Recorder"),
				      QStringLiteral("Failed to initialize the OBS backend (obs_startup)."));
		return 1;
	}

	// Audio and an initial video graph, then load all capture/encoder/output
	// plugins. MainWindow re-configures video per the active preset -- and that
	// second reset is now free when the preset happens to want this same shape,
	// because ObsContext skips an identical one.
	//
	// The placeholder size cannot be avoided by asking the preset first: on
	// Windows the preset's canvas comes from screenForActivePreset(), which
	// needs monitor_capture's properties, which needs the modules that have not
	// been loaded yet.
	step(harpia::StartupPhase::Audio);
	obs.resetAudio();
	step(harpia::StartupPhase::Video);
	obs.resetVideo(1920, 1080, 30);
	step(harpia::StartupPhase::Plugins);
	obs.loadModules();

	// Dependency self-check: if required backend plugins didn't load (missing or
	// blocked libraries), report it clearly and log it, rather than failing with a
	// cryptic error only at record time. Non-fatal so the user can still open the
	// Error Logs to see details.
	step(harpia::StartupPhase::Components);
	{
		const std::vector<std::string> missing = obs.missingDependencies();
		if (!missing.empty()) {
			QString list;
			std::string logLine = "Missing required components:";
			for (const std::string &m : missing) {
				list += QStringLiteral("  •  %1\n").arg(QString::fromStdString(m));
				logLine += " " + m + ";";
			}
			harpia::Logger::instance().log(harpia::LogLevel::Error, logLine);
			// Out of the way first: a modal dialog behind a frameless
			// always-on-top splash is a hang as far as anyone can tell.
			splash.hide();
			QMessageBox::warning(
				nullptr, QStringLiteral("Harpia Recorder — missing components"),
				QStringLiteral(
					"Some required components did not load, so recording may not work:\n\n%1\n"
					"This usually means plugin libraries are missing from the install, or "
					"were blocked by antivirus/security software. See Error Logs for details.")
					.arg(list));
			splash.show();
			splash.showPhase(timeline.label(), timeline.percent());
		}
	}

	// Default recordings folder: <Movies>/Harpia (falls back to home).
	step(harpia::StartupPhase::Presets);
	QString base = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	if (base.isEmpty())
		base = QDir::homePath();
	const QString outputFolder = QDir(base).filePath(QStringLiteral("Harpia"));
	QDir().mkpath(outputFolder);
	// ...and one for saved editing projects beside it, so there is a place to
	// look for the thing you were working on last week. Made at startup rather
	// than on the first save: an empty folder in the file manager is how the
	// user learns it exists. See editor/ProjectPaths.hpp.
	QDir().mkpath(harpia::defaultProjectsFolder());

	harpia::PresetStore presets;
	presets.load(outputFolder.toStdString());

	// Scope the window so it (and the obs sources/outputs its members own) is
	// destroyed BEFORE obs_shutdown() — releasing obs objects after shutdown
	// would crash.
	int rc;
	{
		step(harpia::StartupPhase::Window);
		harpia::MainWindow win(obs, presets, outputFolder);
		win.show();
		splash.hide();
		// Painted before the slow half runs. This is the part that changes what
		// startup FEELS like: finishStartup() probes cameras, lists recordings
		// and creates the capture source, none of which the window needs in
		// order to be on screen and usable.
		QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
		step(harpia::StartupPhase::Finishing);
		win.finishStartup();
		timeline.finish(clock.elapsed());

		// The measurement this whole thing doubles as: what each phase really
		// cost, on this machine, in the session log -- and back into QSettings
		// so the next run's bar is weighted by it.
		harpia::Logger::instance().log(
			harpia::LogLevel::Info,
			"startup " + std::to_string(timeline.totalMs()) + "ms (" +
				timeline.summary().toStdString() + ")");
		saveStartupTimings(timeline.measured());

		// Launching a second copy shows this one rather than doing nothing:
		// double-clicking the icon when the app is already open should get you
		// the app, which is the whole point of refusing the second instance.
		QObject::connect(&instance, &harpia::SingleInstance::anotherInstanceStarted, &win,
				 [&win]() { harpia::raiseWindowToFront(&win); });
		rc = app.exec();
	}

	obs.shutdown();
	harpia::Logger::instance().shutdown();
	return rc;
}
