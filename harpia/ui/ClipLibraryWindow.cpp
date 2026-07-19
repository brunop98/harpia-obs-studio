#include "ClipLibraryWindow.hpp"

#include "RecentListWidget.hpp"
#include "model/PresetStore.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QThreadPool>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace harpia {

namespace {

// Item data roles (kClipPathRole = UserRole+1 lives in RecentListWidget.hpp).
constexpr int kFileNameRole = Qt::UserRole + 2;
constexpr int kMetaRole = Qt::UserRole + 3;     // "19 Jul 2026 • 47.7 MB"
constexpr int kPresetRole = Qt::UserRole + 4;
constexpr int kDurationRole = Qt::UserRole + 5; // qint64 ms
constexpr int kIsGifRole = Qt::UserRole + 6;    // bool
constexpr int kIsFavRole = Qt::UserRole + 7;    // bool
constexpr int kIsRecentRole = Qt::UserRole + 8; // bool

// Thumbnails are generated once at this size and scaled down per card, so the
// size slider never forces a re-decode.
constexpr QSize kGenThumb(320, 180);
constexpr int kMaxRecentlyViewed = 40;

// Paints a recording as a rounded card: 16:9 thumbnail on top with the duration
// overlaid and a GIF badge / favorite star / recently-viewed heart, then the
// (elided) file name, date + size, and preset below.
class ClipCardDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	QSize cardSize{220, 180};
	int thumbHeight = 124;
	std::function<void(const QModelIndex &)> onStarClicked;

	QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return cardSize; }

	QRect starRect(const QRect &card) const
	{
		const int s = 24;
		return QRect(card.right() - s - 6, card.top() + 6, s, s);
	}

	void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
	{
		p->save();
		p->setRenderHint(QPainter::Antialiasing, true);

		const QRect card = opt.rect.adjusted(6, 6, -6, -6);
		const bool hover = opt.state & QStyle::State_MouseOver;
		const bool sel = opt.state & QStyle::State_Selected;

		QColor bg = sel ? QColor(0x2b, 0x2f, 0x36) : QColor(0x1f, 0x22, 0x28);
		QColor border = sel ? QColor(0x4c, 0x8b, 0xf5)
				    : (hover ? QColor(0x5a, 0x60, 0x6a) : QColor(0x33, 0x37, 0x3f));
		p->setBrush(bg);
		p->setPen(QPen(border, sel ? 2 : 1));
		p->drawRoundedRect(card, 10, 10);

		// Thumbnail region.
		const QRect thumbRect(card.left() + 1, card.top() + 1, card.width() - 2, thumbHeight);
		p->save();
		QPainterPath clip;
		clip.addRoundedRect(thumbRect, 9, 9);
		p->setClipPath(clip);
		const QPixmap pm = qvariant_cast<QIcon>(idx.data(Qt::DecorationRole)).pixmap(thumbRect.size());
		if (!pm.isNull()) {
			const QPixmap s = pm.scaled(thumbRect.size(), Qt::KeepAspectRatioByExpanding,
						    Qt::SmoothTransformation);
			p->drawPixmap(thumbRect.x() + (thumbRect.width() - s.width()) / 2,
				      thumbRect.y() + (thumbRect.height() - s.height()) / 2, s);
		} else {
			p->fillRect(thumbRect, QColor(0x15, 0x16, 0x1a));
		}
		p->restore();

		// Duration overlay (bottom-right of the thumbnail).
		const QString dur = ClipInfo::durationString(idx.data(kDurationRole).toLongLong());
		if (!dur.isEmpty()) {
			QFont f = opt.font;
			f.setPointSizeF(f.pointSizeF() * 0.9);
			p->setFont(f);
			const QFontMetrics fm(f);
			QRect tb = fm.boundingRect(dur).adjusted(-6, -3, 6, 3);
			tb.moveBottomRight(QPoint(thumbRect.right() - 6, thumbRect.bottom() - 6));
			p->setBrush(QColor(0, 0, 0, 170));
			p->setPen(Qt::NoPen);
			p->drawRoundedRect(tb, 4, 4);
			p->setPen(Qt::white);
			p->drawText(tb, Qt::AlignCenter, dur);
		}

		// GIF badge (top-left of the thumbnail).
		if (idx.data(kIsGifRole).toBool()) {
			QFont f = opt.font;
			f.setBold(true);
			f.setPointSizeF(f.pointSizeF() * 0.85);
			p->setFont(f);
			const QFontMetrics fm(f);
			QRect gb = fm.boundingRect(QStringLiteral("GIF")).adjusted(-6, -3, 6, 3);
			gb.moveTopLeft(QPoint(thumbRect.left() + 6, thumbRect.top() + 6));
			p->setBrush(QColor(0x89, 0x57, 0xe5));
			p->setPen(Qt::NoPen);
			p->drawRoundedRect(gb, 4, 4);
			p->setPen(Qt::white);
			p->drawText(gb, Qt::AlignCenter, QStringLiteral("GIF"));
		}

		// Favorite star (top-right) — shown filled when favorited, faint otherwise.
		{
			const bool fav = idx.data(kIsFavRole).toBool();
			QFont sf = opt.font;
			sf.setPointSize(15);
			p->setFont(sf);
			p->setPen(fav ? QColor(0xf5, 0xc5, 0x18) : QColor(255, 255, 255, 140));
			p->drawText(starRect(card), Qt::AlignCenter,
				    fav ? QString::fromUtf8("\xE2\x98\x85") : QString::fromUtf8("\xE2\x98\x86"));
		}

		// Recently-viewed heart, left of the star.
		if (idx.data(kIsRecentRole).toBool()) {
			QFont hf = opt.font;
			hf.setPointSize(12);
			p->setFont(hf);
			p->setPen(QColor(0xe5, 0x48, 0x4d));
			p->drawText(starRect(card).translated(-24, 1), Qt::AlignCenter,
				    QString::fromUtf8("\xE2\x99\xA5"));
		}

		// Text block.
		const int textTop = thumbRect.bottom() + 6;
		const QRect textRect(card.left() + 10, textTop, card.width() - 20, card.bottom() - textTop - 8);
		QFont nameF = opt.font;
		nameF.setBold(true);
		p->setFont(nameF);
		const QFontMetrics nfm(nameF);
		p->setPen(QColor(0xe6, 0xe6, 0xe6));
		int y = textRect.top() + nfm.ascent();
		p->drawText(textRect.left(), y,
			    nfm.elidedText(idx.data(kFileNameRole).toString(), Qt::ElideRight, textRect.width()));

		p->setFont(opt.font);
		const QFontMetrics sfm(opt.font);
		p->setPen(QColor(0x9a, 0xa0, 0xa8));
		y += sfm.height() + 2;
		p->drawText(textRect.left(), y,
			    sfm.elidedText(idx.data(kMetaRole).toString(), Qt::ElideRight, textRect.width()));

		const QString preset = idx.data(kPresetRole).toString();
		if (!preset.isEmpty() && y + sfm.height() <= textRect.bottom()) {
			y += sfm.height();
			p->drawText(textRect.left(), y,
				    sfm.elidedText(preset, Qt::ElideRight, textRect.width()));
		}
		p->restore();
	}

	bool editorEvent(QEvent *e, QAbstractItemModel *, const QStyleOptionViewItem &opt,
			 const QModelIndex &idx) override
	{
		if (e->type() == QEvent::MouseButtonRelease) {
			auto *me = static_cast<QMouseEvent *>(e);
			const QRect card = opt.rect.adjusted(6, 6, -6, -6);
			if (starRect(card).contains(me->pos()) && onStarClicked) {
				onStarClicked(idx);
				return true; // consume so it doesn't also toggle selection
			}
		}
		return false;
	}
};

} // namespace

ClipLibraryWindow::ClipLibraryWindow(PresetStore &store, QWidget *parent)
	: QWidget(parent, Qt::Window), store_(store)
{
	setWindowTitle(QStringLiteral("Clip Library"));
	resize(860, 620);

	loadPersistentState();

	// Header: totals + a Small/Medium/Large size slider.
	totalsLabel_ = new QLabel(this);
	totalsLabel_->setStyleSheet(QStringLiteral("font-weight:bold;"));

	sizeSlider_ = new QSlider(Qt::Horizontal, this);
	sizeSlider_->setRange(0, 2);
	sizeSlider_->setValue(cardScale_);
	sizeSlider_->setFixedWidth(120);
	sizeSlider_->setPageStep(1);
	connect(sizeSlider_, &QSlider::valueChanged, this, &ClipLibraryWindow::onCardScaleChanged);

	auto *header = new QHBoxLayout;
	header->addWidget(totalsLabel_, 1);
	header->addWidget(new QLabel(QStringLiteral("Size"), this));
	header->addWidget(sizeSlider_);

	grid_ = new RecentListWidget(this); // multi-select + drag-out for free
	grid_->setViewMode(QListView::IconMode);
	grid_->setResizeMode(QListView::Adjust);
	grid_->setMovement(QListView::Static);
	grid_->setUniformItemSizes(true);
	grid_->setSpacing(6);
	grid_->setMouseTracking(true); // enables hover repaint of cards
	grid_->setIconSize(kGenThumb);
	grid_->setContextMenuPolicy(Qt::CustomContextMenu);

	auto *delegate = new ClipCardDelegate(grid_);
	delegate->onStarClicked = [this](const QModelIndex &idx) {
		const QString path = idx.data(kClipPathRole).toString();
		if (path.isEmpty())
			return;
		if (favorites_.contains(path))
			favorites_.remove(path);
		else
			favorites_.insert(path);
		savePersistentState();
		if (QListWidgetItem *it = itemByPath_.value(path, nullptr))
			it->setData(kIsFavRole, favorites_.contains(path));
		grid_->viewport()->update();
	};
	grid_->setItemDelegate(delegate);
	cardDelegate_ = delegate;
	applyCardMetrics();

	connect(grid_, &QListWidget::customContextMenuRequested, this, &ClipLibraryWindow::showContextMenu);
	// Single click selects; double-click (or Enter) opens the recording.
	connect(grid_, &QListWidget::itemDoubleClicked, this, &ClipLibraryWindow::openSelected);
	connect(&thumbnails_, &ThumbnailCache::ready, this, &ClipLibraryWindow::onThumbnailReady);

	auto *refreshBtn = new QPushButton(QStringLiteral("Refresh"), this);
	connect(refreshBtn, &QPushButton::clicked, this, &ClipLibraryWindow::refresh);

	auto *footer = new QHBoxLayout;
	footer->addStretch(1);
	footer->addWidget(refreshBtn);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(header);
	layout->addWidget(grid_, 1);
	layout->addLayout(footer);

	auto *del = new QShortcut(QKeySequence(QKeySequence::Delete), grid_);
	del->setContext(Qt::WidgetShortcut);
	connect(del, &QShortcut::activated, this, &ClipLibraryWindow::deleteSelected);
}

QSize ClipLibraryWindow::thumbSize() const
{
	// Card width per scale; the thumbnail is 16:9 across it.
	const int cardW = cardScale_ == 0 ? 180 : (cardScale_ == 2 ? 300 : 220);
	return QSize(cardW, cardW * 9 / 16);
}

void ClipLibraryWindow::applyCardMetrics()
{
	const QSize thumb = thumbSize();
	const int textBlock = cardScale_ == 0 ? 58 : 66; // room for name + date + preset
	const QSize card(thumb.width() + 12, thumb.height() + textBlock);

	if (auto *d = static_cast<ClipCardDelegate *>(cardDelegate_)) {
		d->cardSize = card;
		d->thumbHeight = thumb.height();
	}
	grid_->setGridSize(card);
	grid_->doItemsLayout();
	grid_->viewport()->update();
}

void ClipLibraryWindow::onCardScaleChanged(int level)
{
	cardScale_ = qBound(0, level, 2);
	savePersistentState();
	applyCardMetrics();
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
	itemByPath_.clear();

	QFileIconProvider iconProvider;
	const QVector<ClipInfo> clips = ClipLibrary::scan(folders(), presetByFolder());

	qint64 totalBytes = 0;
	for (const ClipInfo &clip : clips) {
		totalBytes += clip.sizeBytes;

		auto *item = new QListWidgetItem;
		item->setData(kClipPathRole, clip.filePath);
		item->setData(kFileNameRole, clip.fileName);
		item->setData(kMetaRole, QStringLiteral("%1 • %2").arg(clip.exactDate(), clip.humanSize()));
		item->setData(kPresetRole, clip.presetName);
		item->setData(kIsGifRole, clip.isGif);
		item->setData(kIsFavRole, favorites_.contains(clip.filePath));
		item->setData(kIsRecentRole, recentlyViewed_.contains(clip.filePath));
		item->setData(kDurationRole, durationMs_.value(clip.filePath, 0));
		// Tooltip: full name (may be elided in the card) + relative age.
		item->setToolTip(QStringLiteral("%1\n%2").arg(clip.fileName, clip.relativeAge()));

		const QImage thumb = thumbnails_.cached(clip.filePath, kGenThumb);
		if (!thumb.isNull())
			item->setIcon(QIcon(QPixmap::fromImage(thumb)));
		else {
			item->setIcon(iconProvider.icon(QFileInfo(clip.filePath)));
			thumbnails_.ensure(clip.filePath, kGenThumb);
		}

		itemByPath_.insert(clip.filePath, item);
		grid_->addItem(item);

		if (!clip.isGif && !durationMs_.contains(clip.filePath))
			probeDurationAsync(clip.filePath);
	}

	totalsLabel_->setText(QStringLiteral("%1 recording%2 • %3 total")
				      .arg(clips.size())
				      .arg(clips.size() == 1 ? "" : "s")
				      .arg(QLocale().formattedDataSize(totalBytes, 1,
								       QLocale::DataSizeTraditionalFormat)));
}

void ClipLibraryWindow::probeDurationAsync(const QString &path)
{
	if (durationInFlight_.contains(path))
		return;
	durationInFlight_.insert(path);

	QPointer<ClipLibraryWindow> guard(this);
	QThreadPool::globalInstance()->start(QRunnable::create([guard, path]() {
		const qint64 ms = ThumbnailCache::probeDurationMs(path);
		QMetaObject::invokeMethod(
			qApp,
			[guard, path, ms]() {
				if (!guard)
					return;
				guard->durationInFlight_.remove(path);
				guard->durationMs_.insert(path, ms);
				if (QListWidgetItem *it = guard->itemByPath_.value(path, nullptr)) {
					it->setData(kDurationRole, ms);
					guard->grid_->viewport()->update();
				}
			},
			Qt::QueuedConnection);
	}));
}

void ClipLibraryWindow::onThumbnailReady(const QString &path)
{
	QListWidgetItem *item = itemByPath_.value(path, nullptr);
	if (!item)
		return;
	const QImage thumb = thumbnails_.cached(path, kGenThumb);
	if (!thumb.isNull())
		item->setIcon(QIcon(QPixmap::fromImage(thumb)));
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

void ClipLibraryWindow::markViewed(const QString &path)
{
	recentlyViewed_.insert(path);
	// Bound the set so it doesn't grow forever (order isn't tracked precisely;
	// this just caps memory — the newest additions are what matter most).
	if (recentlyViewed_.size() > kMaxRecentlyViewed) {
		auto it = recentlyViewed_.begin();
		recentlyViewed_.erase(it);
	}
	savePersistentState();
	if (QListWidgetItem *it = itemByPath_.value(path, nullptr))
		it->setData(kIsRecentRole, true);
	grid_->viewport()->update();
}

void ClipLibraryWindow::showContextMenu(const QPoint &pos)
{
	QListWidgetItem *item = grid_->itemAt(pos);
	if (!item)
		return;
	if (!item->isSelected())
		grid_->setCurrentItem(item);

	const QStringList sel = selectedPaths();
	const bool anyFav = std::any_of(sel.begin(), sel.end(),
					[this](const QString &p) { return favorites_.contains(p); });

	QMenu menu(this);
	QAction *openAct = menu.addAction(QStringLiteral("Open"));
	QAction *favAct = menu.addAction(anyFav ? QStringLiteral("Remove from favorites")
						: QStringLiteral("Add to favorites"));
	QAction *copyAct = menu.addAction(QStringLiteral("Copy"));
	QAction *renameAct = menu.addAction(QStringLiteral("Rename…"));
	menu.addSeparator();
	QAction *deleteAct = menu.addAction(QStringLiteral("Delete"));

	QAction *chosen = menu.exec(grid_->viewport()->mapToGlobal(pos));
	if (chosen == openAct)
		openSelected();
	else if (chosen == favAct)
		toggleFavoriteSelected();
	else if (chosen == copyAct)
		copySelected();
	else if (chosen == renameAct)
		renameSelected();
	else if (chosen == deleteAct)
		deleteSelected();
}

void ClipLibraryWindow::toggleFavoriteSelected()
{
	const QStringList sel = selectedPaths();
	if (sel.isEmpty())
		return;
	// If any selected clip is not a favorite, favorite them all; else unfavorite.
	const bool makeFav = std::any_of(sel.begin(), sel.end(),
					 [this](const QString &p) { return !favorites_.contains(p); });
	for (const QString &p : sel) {
		if (makeFav)
			favorites_.insert(p);
		else
			favorites_.remove(p);
		if (QListWidgetItem *it = itemByPath_.value(p, nullptr))
			it->setData(kIsFavRole, favorites_.contains(p));
	}
	savePersistentState();
	grid_->viewport()->update();
}

void ClipLibraryWindow::openSelected()
{
	for (const QString &path : selectedPaths()) {
		markViewed(path);
		QDesktopServices::openUrl(QUrl::fromLocalFile(path));
	}
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
	mime->setUrls(urls);
	mime->setText(paths.join(QLatin1Char('\n')));
	QApplication::clipboard()->setMimeData(mime);
}

void ClipLibraryWindow::renameSelected()
{
	const QStringList paths = selectedPaths();
	if (paths.size() != 1)
		return;

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
	if (!QFile::rename(paths.front(), target)) {
		QMessageBox::warning(this, QStringLiteral("Rename"), QStringLiteral("Could not rename the file."));
	} else {
		// Favorites/recently-viewed are keyed by absolute path — migrate them
		// so a starred clip keeps its star across a rename.
		const QString oldAbs = QFileInfo(paths.front()).absoluteFilePath();
		const QString newAbs = QFileInfo(target).absoluteFilePath();
		if (favorites_.remove(oldAbs))
			favorites_.insert(newAbs);
		if (recentlyViewed_.remove(oldAbs))
			recentlyViewed_.insert(newAbs);
		savePersistentState();
	}

	refresh();
}

void ClipLibraryWindow::deleteSelected()
{
	const QStringList paths = selectedPaths();
	if (paths.isEmpty())
		return;

	const QString prompt =
		paths.size() == 1
			? QStringLiteral("Move \"%1\" to the recycle bin?").arg(QFileInfo(paths.front()).fileName())
			: QStringLiteral("Move %1 clips to the recycle bin?").arg(paths.size());
	if (QMessageBox::question(this, QStringLiteral("Delete"), prompt) != QMessageBox::Yes)
		return;

	for (const QString &path : paths) {
		QFile f(path);
		if (!f.moveToTrash())
			f.remove();
		favorites_.remove(path);
		recentlyViewed_.remove(path);
	}
	savePersistentState();
	refresh();
}

void ClipLibraryWindow::loadPersistentState()
{
	QSettings s(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	cardScale_ = qBound(0, s.value(QStringLiteral("library/cardScale"), 1).toInt(), 2);
	const QStringList favs = s.value(QStringLiteral("library/favorites")).toStringList();
	const QStringList recent = s.value(QStringLiteral("library/recentlyViewed")).toStringList();
	favorites_ = QSet<QString>(favs.begin(), favs.end());
	recentlyViewed_ = QSet<QString>(recent.begin(), recent.end());
}

void ClipLibraryWindow::savePersistentState()
{
	QSettings s(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	s.setValue(QStringLiteral("library/cardScale"), cardScale_);
	s.setValue(QStringLiteral("library/favorites"), QStringList(favorites_.begin(), favorites_.end()));
	s.setValue(QStringLiteral("library/recentlyViewed"),
		   QStringList(recentlyViewed_.begin(), recentlyViewed_.end()));
}

} // namespace harpia
