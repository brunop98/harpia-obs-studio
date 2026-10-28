// Modal editor tools block their own window family, while recording controls
// continue to receive real window-system input. Widget sendEvent() would bypass
// Qt's modal-input gate, so click the QWindow rather than the button itself.
#include "editor/EditorDialogScope.hpp"

#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *message)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
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
	QSettings::setDefaultFormat(QSettings::IniFormat);
	for (QSettings::Format format : {QSettings::IniFormat, QSettings::NativeFormat}) {
		QSettings::setPath(format, QSettings::UserScope, temporary.path());
		QSettings::setPath(format, QSettings::SystemScope, temporary.filePath("system"));
	}
	Host recorder(20), editor(260), other(500);
	QTest::qWait(30);
	recorder.click();
	check(recorder.clicks == 1, "window-system input reaches an unblocked recorder");
	{
		QDialog ordinary(&editor.window);
		QTimer::singleShot(0, [&]() {
			recorder.click();
			editor.click();
			check(recorder.clicks == 1 && editor.clicks == 0,
			      "fixture observes Qt blocking unrelated windows for an application-modal dialog");
			ordinary.reject();
		});
		ordinary.exec();
	}
	EditorDialogScope scope(&editor.window);
	{
		QDialog tool(&editor.window);
		// A tool can be polished before somebody decides to call exec().
		tool.ensurePolished();
		QTimer::singleShot(0, [&]() {
			check(tool.windowModality() == Qt::WindowModal && tool.windowHandle()->modality() == Qt::WindowModal,
			      "exec scopes both widget and native-window modality to the editor");
			recorder.click();
			editor.click();
			check(recorder.clicks == 2 && editor.clicks == 0,
			      "recorder remains clickable while an editor exec dialog blocks editor input");
			tool.accept();
		});
		tool.exec();
	}
	{
		bool seen = false;
		QTimer::singleShot(0, [&]() {
			auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
			seen = question && question->windowModality() == Qt::WindowModal;
			recorder.click();
			editor.click();
			if (question)
				question->button(QMessageBox::Yes)->click();
		});
		const auto response = QMessageBox::question(&editor.window, QStringLiteral("Question"),
			QStringLiteral("Continue?"), QMessageBox::Yes | QMessageBox::No);
		check(seen && response == QMessageBox::Yes && recorder.clicks == 3 && editor.clicks == 0,
		      "static editor questions stay modal to the editor and leave recording controls usable");
	}
	{
		QDialog outer(&editor.window);
		QTimer::singleShot(0, [&]() {
			QDialog inner(&outer);
			QTimer::singleShot(0, [&]() {
				recorder.click();
				editor.click();
				check(inner.windowModality() == Qt::WindowModal && recorder.clicks == 4 && editor.clicks == 0,
				      "nested editor questions preserve recorder input and editor modal blocking");
				inner.accept();
			});
			inner.exec();
			outer.accept();
		});
		outer.exec();
	}
	{
		QFileDialog files(&editor.window);
		files.setOption(QFileDialog::DontUseNativeDialog);
		QTimer::singleShot(0, [&]() {
			recorder.click();
			editor.click();
			check(files.windowModality() == Qt::WindowModal && recorder.clicks == 5 && editor.clicks == 0,
			      "editor file selection allows recording controls to receive input");
			files.reject();
		});
		files.exec();
	}
	{
		QDialog progress(&editor.window);
		progress.setWindowModality(Qt::WindowModal);
		progress.show();
		QCoreApplication::processEvents();
		recorder.click();
		editor.click();
		check(progress.windowModality() == Qt::WindowModal && recorder.clicks == 6 && editor.clicks == 0,
		      "existing window-modal progress dialogs retain their behavior");
		progress.close();
	}
	{
		QDialog panel(&editor.window);
		panel.show();
		QCoreApplication::processEvents();
		recorder.click();
		editor.click();
		check(panel.windowModality() == Qt::NonModal && recorder.clicks == 7 && editor.clicks == 1,
		      "modeless editor panels continue to allow input in both windows");
		panel.close();
	}
	{
		QDialog unrelated(&other.window);
		QTimer::singleShot(0, [&]() {
			recorder.click();
			check(unrelated.windowModality() == Qt::ApplicationModal && recorder.clicks == 7,
			      "unrelated application-modal windows retain their own modality policy");
			unrelated.reject();
		});
		unrelated.exec();
	}
	{
		editor.window.hide();
		QDialog loading(&editor.window);
		QTimer::singleShot(0, [&]() {
			recorder.click();
			check(loading.windowModality() == Qt::WindowModal && recorder.clicks == 8,
			      "an editor loading before its first show does not block recorder input");
			loading.reject();
		});
		loading.exec();
		editor.window.show();
	}
	check(failures == 0, "all editor dialog scoping checks pass");
	return failures ? 1 : 0;
}
