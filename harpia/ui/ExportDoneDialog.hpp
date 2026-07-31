#pragma once

// What you see the moment an export finishes.
//
// It used to be a message box: the path as text, Open File, Open Folder, Done
// -- and every one of those buttons closed it. So the common case, "export and
// then go and use the file", meant exporting, dismissing, and going to find the
// file yourself; and wanting two of those things meant exporting twice or
// digging through Explorer.
//
// This is the same information with the file made HANDLEABLE:
//   * the thumbnail is a drag source. Drag it into Discord, an email, a folder
//     -- anywhere that takes a dropped file -- and the exported file goes with
//     it, because the drag carries the file's URL and not a picture of it.
//   * five actions, none of which dismiss anything. Copy the file, its path,
//     its folder; open the file or the folder. Do all five if you like.
//   * it closes when YOU close it. Close, or the window's X. Nothing else.
//
// The two pieces worth testing on their own -- what the clipboard and the drag
// actually carry -- are static and take a path, so they can be checked without
// a window.

#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QPoint>
#include <QString>

class QMimeData;

namespace harpia {

// A label that hands out the file when you drag it.
class FileDragThumb : public QLabel {
	Q_OBJECT
public:
	explicit FileDragThumb(QWidget *parent = nullptr);
	void setFile(const QString &path) { path_ = path; }
	void setThumb(const QImage &img);

protected:
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;

private:
	QString path_;
	QPoint pressPos_;
	bool pressed_ = false;
};

class ExportDoneDialog : public QDialog {
	Q_OBJECT
public:
	// `thumb` may be null; the dialog then shows a placeholder rather than a
	// broken frame, and dragging still works because the drag carries the path.
	ExportDoneDialog(const QString &filePath, const QImage &thumb, QWidget *parent = nullptr);

	// What a drag or a "Copy File" carries: the file itself, as a URL, plus its
	// path as plain text so a text field gets something useful too. Caller owns
	// the result. Empty path -> nullptr.
	static QMimeData *fileMimeData(const QString &filePath);

	// The folder a "Copy Folder Path" should yield, in the platform's own
	// spelling. Empty for an empty path.
	static QString folderPathFor(const QString &filePath);
	// The file path in the platform's own spelling.
	static QString nativePathFor(const QString &filePath);

signals:
	// Emitted for each action, so a test (and the log) can see what was asked
	// for without watching the clipboard or launching a file manager.
	void actionTriggered(const QString &id);

private:
	void copyToClipboard(const QString &text, const QString &what);
	void flash(const QString &message);

	QString path_;
	QLabel *status_ = nullptr;
};

} // namespace harpia
