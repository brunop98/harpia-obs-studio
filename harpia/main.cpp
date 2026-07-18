#include "core/ObsContext.hpp"
#include "model/PresetStore.hpp"
#include "ui/MainWindow.hpp"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QPalette>
#include <QStandardPaths>
#include <QStyleFactory>

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

} // namespace

// Entry point for the Harpia recorder. Brings up the libobs backend in the
// canonical order (startup -> audio -> video -> modules), then shows the
// minimal recorder window.
int main(int argc, char *argv[])
{
	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("Harpia Recorder"));
	QApplication::setOrganizationName(QStringLiteral("Harpia"));
	applyDarkPalette(app);

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

	harpia::MainWindow win(obs, presets, outputFolder);
	win.show();

	const int rc = app.exec();

	obs.shutdown();
	return rc;
}
