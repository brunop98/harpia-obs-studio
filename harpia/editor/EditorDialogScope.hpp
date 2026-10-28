#pragma once

// Editor tools may ask a modal question or run a dialog's local event loop, but
// recording is an independent job. Scope those dialogs to the editor's window
// family so the recorder and its floating controls remain usable.

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QPointer>
#include <QWindow>

namespace harpia {

class EditorDialogScope : public QObject {
public:
	explicit EditorDialogScope(QWidget *editor) : QObject(editor), editor_(editor)
	{
		if (auto *application = QCoreApplication::instance())
			application->installEventFilter(this);
	}
	~EditorDialogScope() override
	{
		if (auto *application = QCoreApplication::instance())
			application->removeEventFilter(this);
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (event->type() != QEvent::Show || !editor_)
			return false;
		auto *dialog = qobject_cast<QDialog *>(object);
		if (!dialog || dialog == editor_ || dialog->windowModality() != Qt::ApplicationModal)
			return false;
		bool belongs = false;
		for (QWidget *owner = dialog->parentWidget(); owner; owner = owner->parentWidget())
			if (owner == editor_) {
				belongs = true;
				break;
			}
		if (!belongs)
			return false;
		dialog->setWindowModality(Qt::WindowModal);
		// At Show the native window can already carry the modality assigned by
		// QDialog::exec(). QWidget's setter alone does not update that window,
		// leaving unrelated recorder windows blocked despite the new property.
		if (QWindow *window = dialog->windowHandle())
			window->setModality(Qt::WindowModal);
		return false;
	}

private:
	QPointer<QWidget> editor_;
};

} // namespace harpia
