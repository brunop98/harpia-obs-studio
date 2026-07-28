#include "Version.hpp"
#include "core/Logger.hpp"
#include "core/ObsContext.hpp"
#include "model/PresetStore.hpp"
#include "ui/MainWindow.hpp"
#include "ui/UiText.hpp"

#include <util/bmem.h>
#include <util/platform.h>

#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QMessageBox>
#include <QPalette>
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

	// Start logging before anything else so startup and any early crash are
	// captured to disk (flushed per line).
	harpia::Logger::instance().init(resolveLogDir());
	qInstallMessageHandler(qtMessageToLogger);
	harpia::Logger::instance().log(harpia::LogLevel::Info,
				       std::string("Harpia Recorder v") + HARPIA_VERSION_STRING + " starting");

	harpia::ObsContext obs;
	if (!obs.startup()) {
		QMessageBox::critical(nullptr, QStringLiteral("Harpia Recorder"),
				      QStringLiteral("Failed to initialize the OBS backend (obs_startup)."));
		return 1;
	}

	// Audio and an initial video graph, then load all capture/encoder/output
	// plugins. MainWindow re-configures video per the active preset.
	obs.resetAudio();
	obs.resetVideo(1920, 1080, 30);
	obs.loadModules();

	// Dependency self-check: if required backend plugins didn't load (missing or
	// blocked libraries), report it clearly and log it, rather than failing with a
	// cryptic error only at record time. Non-fatal so the user can still open the
	// Error Logs to see details.
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
			QMessageBox::warning(
				nullptr, QStringLiteral("Harpia Recorder — missing components"),
				QStringLiteral(
					"Some required components did not load, so recording may not work:\n\n%1\n"
					"This usually means plugin libraries are missing from the install, or "
					"were blocked by antivirus/security software. See Error Logs for details.")
					.arg(list));
		}
	}

	// Default recordings folder: <Movies>/Harpia (falls back to home).
	QString base = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	if (base.isEmpty())
		base = QDir::homePath();
	const QString outputFolder = QDir(base).filePath(QStringLiteral("Harpia"));
	QDir().mkpath(outputFolder);

	harpia::PresetStore presets;
	presets.load(outputFolder.toStdString());

	// Scope the window so it (and the obs sources/outputs its members own) is
	// destroyed BEFORE obs_shutdown() — releasing obs objects after shutdown
	// would crash.
	int rc;
	{
		harpia::MainWindow win(obs, presets, outputFolder);
		win.show();
		rc = app.exec();
	}

	obs.shutdown();
	harpia::Logger::instance().shutdown();
	return rc;
}
