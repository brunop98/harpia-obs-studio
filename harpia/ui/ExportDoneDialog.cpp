#include "ui/ExportDoneDialog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace harpia {

namespace {
constexpr int kThumbW = 220;
constexpr int kThumbH = 124;

// Select the file in the platform's file manager, rather than merely opening
// the folder: what you want after an export is the file highlighted.
void revealFile(const QString &path)
{
#if defined(Q_OS_WIN)
	QProcess::startDetached(QStringLiteral("explorer.exe"),
				{QStringLiteral("/select,") + QDir::toNativeSeparators(path)});
#elif defined(Q_OS_MAC)
	QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#else
	// No portable "select this file" on Linux; the folder is the honest answer.
	QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}
} // namespace

// ---------------------------------------------------------------------------

FileDragThumb::FileDragThumb(QWidget *parent) : QLabel(parent)
{
	setFixedSize(kThumbW, kThumbH);
	setAlignment(Qt::AlignCenter);
	setCursor(Qt::OpenHandCursor);
	setToolTip(QStringLiteral("Drag this into another app to hand it the file"));
	setStyleSheet(QStringLiteral("background:#0d0e11; border:1px solid #3a3f49; border-radius:4px;"
				     "color:#7d838f;"));
	setText(QStringLiteral("no preview"));
}

void FileDragThumb::setThumb(const QImage &img)
{
	if (img.isNull())
		return;
	setPixmap(QPixmap::fromImage(
		img.scaled(kThumbW - 2, kThumbH - 2, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

void FileDragThumb::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton) {
		pressed_ = true;
		pressPos_ = e->pos();
	}
	QLabel::mousePressEvent(e);
}

void FileDragThumb::mouseMoveEvent(QMouseEvent *e)
{
	// Qt's own threshold, so this feels like every other drag on the machine and
	// a slightly shaky click is still a click.
	if (!pressed_ || path_.isEmpty() ||
	    (e->pos() - pressPos_).manhattanLength() < QApplication::startDragDistance()) {
		QLabel::mouseMoveEvent(e);
		return;
	}
	pressed_ = false;
	QMimeData *mime = ExportDoneDialog::fileMimeData(path_);
	if (!mime)
		return;
	auto *drag = new QDrag(this);
	drag->setMimeData(mime);
	// Drag the picture the user is pointing at, so it is obvious what is moving.
	const QPixmap shown = pixmap();
	if (!shown.isNull()) {
		drag->setPixmap(shown.scaledToWidth(140, Qt::SmoothTransformation));
		drag->setHotSpot(QPoint(70, drag->pixmap().height() / 2));
	}
	setCursor(Qt::ClosedHandCursor);
	drag->exec(Qt::CopyAction, Qt::CopyAction);
	setCursor(Qt::OpenHandCursor);
}

// ---------------------------------------------------------------------------

QMimeData *ExportDoneDialog::fileMimeData(const QString &filePath)
{
	if (filePath.isEmpty())
		return nullptr;
	auto *mime = new QMimeData;
	// The URL list is the part that matters: it is what a file manager, a chat
	// client or a mail composer reads as "here is a file", and on Windows Qt
	// turns it into the CF_HDROP that Explorer pastes as the file itself.
	mime->setUrls({QUrl::fromLocalFile(QFileInfo(filePath).absoluteFilePath())});
	// And the path as text, so dropping onto a terminal or a text field gives
	// something useful instead of nothing.
	mime->setText(QDir::toNativeSeparators(QFileInfo(filePath).absoluteFilePath()));
	return mime;
}

QString ExportDoneDialog::folderPathFor(const QString &filePath)
{
	if (filePath.isEmpty())
		return QString();
	return QDir::toNativeSeparators(QFileInfo(filePath).absolutePath());
}

QString ExportDoneDialog::nativePathFor(const QString &filePath)
{
	if (filePath.isEmpty())
		return QString();
	return QDir::toNativeSeparators(QFileInfo(filePath).absoluteFilePath());
}

ExportDoneDialog::ExportDoneDialog(const QString &filePath, const QImage &thumb, QWidget *parent)
	: QDialog(parent), path_(filePath)
{
	setWindowTitle(QStringLiteral("Clip exported"));
	setModal(true);

	const QFileInfo fi(filePath);

	auto *root = new QVBoxLayout(this);
	root->setSpacing(12);

	auto *top = new QHBoxLayout;
	top->setSpacing(14);
	auto *thumbLbl = new FileDragThumb(this);
	thumbLbl->setFile(filePath);
	thumbLbl->setThumb(thumb);
	top->addWidget(thumbLbl, 0, Qt::AlignTop);

	auto *info = new QVBoxLayout;
	info->setSpacing(4);
	auto *name = new QLabel(fi.fileName(), this);
	QFont nf = name->font();
	nf.setBold(true);
	name->setFont(nf);
	name->setTextInteractionFlags(Qt::TextSelectableByMouse);
	info->addWidget(name);

	auto *where = new QLabel(folderPathFor(filePath), this);
	where->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	where->setWordWrap(true);
	where->setTextInteractionFlags(Qt::TextSelectableByMouse);
	info->addWidget(where);

	const qint64 bytes = fi.size();
	auto *size = new QLabel(QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1), this);
	size->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	info->addWidget(size);

	auto *hint = new QLabel(QStringLiteral("Drag the thumbnail into another app to share it."), this);
	hint->setStyleSheet(QStringLiteral("color:#7d838f;"));
	hint->setWordWrap(true);
	info->addWidget(hint);
	info->addStretch(1);
	top->addLayout(info, 1);
	root->addLayout(top);

	// The actions, in a grid so five of them do not stretch the dialog into a
	// letterbox. None of these close anything -- that is the whole point.
	auto *grid = new QGridLayout;
	grid->setSpacing(6);
	struct Action {
		const char *label;
		const char *id;
		int row, col;
	};
	const Action actions[] = {
		{"Copy File", "copyFile", 0, 0},   {"Copy File Path", "copyFilePath", 0, 1},
		{"Copy Folder Path", "copyFolderPath", 0, 2}, {"Open File", "openFile", 1, 0},
		{"Open Folder", "openFolder", 1, 1},
	};
	for (const Action &a : actions) {
		auto *b = new QPushButton(QString::fromLatin1(a.label), this);
		// Not the default button, and not auto-default: otherwise Return would
		// fire whichever one Qt picked instead of closing, and a button that
		// looks like the primary action would imply it finishes the dialog.
		b->setAutoDefault(false);
		b->setDefault(false);
		const QString id = QString::fromLatin1(a.id);
		connect(b, &QPushButton::clicked, this, [this, id]() {
			if (id == QLatin1String("copyFile")) {
				if (QMimeData *m = fileMimeData(path_)) {
					QApplication::clipboard()->setMimeData(m);
					flash(QStringLiteral("File copied — paste it anywhere"));
				}
			} else if (id == QLatin1String("copyFilePath")) {
				copyToClipboard(nativePathFor(path_), QStringLiteral("File path copied"));
			} else if (id == QLatin1String("copyFolderPath")) {
				copyToClipboard(folderPathFor(path_), QStringLiteral("Folder path copied"));
			} else if (id == QLatin1String("openFile")) {
				QDesktopServices::openUrl(QUrl::fromLocalFile(path_));
				flash(QStringLiteral("Opening the file…"));
			} else if (id == QLatin1String("openFolder")) {
				revealFile(path_);
				flash(QStringLiteral("Opening the folder…"));
			}
			emit actionTriggered(id);
		});
		grid->addWidget(b, a.row, a.col);
	}
	root->addLayout(grid);

	// A line that says what just happened. Copying is invisible otherwise, and
	// an action with no feedback reads as a broken button.
	status_ = new QLabel(QString(), this);
	status_->setStyleSheet(QStringLiteral("color:#5fd08a;"));
	status_->setMinimumHeight(status_->sizeHint().height());
	root->addWidget(status_);

	auto *bottom = new QHBoxLayout;
	bottom->addStretch(1);
	auto *close = new QPushButton(QStringLiteral("Close"), this);
	close->setDefault(true);
	connect(close, &QPushButton::clicked, this, &QDialog::accept);
	bottom->addWidget(close);
	root->addLayout(bottom);

	setMinimumWidth(520);
}

void ExportDoneDialog::copyToClipboard(const QString &text, const QString &what)
{
	if (text.isEmpty())
		return;
	QApplication::clipboard()->setText(text);
	flash(what);
}

void ExportDoneDialog::flash(const QString &message)
{
	if (!status_)
		return;
	status_->setText(message);
	// Cleared after a moment so the line describes the LAST thing you did rather
	// than accumulating a history nobody asked for.
	QTimer::singleShot(2500, status_, [this, message]() {
		if (status_ && status_->text() == message)
			status_->clear();
	});
}

} // namespace harpia
