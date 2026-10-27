#pragma once

// Native dialog helpers receive modality before any QWidget Show event. Set
// that policy explicitly before exec(), keeping the platform's native picker
// while allowing independent recorder windows to continue receiving input.

#include <QColorDialog>
#include <QFileDialog>
#include <QPointer>

namespace harpia::editor_dialogs {
namespace detail {
inline QStringList files(QWidget *parent, const QString &caption, const QString &directory,
			 const QString &filter, QString *selectedFilter, QFileDialog::Options options,
			 QFileDialog::FileMode fileMode, QFileDialog::AcceptMode acceptMode)
{
	QPointer<QFileDialog> dialog = new QFileDialog(parent, caption, directory, filter);
	dialog->setOptions(options);
	dialog->setFileMode(fileMode);
	dialog->setAcceptMode(acceptMode);
	if (selectedFilter && !selectedFilter->isEmpty())
		dialog->selectNameFilter(*selectedFilter);
	dialog->setWindowModality(Qt::WindowModal);
	const int result = dialog->exec();
	if (!dialog)
		return {}; // its parent can be closed by the independent recorder
	QStringList paths;
	if (result == QDialog::Accepted) {
		paths = dialog->selectedFiles();
		if (selectedFilter)
			*selectedFilter = dialog->selectedNameFilter();
	}
	delete dialog.data();
	return paths;
}
} // namespace detail

inline QString getOpenFileName(QWidget *parent = nullptr, const QString &caption = QString(),
			       const QString &directory = QString(), const QString &filter = QString(),
			       QString *selectedFilter = nullptr, QFileDialog::Options options = {})
{
	return detail::files(parent, caption, directory, filter, selectedFilter, options,
			     QFileDialog::ExistingFile, QFileDialog::AcceptOpen).value(0);
}

inline QStringList getOpenFileNames(QWidget *parent = nullptr, const QString &caption = QString(),
				   const QString &directory = QString(), const QString &filter = QString(),
				   QString *selectedFilter = nullptr, QFileDialog::Options options = {})
{
	return detail::files(parent, caption, directory, filter, selectedFilter, options,
			     QFileDialog::ExistingFiles, QFileDialog::AcceptOpen);
}

inline QString getSaveFileName(QWidget *parent = nullptr, const QString &caption = QString(),
			       const QString &directory = QString(), const QString &filter = QString(),
			       QString *selectedFilter = nullptr, QFileDialog::Options options = {})
{
	return detail::files(parent, caption, directory, filter, selectedFilter, options,
			     QFileDialog::AnyFile, QFileDialog::AcceptSave).value(0);
}

inline QString getExistingDirectory(QWidget *parent = nullptr, const QString &caption = QString(),
				    const QString &directory = QString(),
				    QFileDialog::Options options = QFileDialog::ShowDirsOnly)
{
	return detail::files(parent, caption, directory, QString(), nullptr, options,
			     QFileDialog::Directory, QFileDialog::AcceptOpen).value(0);
}

inline QColor getColor(const QColor &initial = Qt::white, QWidget *parent = nullptr,
			const QString &title = QString(), QColorDialog::ColorDialogOptions options = {})
{
	QPointer<QColorDialog> dialog = new QColorDialog(parent);
	if (!title.isEmpty())
		dialog->setWindowTitle(title);
	dialog->setOptions(options);
	dialog->setCurrentColor(initial);
	dialog->setWindowModality(Qt::WindowModal);
	dialog->exec();
	if (!dialog)
		return {};
	const QColor selected = dialog->selectedColor();
	delete dialog.data();
	return selected;
}

} // namespace harpia::editor_dialogs
