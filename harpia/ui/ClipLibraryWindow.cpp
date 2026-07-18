#include "ClipLibraryWindow.hpp"

#include "RecentListWidget.hpp"
#include "model/PresetStore.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QPushButton>
#include <QShortcut>
#include <QUrl>
#include <QVBoxLayout>

namespace harpia {

namespace {
constexpr QSize kThumbSize(180, 110);
}

ClipLibraryWindow::ClipLibraryWindow(PresetStore &store, QWidget *parent)
	: QWidget(parent, Qt::Window), store_(store)
{
	setWindowTitle(QStringLiteral("Clip Library"));
	resize(760, 560);

	grid_ = new RecentListWidget(this);
	grid_->setViewMode(QListView::IconMode);
	grid_->setResizeMode(QListView::Adjust);
	grid_->setMovement(QListView::Static);
	grid_->setIconSize(kThumbSize);
	grid_->setGridSize(kThumbSize + QSize(40, 60));
	grid_->setWordWrap(true);
	grid_->setSpacing(10);
	grid_->setUniformItemSizes(true);
	grid_->setContextMenuPolicy(Qt::CustomContextMenu);

	connect(grid_, &QListWidget::customContextMenuRequested, this, &ClipLibraryWindow::showContextMenu);
	connect(grid_, &QListWidget::itemActivated, this, &ClipLibraryWindow::openSelected);

	auto *refreshBtn = new QPushButton(QStringLiteral("Refresh"), this);
	connect(refreshBtn, &QPushButton::clicked, this, &ClipLibraryWindow::refresh);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(grid_, 1);
	layout->addWidget(refreshBtn);

	// Delete key removes the selected clips.
	auto *del = new QShortcut(QKeySequence(QKeySequence::Delete), grid_);
	del->setContext(Qt::WidgetShortcut);
	connect(del, &QShortcut::activated, this, &ClipLibraryWindow::deleteSelected);
}

void ClipLibraryWindow::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	refresh();
}

QStringList ClipLibraryWindow::folders() const
{
	QStringList out;
	for (const Preset &p : store_.presets()) {
		const QString f = QString::fromStdString(p.outputFolder);
		if (!f.isEmpty() && !out.contains(f))
			out << f;
	}
	return out;
}

ClipLibrary::PresetByFolder ClipLibraryWindow::presetByFolder() const
{
	ClipLibrary::PresetByFolder map;
	for (const Preset &p : store_.presets()) {
		if (!p.outputFolder.empty())
			map.insert(QString::fromStdString(p.outputFolder), QString::fromStdString(p.name));
	}
	return map;
}

void ClipLibraryWindow::refresh()
{
	grid_->clear();

	QFileIconProvider iconProvider;
	const QVector<ClipInfo> clips = ClipLibrary::scan(folders(), presetByFolder());

	for (const ClipInfo &clip : clips) {
		QString label = QStringLiteral("%1\n%2 · %3").arg(clip.fileName, clip.relativeAge(), clip.humanSize());
		if (!clip.presetName.isEmpty())
			label += QStringLiteral("\n%1").arg(clip.presetName);

		auto *item = new QListWidgetItem(label);
		item->setData(kClipPathRole, clip.filePath);
		item->setToolTip(clip.filePath);
		item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);

		// Prefer a real thumbnail; fall back to the platform's file icon until
		// thumbnail generation lands.
		const QImage thumb = thumbnails_.thumbnailFor(clip.filePath, kThumbSize);
		if (!thumb.isNull())
			item->setIcon(QIcon(QPixmap::fromImage(thumb)));
		else
			item->setIcon(iconProvider.icon(QFileInfo(clip.filePath)));

		grid_->addItem(item);
	}
}

QStringList ClipLibraryWindow::selectedPaths() const
{
	QStringList paths;
	for (QListWidgetItem *item : grid_->selectedItems()) {
		const QString p = item->data(kClipPathRole).toString();
		if (!p.isEmpty())
			paths << p;
	}
	return paths;
}

void ClipLibraryWindow::showContextMenu(const QPoint &pos)
{
	QListWidgetItem *item = grid_->itemAt(pos);
	if (!item)
		return;
	if (!item->isSelected())
		grid_->setCurrentItem(item);

	QMenu menu(this);
	QAction *openAct = menu.addAction(QStringLiteral("Open"));
	QAction *copyAct = menu.addAction(QStringLiteral("Copy"));
	QAction *renameAct = menu.addAction(QStringLiteral("Rename…"));
	menu.addSeparator();
	QAction *deleteAct = menu.addAction(QStringLiteral("Delete"));

	QAction *chosen = menu.exec(grid_->viewport()->mapToGlobal(pos));
	if (chosen == openAct)
		openSelected();
	else if (chosen == copyAct)
		copySelected();
	else if (chosen == renameAct)
		renameSelected();
	else if (chosen == deleteAct)
		deleteSelected();
}

void ClipLibraryWindow::openSelected()
{
	for (const QString &path : selectedPaths())
		QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void ClipLibraryWindow::copySelected()
{
	const QStringList paths = selectedPaths();
	if (paths.isEmpty())
		return;

	QList<QUrl> urls;
	for (const QString &p : paths)
		urls << QUrl::fromLocalFile(p);

	auto *mime = new QMimeData();
	mime->setUrls(urls);                       // paste as file(s) into a file manager
	mime->setText(paths.join(QLatin1Char('\n'))); // and as text into editors
	QApplication::clipboard()->setMimeData(mime);
}

void ClipLibraryWindow::renameSelected()
{
	const QStringList paths = selectedPaths();
	if (paths.size() != 1)
		return; // rename operates on a single clip

	QFileInfo fi(paths.front());
	bool ok = false;
	const QString newBase = QInputDialog::getText(this, QStringLiteral("Rename"), QStringLiteral("New name:"),
						      QLineEdit::Normal, fi.completeBaseName(), &ok);
	if (!ok || newBase.trimmed().isEmpty())
		return;

	QString target = fi.absolutePath() + QLatin1Char('/') + newBase.trimmed();
	if (!fi.suffix().isEmpty())
		target += QLatin1Char('.') + fi.suffix();

	if (QFileInfo::exists(target)) {
		QMessageBox::warning(this, QStringLiteral("Rename"),
				     QStringLiteral("A file with that name already exists."));
		return;
	}
	if (!QFile::rename(paths.front(), target))
		QMessageBox::warning(this, QStringLiteral("Rename"), QStringLiteral("Could not rename the file."));

	refresh();
}

void ClipLibraryWindow::deleteSelected()
{
	const QStringList paths = selectedPaths();
	if (paths.isEmpty())
		return;

	const QString prompt = paths.size() == 1
				       ? QStringLiteral("Move \"%1\" to the recycle bin?").arg(QFileInfo(paths.front()).fileName())
				       : QStringLiteral("Move %1 clips to the recycle bin?").arg(paths.size());
	if (QMessageBox::question(this, QStringLiteral("Delete"), prompt) != QMessageBox::Yes)
		return;

	for (const QString &path : paths) {
		QFile f(path);
		// Prefer the recycle bin; fall back to a hard delete if unsupported.
		if (!f.moveToTrash())
			f.remove();
	}
	refresh();
}

} // namespace harpia
