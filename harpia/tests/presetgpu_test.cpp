#include "ui/PresetEditorDialog.hpp"
#include "core/EncoderFactory.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QElapsedTimer>
#include <QThread>
#include <QDir>
#include <cstdio>
#include <obs.h>

using namespace harpia;
static int failures;
static void ok(bool pass, const char *why) { std::printf("%s %s\n", pass ? "PASS" : "FAIL", why); failures += !pass; }
int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	if (!obs_startup("en-US", nullptr, nullptr)) return 2;
	AudioManager audio;
	Preset preset = Preset::makeDefault(QDir::tempPath().toStdString()); preset.gpuCompression = true;
	PresetEditorDialog dlg(preset, audio);
	dlg.show(); dlg.showPage(QStringLiteral("Advanced")); app.processEvents();
	auto *gpu = dlg.findChild<QCheckBox *>(QStringLiteral("recordingGpuCompression"));
	ok(gpu && gpu->isVisible() && gpu->isChecked(), "saved GPU preference is shown on Advanced");
	dlg.showPage(QStringLiteral("Video")); app.processEvents();
	ok(!gpu->isVisible(), "GPU compression is absent from Video page");
	QElapsedTimer timer; timer.start();
	while (timer.elapsed() < 450) { app.processEvents(); QThread::msleep(5); }
	bool advancedChanged = false, videoChanged = false;
	for (auto *nav : dlg.findChildren<QListWidget *>())
		for (int i = 0; i < nav->count(); ++i) {
			const auto *item = nav->item(i);
			if (item->text() == QStringLiteral("Advanced")) advancedChanged = !item->icon().isNull();
			if (item->text() == QStringLiteral("Video")) videoChanged = !item->icon().isNull();
		}
	ok(advancedChanged && !videoChanged, "GPU changed indicator belongs to Advanced");
	for (auto *button : dlg.findChildren<QPushButton *>())
		if (button->text().contains(QStringLiteral("Reset"))) button->click();
	ok(gpu->isChecked(), "resetting Video does not reset GPU setting");
	dlg.showPage(QStringLiteral("Advanced"));
	for (auto *button : dlg.findChildren<QPushButton *>())
		if (button->text().contains(QStringLiteral("Reset"))) button->click();
	ok(!gpu->isChecked(), "resetting Advanced restores GPU default");
	gpu->setChecked(true);
	QMetaObject::invokeMethod(&dlg, "accept", Qt::DirectConnection);
	ok(dlg.result().gpuCompression, "GPU checkbox changes are saved to original preset field");
	Preset gif = preset; gif.format = RecordingFormat::GIF; gif.gpuCompression = true;
	PresetEditorDialog gifDlg(gif, audio); gifDlg.showPage(QStringLiteral("Advanced")); gifDlg.show(); app.processEvents();
	auto *hidden = gifDlg.findChild<QCheckBox *>(QStringLiteral("recordingGpuCompression"));
	ok(hidden && !hidden->isVisible() && hidden->isChecked(), "GIF hides GPU option without clearing stored preference");
	QMetaObject::invokeMethod(&gifDlg, "accept", Qt::DirectConnection);
	ok(gifDlg.result().gpuCompression, "saving GIF preserves hidden GPU preference");
	return failures ? 1 : 0;
}
