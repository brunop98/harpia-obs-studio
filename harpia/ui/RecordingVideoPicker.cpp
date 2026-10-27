#include "RecordingVideoPicker.hpp"

#include "library/ClipLibrary.hpp"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QVBoxLayout>

namespace harpia {

namespace {
const QSize kThumbnailSize(192, 108);

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
	status_->setText(itemByPath_.isEmpty()
		? QStringLiteral("No recorded videos found. Use Import from folder… to choose a video on your computer.")
		: QStringLiteral("%1 recorded video%2").arg(itemByPath_.size()).arg(itemByPath_.size() == 1 ? QString() : QStringLiteral("s")));
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
