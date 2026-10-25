// Warning presentation stays nonmodal, copyable and local to one editor.
#include "editor/PreviewRenderWarning.hpp"

#include <QTest>

#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
QStringList logged;
void check(bool value, const char *message)
{
	std::printf("  %s %s\n", value ? "PASS" : "FAIL", message);
	failures += !value;
}
void capture(EditorLogLevel level, const QString &report)
{
	if (level == EditorLogLevel::Warning)
		logged.append(report);
}
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	const auto original = editorLogSink();
	editorLogSink() = capture;
	QWidget parent;
	auto *layout = new QVBoxLayout(&parent);
	PreviewRenderWarning first(&parent), second(&parent);
	layout->addWidget(&first);
	layout->addWidget(&second);
	parent.show();
	QApplication::processEvents();
	check(first.isHidden() && second.isHidden(), "healthy previews have no warning banner");
	const QString report = QStringLiteral("Shader <take> could not compile.\nFix its .frag file.\nDriver: undeclared identifier at line 14.\nTrack: 2; clip: 3; time: 500 ms");
	for (int i = 0; i < 100; ++i)
		first.setFailure(QStringLiteral("shader/2/3"), QStringLiteral("Shader <take> could not compile. Fix its .frag file."), report);
	check(first.isVisible() && second.isHidden(), "a failure is visible only in the affected editor's banner");
	check(logged.size() == 1 && logged.first().contains(report), "one hundred failed frames log the complete diagnostic only once");
	check(!QApplication::activeModalWidget() && first.findChildren<QDialog *>().isEmpty(), "playback failures never open a modal or modeless dialog automatically");
	auto *label = first.findChild<QLabel *>(QStringLiteral("previewRenderWarningText"));
	check(label && label->textFormat() == Qt::PlainText && label->text().contains(QStringLiteral("<take>")), "effect names are shown as plain text with an actionable reason");
	first.findChild<QPushButton *>(QStringLiteral("previewRenderWarningDetails"))->click();
	QApplication::processEvents();
	const auto details = first.findChildren<QDialog *>();
	check(details.size() == 1 && details.first()->isVisible() && !details.first()->isModal(), "Details opens one modeless diagnostic window on request");
	auto *text = details.isEmpty() ? nullptr : details.first()->findChild<QPlainTextEdit *>();
	check(text && text->isReadOnly() && text->toPlainText() == report, "details retain the complete copyable driver and clip context");
	if (!details.isEmpty())
		for (auto *button : details.first()->findChildren<QPushButton *>())
			if (button->text() == QStringLiteral("Copy report"))
				button->click();
	check(QApplication::clipboard()->text() == report, "Copy report copies the exact diagnostic without truncating compiler output");
	first.clearFailure();
	check(first.isHidden(), "a healthy frame clears the visible warning");
	first.setFailure(QStringLiteral("shader/2/3"), QStringLiteral("same failure"), report);
	check(logged.size() == 1, "returning to the same faulty clip does not repeat its log on every scrub");
	second.setFailure(QStringLiteral("shader/2/3"), QStringLiteral("another editor"), report);
	check(logged.size() == 2, "another editor owns its own diagnostic history");
	first.resetHistory();
	first.setFailure(QStringLiteral("shader/2/3"), QStringLiteral("new project"), report);
	check(logged.size() == 3, "opening another project resets that editor's warning history");
	editorLogSink() = original;
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
