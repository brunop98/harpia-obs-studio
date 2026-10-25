#include "editor/ExportOptionsDialog.hpp"
#include "ui/ExportProgressDialog.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTemporaryDir>
#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool pass, const char *message)
{
	std::printf("  %s %s\n", pass ? "PASS" : "FAIL", message);
	failures += !pass;
}
QPushButton *exportButton(ExportOptionsDialog &dialog)
{
	for (auto *button : dialog.findChildren<QPushButton *>())
		if (button->text().startsWith(QStringLiteral("Export")))
			return button;
	return nullptr;
}
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaExportTests"));
	QCoreApplication::setApplicationName(QStringLiteral("ExportChoices"));
	QTemporaryDir folder;
	QSettings settings;
	settings.remove(QStringLiteral("export"));
	ExportOptionsDialog::Context context;
	context.defaultName = QStringLiteral("test");
	context.defaultFolder = folder.path();
	context.sourceSize = QSize(320, 180);
	context.seconds = 1.0;
	context.allowGif = false;
	context.offerAppleHardwareEncoding = true;
	{
		ExportOptionsDialog dialog(context);
		auto *encoding = dialog.findChild<QComboBox *>(QStringLiteral("exportEncoding"));
		auto *quality = dialog.findChild<QSlider *>(QStringLiteral("exportQuality"));
		auto *chroma = dialog.findChild<QCheckBox *>(QStringLiteral("exportChroma444"));
		auto *format = dialog.findChild<QComboBox *>(QStringLiteral("exportFormat"));
		auto *hint = dialog.findChild<QLabel *>(QStringLiteral("exportEncodingHint"));
		auto *effort = dialog.findChild<QComboBox *>(QStringLiteral("exportEffort"));
		auto *action = exportButton(dialog);
		check(encoding && quality && chroma && format && hint && action && effort,
		      "Mac export choices and their explanation are present");
		if (!encoding || !quality || !chroma || !format || !hint || !action || !effort)
			return 1;
		check(dialog.encoding() == export_video::Encoding::Automatic && action->isEnabled(),
		      "fresh Mac exports prefer hardware automatically");
		encoding->setCurrentIndex(encoding->findData(int(export_video::Encoding::AppleHardware)));
		check(!effort->isEnabled() && dialog.encoding() == export_video::Encoding::AppleHardware,
		      "explicit hardware does not offer an ignored software effort control");
		quality->setValue(100);
		check(dialog.videoCrf() == 0 && !action->isEnabled() && hint->text().contains(QStringLiteral("lossless")),
		      "explicit hardware cannot silently downgrade a lossless export");
		encoding->setCurrentIndex(encoding->findData(int(export_video::Encoding::Automatic)));
		check(action->isEnabled() && hint->text().contains(QStringLiteral("Software")),
		      "Automatic clearly preserves lossless settings using software");
		quality->setValue(60);
		chroma->setChecked(true);
		encoding->setCurrentIndex(encoding->findData(int(export_video::Encoding::AppleHardware)));
		check(!action->isEnabled() && hint->text().contains(QStringLiteral("4:4:4")),
		      "full colour detail also requires software instead of a hardware downgrade");
		encoding->setCurrentIndex(encoding->findData(int(export_video::Encoding::Software)));
		check(action->isEnabled() && dialog.chroma444() && effort->isEnabled(),
		      "software preserves full colour and effort choices");
		if (format->findData(int(ClipExporter::Format::WebM)) >= 0) {
			format->setCurrentIndex(format->findData(int(ClipExporter::Format::WebM)));
			check(!encoding->isEnabled() && dialog.encoding() == export_video::Encoding::Software,
			      "WebM uses its supported software encoder");
		}
		format->setCurrentIndex(format->findData(int(ClipExporter::Format::Mp4)));
		check(encoding->isEnabled() && dialog.encoding() == export_video::Encoding::Software,
		      "changing container does not discard the chosen H.264 encoding mode");
		dialog.accept();
	}
	{
		ExportOptionsDialog dialog(context);
		check(dialog.encoding() == export_video::Encoding::Software,
		      "the selected Mac encoding mode is remembered across exports");
	}
	context.offerAppleHardwareEncoding = false;
	{
		ExportOptionsDialog dialog(context);
		check(!dialog.findChild<QComboBox *>(QStringLiteral("exportEncoding")) &&
		      dialog.encoding() == export_video::Encoding::Software,
		      "other platforms keep their software export choices");
	}
	{
		ExportProgressDialog progress(folder.filePath(QStringLiteral("one.mp4")), QStringLiteral("test"));
		progress.setEncoding(QStringLiteral("Software fallback: Apple hardware unavailable"));
		const auto *label = progress.findChild<QLabel *>(QStringLiteral("exportActualEncoding"));
		check(label && !label->isHidden() && label->text().contains(QStringLiteral("fallback")),
		      "the actual encoder and fallback explanation remain visible in progress");
		progress.setStage(QStringLiteral("Mixing audio"));
		check(label && label->text().contains(QStringLiteral("fallback")),
		      "later export stages preserve the encoder explanation");
		progress.setOutputPath(folder.filePath(QStringLiteral("two.mp4")));
		check(label && label->isHidden() && label->text().isEmpty(),
		      "a new batch file clears the previous file's encoder status");
		progress.finish(true);
	}
	return failures ? 1 : 0;
}
