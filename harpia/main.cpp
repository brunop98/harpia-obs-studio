#include "core/ObsContext.hpp"
#include "model/PresetStore.hpp"
#include "ui/MainWindow.hpp"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>

// Entry point for the Harpia recorder. Brings up the libobs backend in the
// canonical order (startup -> audio -> video -> modules), then shows the
// minimal recorder window.
int main(int argc, char *argv[])
{
	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("Harpia Recorder"));
	QApplication::setOrganizationName(QStringLiteral("Harpia"));

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

	// Default recordings folder: <Movies>/Harpia (falls back to home).
	QString base = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	if (base.isEmpty())
		base = QDir::homePath();
	const QString outputFolder = QDir(base).filePath(QStringLiteral("Harpia"));
	QDir().mkpath(outputFolder);

	harpia::PresetStore presets;
	presets.load(outputFolder.toStdString());

	harpia::MainWindow win(obs, presets);
	win.show();

	const int rc = app.exec();

	obs.shutdown();
	return rc;
}
