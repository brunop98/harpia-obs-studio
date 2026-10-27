#include "RecordingVideoPicker.hpp"

#include "library/ClipLibrary.hpp"
#include "RecordingDelete.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

namespace harpia {

namespace {
const QSize kThumbnailSize(192, 108);

QString recordingPathKey(const QString &path)
{
	const QFileInfo file(path);
	QString key = file.canonicalFilePath();
	if (key.isEmpty())
		key = file.absoluteFilePath();
#ifdef Q_OS_WIN
	key = key.toCaseFolded();
#endif
	return key;
}

QIcon videoPlaceholder(const QPalette &palette)
{
	QPixmap image(kThumbnailSize);
	image.fill(palette.color(QPalette::AlternateBase));
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	QColor color = palette.color(QPalette::Text);
	color.setAlpha(150);
	painter.setPen(Qt::NoPen);
	painter.setBrush(color);
	const QPointF centre(image.width() / 2.0, image.height() / 2.0);
	painter.drawPolygon(QPolygonF{centre + QPointF(-9, -14), centre + QPointF(-9, 14),
				     centre + QPointF(14, 0)});
	painter.end();
	return QIcon(image);
}
} // namespace

RecordingVideoPicker::RecordingVideoPicker(const QStringList &libraryFolders, QWidget *parent)
	: QDialog(parent), thumbnails_(this)
{
	setObjectName(QStringLiteral("recordingVideoPicker"));
	setWindowTitle(QStringLiteral("Add video from recordings"));
	setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
		       Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
	setSizeGripEnabled(true);
	resize(860, 580);
	setMinimumSize(480, 340);

	auto *layout = new QVBoxLayout(this);
	auto *hint = new QLabel(QStringLiteral("Choose a recording to add to this project, or double-click it."), this);
	hint->setWordWrap(true);
	layout->addWidget(hint);

	grid_ = new QListWidget(this);
	grid_->setObjectName(QStringLiteral("recordingVideoGrid"));
	grid_->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(grid_, &QListWidget::customContextMenuRequested, this, &RecordingVideoPicker::showContextMenu);
	grid_->setViewMode(QListView::IconMode);
	grid_->setResizeMode(QListView::Adjust);
	grid_->setMovement(QListView::Static);
	grid_->setSelectionMode(QAbstractItemView::SingleSelection);
	grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	grid_->setUniformItemSizes(true);
	grid_->setSpacing(8);
	grid_->setIconSize(kThumbnailSize);
	grid_->setGridSize(QSize(216, kThumbnailSize.height() + grid_->fontMetrics().lineSpacing() * 2 + 20));
	grid_->setWordWrap(false);
	grid_->setTextElideMode(Qt::ElideMiddle);
	grid_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	layout->addWidget(grid_, 1);

	status_ = new QLabel(this);
	status_->setObjectName(QStringLiteral("recordingVideoStatus"));
	status_->setWordWrap(true);
	layout->addWidget(status_);

	auto *importButton = new QPushButton(QStringLiteral("Import from folder…"), this);
	importButton->setObjectName(QStringLiteral("importVideoFromFolder"));
	importButton->setAutoDefault(false);
	importButton->setToolTip(QStringLiteral("Browse your computer for another video file."));
	connect(importButton, &QPushButton::clicked, this, [this]() {
		importFromFolder_ = true;
		accept();
	});

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
	addButton_ = buttons->addButton(QStringLiteral("Add video"), QDialogButtonBox::AcceptRole);
	addButton_->setObjectName(QStringLiteral("addSelectedRecording"));
	addButton_->setDefault(true);
	addButton_->setEnabled(false);
	connect(addButton_, &QPushButton::clicked, this, &RecordingVideoPicker::acceptSelection);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(grid_, &QListWidget::itemSelectionChanged, this, [this]() {
		addButton_->setEnabled(!selectedVideo().isEmpty());
	});
	connect(grid_, &QListWidget::itemActivated, this, [this](QListWidgetItem *) { acceptSelection(); });
	connect(&thumbnails_, &ThumbnailCache::ready, this, &RecordingVideoPicker::updateThumbnail);

	auto *footer = new QHBoxLayout;
	footer->addWidget(importButton);
	footer->addStretch(1);
	footer->addWidget(buttons);
	layout->addLayout(footer);

	const QIcon placeholder = videoPlaceholder(grid_->palette());
	const QVector<ClipInfo> recordings = ClipLibrary::scan(libraryFolders);
	for (const ClipInfo &clip : recordings) {
		if (clip.isAudio)
			continue;
		auto *item = new QListWidgetItem(placeholder,
			QStringLiteral("%1\n%2 · %3").arg(clip.fileName, clip.humanSize(), clip.relativeAge()), grid_);
		item->setData(Qt::UserRole, clip.filePath);
		item->setTextAlignment(Qt::AlignHCenter);
		item->setToolTip(QStringLiteral("%1\n%2 · %3").arg(clip.filePath, clip.exactDate(), clip.humanSize()));
		itemByPath_.insert(clip.filePath, item);
		updateThumbnail(clip.filePath);
		thumbnails_.ensure(clip.filePath, kThumbnailSize);
	}
	updateRecordingCount();
}

void RecordingVideoPicker::updateRecordingCount()
{
	status_->setText(itemByPath_.isEmpty()
		? QStringLiteral("No recorded videos found. Use Import from folder… to choose a video on your computer.")
		: QStringLiteral("%1 recorded video%2").arg(itemByPath_.size()).arg(itemByPath_.size() == 1 ? QString() : QStringLiteral("s")));
}

void RecordingVideoPicker::setProtectedPaths(const QStringList &paths)
{
	protectedPaths_.clear();
	for (const QString &path : paths)
		if (!path.isEmpty())
			protectedPaths_.insert(recordingPathKey(path));
}

void RecordingVideoPicker::showContextMenu(const QPoint &pos)
{
	QListWidgetItem *item = grid_->itemAt(pos);
	if (!item)
		return;
	grid_->setCurrentItem(item);
	const QString path = item->data(Qt::UserRole).toString();
	if (path.isEmpty())
		return;
	addButton_->setEnabled(!selectedVideo().isEmpty());
	const QFileInfo file(path);
	const QString folder = file.absolutePath();
	const bool exists = file.isFile();
	const bool protectedFile = protectedPaths_.contains(recordingPathKey(path));
	QMenu menu(this);
	menu.setObjectName(QStringLiteral("recordingVideoContextMenu"));
	menu.setToolTipsVisible(true);
	auto action = [&menu](const QString &text, const QString &name, bool enabled = true) {
		QAction *entry = menu.addAction(text);
		entry->setObjectName(name);
		entry->setEnabled(enabled);
		return entry;
	};
	QAction *play = action(QStringLiteral("Play video"), QStringLiteral("recordingPlayVideo"), exists);
	QAction *copy = action(QStringLiteral("Copy"), QStringLiteral("recordingCopyFile"), exists);
	QAction *copyPath = action(QStringLiteral("Copy file path"), QStringLiteral("recordingCopyPath"));
	QAction *copyFolder = action(QStringLiteral("Copy folder path"), QStringLiteral("recordingCopyFolderPath"));
	QAction *openFolder = action(QStringLiteral("Open folder"), QStringLiteral("recordingOpenFolder"), QFileInfo(folder).isDir());
	menu.addSeparator();
	QAction *remove = action(QStringLiteral("Delete"), QStringLiteral("recordingDelete"), exists && !protectedFile);
	if (protectedFile)
		remove->setToolTip(QStringLiteral("This video is loaded in the current project. Remove it from the project before deleting it."));
	QAction *chosen = menu.exec(grid_->viewport()->mapToGlobal(pos));
	if (chosen == copy) {
		auto *mime = new QMimeData;
		mime->setUrls({QUrl::fromLocalFile(file.absoluteFilePath())});
		mime->setText(QDir::toNativeSeparators(file.absoluteFilePath()));
		QApplication::clipboard()->setMimeData(mime);
	} else if (chosen == copyPath || chosen == copyFolder) {
		QApplication::clipboard()->setText(QDir::toNativeSeparators(chosen == copyPath ? file.absoluteFilePath() : folder));
	} else if (chosen == play || chosen == openFolder) {
		if (!QDesktopServices::openUrl(QUrl::fromLocalFile(chosen == play ? file.absoluteFilePath() : folder)))
			QMessageBox::warning(this, QStringLiteral("Open"),
				chosen == play ? QStringLiteral("Could not open the video in its default player.")
					       : QStringLiteral("Could not open the recording's folder."));
	} else if (chosen == remove && !protectedPaths_.contains(recordingPathKey(path))) {
		const QStringList gone = recording_delete::confirmAndRecycle(this, {path});
		if (gone.isEmpty())
			return;
		{
			QSignalBlocker block(grid_);
			for (const QString &removed : gone)
				if (QListWidgetItem *row = itemByPath_.take(removed))
					delete grid_->takeItem(grid_->row(row));
			grid_->clearSelection();
			grid_->setCurrentRow(-1);
		}
		addButton_->setEnabled(false);
		updateRecordingCount();
		emit recordingsDeleted(gone);
	}
}

QString RecordingVideoPicker::selectedVideo() const
{
	if (importFromFolder_ || !grid_->currentItem() || !grid_->currentItem()->isSelected())
		return {};
	const QString path = grid_->currentItem()->data(Qt::UserRole).toString();
	return QFileInfo(path).isFile() ? path : QString();
}

void RecordingVideoPicker::acceptSelection()
{
	if (selectedVideo().isEmpty()) {
		addButton_->setEnabled(false);
		status_->setText(QStringLiteral("This recording is no longer available. Choose another video or import from a folder."));
		return;
	}
	accept();
}

void RecordingVideoPicker::updateThumbnail(const QString &path)
{
	QListWidgetItem *item = itemByPath_.value(path, nullptr);
	if (!item)
		return;
	const QImage image = thumbnails_.cached(path, kThumbnailSize);
	if (!image.isNull())
		item->setIcon(QIcon(QPixmap::fromImage(image)));
}

} // namespace harpia
