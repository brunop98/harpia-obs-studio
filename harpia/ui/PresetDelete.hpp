#pragma once

// Deleting a preset: the question Harpia asks first. Pure, so it is tested
// (tests/presetdelete_test.cpp); MainWindow::deleteActivePreset asks it.
//
// A preset's recordings are never deleted with it -- they stay in its output
// folder. But Harpia lists recordings by the presets' folders (Recent
// recordings, the Clip Library), so the ones in a folder that no other preset
// uses drop out of those lists. The question says which of the two it is,
// rather than leaving "where did my videos go?" for later.

#include <QDir>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QVector>

namespace harpia::preset_delete {

// The same folder? Cleaned, either kind of slash, and without regard to case
// on Windows, where the file system has none.
inline bool sameFolder(const QString &a, const QString &b)
{
	if (a.trimmed().isEmpty() || b.trimmed().isEmpty())
		return false;
	const QString ca = QDir::cleanPath(QString(a.trimmed()).replace(QLatin1Char('\\'), QLatin1Char('/')));
	const QString cb = QDir::cleanPath(QString(b.trimmed()).replace(QLatin1Char('\\'), QLatin1Char('/')));
#ifdef Q_OS_WIN
	return ca.compare(cb, Qt::CaseInsensitive) == 0;
#else
	return ca == cb;
#endif
}

struct OtherPreset {
	QString name;
	QString folder;
};

// The names of the other presets that save to `folder`, in list order.
inline QStringList sharing(const QString &folder, const QVector<OtherPreset> &others)
{
	QStringList names;
	for (const OtherPreset &o : others)
		if (sameFolder(o.folder, folder))
			names << o.name;
	return names;
}

struct Question {
	QString text;    // the question itself
	QString details; // what happens to the recordings
};

inline Question question(const QString &name, const QString &folder, const QStringList &sharedWith)
{
	Question q;
	q.text = QStringLiteral("Delete the preset \"%1\"?").arg(name);
	const QString where = QDir::toNativeSeparators(QDir::cleanPath(folder.trimmed()));
	QString what;
	if (folder.trimmed().isEmpty()) {
		what = QStringLiteral("It has no recordings folder, so nothing on disk changes.");
	} else if (sharedWith.isEmpty()) {
		what = QStringLiteral("Its recordings are not deleted: they stay in %1. But no other preset saves "
				      "there, so Harpia will no longer list them in Recent recordings or the Clip "
				      "Library.")
			       .arg(where);
	} else {
		QStringList quoted;
		for (const QString &n : sharedWith)
			quoted << QStringLiteral("\"%1\"").arg(n);
		what = QStringLiteral("Its recordings are not deleted: they stay in %1, and stay listed, because %2 "
				      "%3 there too.")
			       .arg(where, (sharedWith.size() == 1 ? QStringLiteral("the preset ")
								    : QStringLiteral("the presets ")) +
						   quoted.join(QStringLiteral(", ")),
				    sharedWith.size() == 1 ? QStringLiteral("saves") : QStringLiteral("save"));
	}
	q.details = what + QStringLiteral("\n\nThe preset itself cannot be brought back.");
	return q;
}

// Asks. True only for the "Delete preset" button: Cancel is the default, so
// Enter, Escape and closing the box all keep the preset.
inline bool confirm(QWidget *parent, const Question &q)
{
	QMessageBox box(QMessageBox::Question, QStringLiteral("Delete preset"), q.text, QMessageBox::NoButton, parent);
	box.setInformativeText(q.details);
	QPushButton *deleteButton = box.addButton(QStringLiteral("Delete preset"), QMessageBox::DestructiveRole);
	box.setDefaultButton(box.addButton(QMessageBox::Cancel));
	box.exec();
	return box.clickedButton() == deleteButton;
}

} // namespace harpia::preset_delete
