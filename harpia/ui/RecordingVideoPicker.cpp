#include "RecordingVideoPicker.hpp"

#include "RecordingDelete.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
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
#include <QProgressBar>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTabBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace harpia {

namespace {
const QSize kThumbnailSize(192, 108);
constexpr int kModifiedRole = Qt::UserRole + 1;
constexpr int kSizeRole = Qt::UserRole + 2;
constexpr int kNameRole = Qt::UserRole + 3;
constexpr const char *kOrderProperty = "recordingVideoSortKey";

QString scanPathKey(const QString &path)
{
	// Scanner paths are already canonical. Keep discovery batches free of
	// filesystem reads on the GUI thread, including for deduplication.
	QString key = QDir::cleanPath(path);
#ifdef Q_OS_WIN
	key = key.toCaseFolded();
#endif
	return key;
}

class VideoItem : public QListWidgetItem {
public:
	using QListWidgetItem::QListWidgetItem;
	bool operator<(const QListWidgetItem &other) const override
	{
		const int order = listWidget() ? listWidget()->property(kOrderProperty).toInt() : 0;
		if (order == 0 || order == 1) {
			const int role = order == 1 ? kSizeRole : kModifiedRole;
			const qint64 value = data(role).toLongLong();
			const qint64 otherValue = other.data(role).toLongLong();
			if (value != otherValue)
				return value > otherValue; // newest or largest first
		}
		const int name = QString::compare(data(kNameRole).toString(), other.data(kNameRole).toString(),
						 Qt::CaseInsensitive);
		return name != 0 ? name < 0 : data(Qt::UserRole).toString() < other.data(Qt::UserRole).toString();
	}
};

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
	: QDialog(parent), libraryFolders_(libraryFolders), thumbnails_(this), scanner_(this)
{
	setObjectName(QStringLiteral("recordingVideoPicker"));
	setWindowTitle(QStringLiteral("Add video"));
	setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
		       Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
	setSizeGripEnabled(true);
	resize(860, 580);
	setMinimumSize(480, 340);

	auto *layout = new QVBoxLayout(this);
	auto *hint = new QLabel(QStringLiteral("Choose a video to add to this project, or double-click it."), this);
	hint->setWordWrap(true);
	layout->addWidget(hint);
	tabs_ = new QTabBar(this);
	tabs_->setObjectName(QStringLiteral("recordingVideoTabs"));
	tabs_->setExpanding(false);
	tabs_->addTab(QStringLiteral("Harpia Editor"));
	tabs_->addTab(QStringLiteral("Downloads"));
	tabs_->addTab(QStringLiteral("Videos"));
	orderBy_ = new QComboBox(this);
	orderBy_->setObjectName(QStringLiteral("recordingVideoOrderBy"));
	orderBy_->setAccessibleName(QStringLiteral("Order by"));
	orderBy_->addItem(QStringLiteral("Date"), 0);
	orderBy_->addItem(QStringLiteral("Size"), 1);
	orderBy_->addItem(QStringLiteral("Name"), 2);
	orderBy_->setToolTip(QStringLiteral("Date: newest first. Size: largest first. Name: A to Z."));
	auto *orderLabel = new QLabel(QStringLiteral("Order by"), this);
	orderLabel->setBuddy(orderBy_);
	auto *browseRow = new QHBoxLayout;
	browseRow->addWidget(tabs_);
	browseRow->addStretch(1);
	browseRow->addWidget(orderLabel);
	browseRow->addWidget(orderBy_);
	layout->addLayout(browseRow);

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
	grid_->setLayoutMode(QListView::Batched);
	grid_->setBatchSize(64);
	grid_->setSpacing(8);
	grid_->setIconSize(kThumbnailSize);
	grid_->setGridSize(QSize(216, kThumbnailSize.height() + grid_->fontMetrics().lineSpacing() * 2 + 20));
	grid_->setWordWrap(false);
	grid_->setTextElideMode(Qt::ElideMiddle);
	grid_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	layout->addWidget(grid_, 1);
	grid_->viewport()->installEventFilter(this);
	thumbnailTimer_ = new QTimer(this);
	thumbnailTimer_->setSingleShot(true);
	thumbnailTimer_->setInterval(35);
	connect(thumbnailTimer_, &QTimer::timeout, this, &RecordingVideoPicker::loadVisibleThumbnails);
	connect(grid_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { scheduleThumbnails(); });

	status_ = new QLabel(this);
	status_->setObjectName(QStringLiteral("recordingVideoStatus"));
	status_->setWordWrap(true);
	progress_ = new QProgressBar(this);
	progress_->setObjectName(QStringLiteral("recordingVideoScanProgress"));
	progress_->setRange(0, 0);
	progress_->setTextVisible(false);
	progress_->setMaximumWidth(140);
	auto *scanStatus = new QHBoxLayout;
	scanStatus->addWidget(status_, 1);
	scanStatus->addWidget(progress_);
	layout->addLayout(scanStatus);

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

	connect(tabs_, &QTabBar::currentChanged, this, [this]() { startScan(); });
	connect(orderBy_, &QComboBox::currentIndexChanged, this, [this]() { sortVideos(); });
	connect(&scanner_, &VideoFolderScanner::batchReady, this, &RecordingVideoPicker::appendVideos);
	connect(&scanner_, &VideoFolderScanner::progress, this, [this](qint64) { updateRecordingCount(); });
	connect(&scanner_, &VideoFolderScanner::finished, this, [this](int unavailable) {
		unavailableRoots_ = unavailable;
		sortVideos(); // respect the current choice, once per completed scan
		progress_->hide();
		updateRecordingCount();
		scheduleThumbnails();
	});
	connect(this, &QDialog::finished, this, [this]() {
		scanner_.cancel();
		thumbnailTimer_->stop();
	});
	startScan();
}

RecordingVideoPicker::~RecordingVideoPicker()
{
	scanner_.cancel();
}

void RecordingVideoPicker::startScan()
{
	scanner_.cancel();
	thumbnailTimer_->stop();
	// Clear pointers before rows: thumbnail deliveries from a previous tab
	// may arrive as soon as control returns to the event loop.
	itemByPath_.clear();
	checkedThumbnails_.clear();
	{
		QSignalBlocker block(grid_);
		grid_->clear();
	}
	addButton_->setEnabled(false);
	unavailableRoots_ = 0;
	QStringList folders;
	if (tabs_->currentIndex() == 0)
		folders = libraryFolders_;
	else
		folders = QStandardPaths::standardLocations(tabs_->currentIndex() == 1
			? QStandardPaths::DownloadLocation : QStandardPaths::MoviesLocation);
	status_->setToolTip(folders.join(QLatin1Char('\n')));
	progress_->show();
	scanner_.start(folders, tabs_->currentIndex() == 0);
	updateRecordingCount();
}

void RecordingVideoPicker::appendVideos(const QVector<ClipInfo> &clips)
{
	const QIcon placeholder = videoPlaceholder(grid_->palette());
	for (const ClipInfo &clip : clips) {
		if (itemByPath_.contains(clip.filePath) || deletedPaths_.contains(scanPathKey(clip.filePath)))
			continue;
		auto *item = new VideoItem(placeholder,
			QStringLiteral("%1\n%2 · %3").arg(clip.fileName, clip.humanSize(), clip.relativeAge()), grid_);
		item->setData(Qt::UserRole, clip.filePath);
		item->setData(kModifiedRole, clip.modified.toMSecsSinceEpoch());
		item->setData(kSizeRole, clip.sizeBytes);
		item->setData(kNameRole, clip.fileName);
		item->setTextAlignment(Qt::AlignHCenter);
		item->setToolTip(QStringLiteral("%1\n%2 · %3").arg(clip.filePath, clip.exactDate(), clip.humanSize()));
		itemByPath_.insert(clip.filePath, item);
	}
	updateRecordingCount();
	scheduleThumbnails();
}

void RecordingVideoPicker::sortVideos()
{
	grid_->setProperty(kOrderProperty, orderBy_->currentData().toInt());
	grid_->sortItems(Qt::AscendingOrder);
	scheduleThumbnails();
}

void RecordingVideoPicker::updateRecordingCount()
{
	const QString count = QStringLiteral("%1 video%2").arg(itemByPath_.size())
		.arg(itemByPath_.size() == 1 ? QString() : QStringLiteral("s"));
	if (scanner_.isRunning()) {
		status_->setText(QStringLiteral("Scanning %1… %2 found").arg(tabs_->tabText(tabs_->currentIndex()), count));
	} else {
		QString text = itemByPath_.isEmpty()
			? QStringLiteral("No videos found. Use Import from folder… to choose another location.") : count;
		if (unavailableRoots_ > 0)
			text += QStringLiteral(" · %1 folder%2 unavailable").arg(unavailableRoots_)
				.arg(unavailableRoots_ == 1 ? QString() : QStringLiteral("s"));
		status_->setText(text);
	}
}

bool RecordingVideoPicker::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == grid_->viewport() && (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
					    event->type() == QEvent::Paint))
		scheduleThumbnails();
	return QDialog::eventFilter(watched, event);
}

void RecordingVideoPicker::scheduleThumbnails()
{
	if (isVisible() && !thumbnailTimer_->isActive())
		thumbnailTimer_->start();
}

void RecordingVideoPicker::loadVisibleThumbnails()
{
	if (!isVisible() || grid_->count() == 0)
		return;
	// Sample visible cells rather than walking every discovered file on each
	// scroll. The grid has uniform cells; half-cell steps also hit partial rows.
	const QSize cell = grid_->gridSize();
	const QSize viewport = grid_->viewport()->size();
	int first = grid_->count(), last = -1;
	for (int y = 0; y < viewport.height(); y += std::max(1, cell.height() / 2))
		for (int x = std::max(1, cell.width() / 4); x < viewport.width(); x += std::max(1, cell.width() / 2)) {
			const QModelIndex index = grid_->indexAt(QPoint(x, y));
			if (index.isValid()) {
				first = std::min(first, index.row());
				last = std::max(last, index.row());
			}
		}
	if (last < 0)
		return;
	const int columns = std::max(1, viewport.width() / std::max(1, cell.width()));
	QVector<int> rows;
	rows.reserve(last - first + 1 + 2 * columns);
	// Current cards get the limited worker slots before off-screen prefetch.
	for (int row = first; row <= last; ++row)
		rows.append(row);
	for (int row = std::max(0, first - columns); row < first; ++row)
		rows.append(row);
	for (int row = last + 1; row <= std::min(grid_->count() - 1, last + columns); ++row)
		rows.append(row);
	for (int row : rows) {
		const QString path = grid_->item(row)->data(Qt::UserRole).toString();
		if (checkedThumbnails_.contains(path))
			continue;
		const QImage image = thumbnails_.cached(path, kThumbnailSize);
		if (!image.isNull()) {
			checkedThumbnails_.insert(path);
			grid_->item(row)->setIcon(QIcon(QPixmap::fromImage(image)));
		} else if (attemptedThumbnails_.contains(path)) {
			checkedThumbnails_.insert(path);
		} else if (pendingThumbnails_.size() < 4) {
			checkedThumbnails_.insert(path);
			attemptedThumbnails_.insert(path);
			if (thumbnails_.ensure(path, kThumbnailSize)) {
				pendingThumbnails_.insert(path);
			} else {
				// A concurrent cache writer may have finished between cached()
				// and ensure(). No worker means no future ready() notification.
				updateThumbnail(path);
			}
		}
	}
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
			for (const QString &removed : gone) {
				deletedPaths_.insert(scanPathKey(removed));
				if (QListWidgetItem *row = itemByPath_.take(removed))
					delete grid_->takeItem(grid_->row(row));
			}
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
		status_->setText(QStringLiteral("This video is no longer available. Choose another video or import from a folder."));
		return;
	}
	accept();
}

void RecordingVideoPicker::updateThumbnail(const QString &path)
{
	pendingThumbnails_.remove(path);
	scheduleThumbnails();
	QListWidgetItem *item = itemByPath_.value(path, nullptr);
	if (!item)
		return;
	const QImage image = thumbnails_.cached(path, kThumbnailSize);
	if (!image.isNull())
		item->setIcon(QIcon(QPixmap::fromImage(image)));
}

} // namespace harpia
