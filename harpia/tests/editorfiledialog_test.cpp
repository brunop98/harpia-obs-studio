// Native-capable editor pickers must receive WindowModal before exec(), rather
// than rely on the QWidget Show event skipped by native platform helpers.
#include "editor/EditorFileDialog.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWindow>

#include <cstdio>
#include <functional>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *message)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}
bool write(const QString &path)
{
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write("fixture") == 7;
}
struct Host {
	QWidget window;
	QPushButton button{QStringLiteral("Action"), &window};
	int clicks = 0;
	explicit Host(int x)
	{
		window.resize(180, 100);
		window.move(x, 20);
		button.setGeometry(15, 15, 130, 40);
		QObject::connect(&button, &QPushButton::clicked, [&]() { ++clicks; });
		window.show();
	}
	void click()
	{
		QTest::mouseClick(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
				  button.geometry().center());
		QCoreApplication::processEvents();
	}
};
void chooseFile(const std::function<void()> &open, QFileDialog::FileMode mode, QFileDialog::AcceptMode acceptMode,
		const QString &path, bool accept, Host &recorder, Host &editor)
{
	bool seen = false, answered = false, timedOut = false;
	QElapsedTimer elapsed;
	elapsed.start();
	QTimer responder;
	QObject::connect(&responder, &QTimer::timeout, [&]() {
		auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
		if (elapsed.elapsed() > 4000) {
			timedOut = true;
			if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
				modal->reject();
			return;
		}
		if (!dialog)
			return;
		if (!answered) {
			seen = true;
			answered = true;
			check(dialog->windowModality() == Qt::WindowModal && dialog->windowHandle()->modality() == Qt::WindowModal,
			      "file picker is explicitly window-modal without an editor Show-event filter");
			check(dialog->fileMode() == mode && dialog->acceptMode() == acceptMode,
			      "file picker preserves its single/multiple/save/directory selection mode");
			const int previous = recorder.clicks, blocked = editor.clicks;
			recorder.click();
			editor.click();
			check(recorder.clicks == previous + 1 && editor.clicks == blocked,
			      "file picker blocks its editor while recorder receives window-system input");
		}
		if (!path.isEmpty()) {
			if (mode == QFileDialog::Directory)
				dialog->setDirectory(path);
			else if (auto *fileName = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
				fileName->setText(path); // type the path into the focused filename field
		}
		if (accept)
			// QFileDialog narrows accept()'s visibility; invoke its meta-method
			// through QDialog's public slot as a user pressing Open would.
			QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
		else
			dialog->reject();
	});
	responder.start(10);
	open();
	check(seen && answered && !timedOut, "file picker completes its accepted or cancelled choice");
}
}

int main(int argc, char **argv)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
	// QFileDialog writes QtProject history through an explicit NativeFormat
	// QSettings constructor. setPath cannot redirect registry/CFPreferences.
	Q_UNUSED(argc);
	Q_UNUSED(argv);
	std::fprintf(stderr, "This fixture requires a file-based native QSettings store.\n");
	return 77;
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir temporary;
	if (!temporary.isValid())
		return 2;
	qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
	qputenv("XDG_CACHE_HOME", temporary.filePath("cache").toUtf8());
	qputenv("XDG_DATA_HOME", temporary.filePath("data").toUtf8());
	QApplication application(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaEditorFileDialogTest"));
	QCoreApplication::setApplicationName(QStringLiteral("ScopedPickers"));
	QSettings::setDefaultFormat(QSettings::IniFormat);
	for (QSettings::Format format : {QSettings::IniFormat, QSettings::NativeFormat}) {
		QSettings::setPath(format, QSettings::UserScope, temporary.path());
		QSettings::setPath(format, QSettings::SystemScope, temporary.filePath("system"));
	}
	Host recorder(20), editor(260);
	QTest::qWait(30);
	const QString first = temporary.filePath("first.txt"), second = temporary.filePath("second.txt");
	check(write(first) && write(second), "existing files are available for native-capable picker wrappers");
	const auto nativeTestOptions = QFileDialog::DontUseNativeDialog;
	QString selectedFilter = QStringLiteral("Text (*.txt)");
	QString result;
	chooseFile([&]() {
		result = editor_dialogs::getOpenFileName(&editor.window, QStringLiteral("Open"), temporary.path(),
			QStringLiteral("Text (*.txt);;All files (*)"), &selectedFilter, nativeTestOptions);
	}, QFileDialog::ExistingFile, QFileDialog::AcceptOpen, first, true, recorder, editor);
	check(result == first && selectedFilter == QStringLiteral("Text (*.txt)"),
	      "single-file wrapper returns the accepted file and selected filter");
	QStringList multiple;
	chooseFile([&]() {
		multiple = editor_dialogs::getOpenFileNames(&editor.window, QStringLiteral("Open files"), temporary.path(),
			QStringLiteral("Text (*.txt)"), nullptr, nativeTestOptions);
	}, QFileDialog::ExistingFiles, QFileDialog::AcceptOpen, second, true, recorder, editor);
	check(multiple == QStringList{second}, "multiple-file wrapper returns its selected-files list");
	const QString output = temporary.filePath("new.txt");
	chooseFile([&]() {
		result = editor_dialogs::getSaveFileName(&editor.window, QStringLiteral("Save"), output,
			QStringLiteral("Text (*.txt)"), nullptr, nativeTestOptions | QFileDialog::DontConfirmOverwrite);
	}, QFileDialog::AnyFile, QFileDialog::AcceptSave, output, true, recorder, editor);
	check(result == output && !QFile::exists(output), "save wrapper returns a new path without writing a file");
	chooseFile([&]() {
		result = editor_dialogs::getExistingDirectory(&editor.window, QStringLiteral("Directory"), temporary.path(),
			nativeTestOptions | QFileDialog::ShowDirsOnly);
	}, QFileDialog::Directory, QFileDialog::AcceptOpen, temporary.path(), true, recorder, editor);
	check(QDir::cleanPath(result) == QDir::cleanPath(temporary.path()), "directory wrapper returns the chosen directory");
	selectedFilter = QStringLiteral("Text (*.txt)");
	chooseFile([&]() {
		result = editor_dialogs::getOpenFileName(&editor.window, QStringLiteral("Cancel"), temporary.path(),
			QStringLiteral("Text (*.txt);;All files (*)"), &selectedFilter, nativeTestOptions);
	}, QFileDialog::ExistingFile, QFileDialog::AcceptOpen, first, false, recorder, editor);
	check(result.isEmpty() && selectedFilter == QStringLiteral("Text (*.txt)"),
	      "cancel returns an empty path and leaves the caller's selected filter untouched");
	{
		const QColor colour(24, 68, 102, 128);
		QTimer::singleShot(0, [&]() {
			auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget());
			check(dialog && dialog->windowModality() == Qt::WindowModal,
			      "colour wrapper sets window modality before opening its picker");
			if (dialog) {
				dialog->setCurrentColor(colour);
				dialog->accept();
			}
		});
		const QColor accepted = editor_dialogs::getColor(Qt::white, &editor.window, QStringLiteral("Colour"),
			QColorDialog::DontUseNativeDialog | QColorDialog::ShowAlphaChannel);
		check(accepted == colour, "colour wrapper retains alpha and returns only the accepted colour");
		QTimer::singleShot(0, [&]() {
			if (auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget()))
				dialog->reject();
		});
		check(!editor_dialogs::getColor(colour, &editor.window, QStringLiteral("Cancel colour"),
			QColorDialog::DontUseNativeDialog).isValid(), "cancelled colour selection returns an invalid colour");
	}
	{
		auto *owner = new QWidget;
		QTimer::singleShot(0, [&]() { delete owner; });
		check(editor_dialogs::getOpenFileName(owner, QStringLiteral("Closing owner"), temporary.path(), QString(),
			nullptr, nativeTestOptions).isEmpty(), "closing a picker owner safely cancels without deleting its dialog twice");
	}
	check(failures == 0, "all explicit editor picker checks pass");
	return failures ? 1 : 0;
}
