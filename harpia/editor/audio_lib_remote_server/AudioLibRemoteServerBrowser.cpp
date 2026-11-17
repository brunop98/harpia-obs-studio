#include "AudioLibRemoteServerBrowser.hpp"

#include "AudioLibRemoteServer.hpp"
#include "../AudioPreview.hpp"
#include "../EditorFileDialog.hpp"
#include "../MediaFiles.hpp"
#include "../VoiceoverMixer.hpp"
#include "../../ui/EditorLog.hpp"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkReply>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace harpia {
namespace {

constexpr qint64 kPreviewMs = 15000;
constexpr int kSoundRole = Qt::UserRole;

bool readDuration(const QString &text, std::optional<double> &duration)
{
	QString seconds = text.trimmed();
	if (seconds.isEmpty()) {
		duration.reset();
		return true;
	}
	seconds.replace(QLatin1Char(','), QLatin1Char('.'));
	bool ok = false;
	const double value = seconds.toDouble(&ok);
	if (!ok || !std::isfinite(value) || value < 0)
		return false;
	duration = value;
	return true;
}

class AudioLibRemoteServerResults : public QTreeWidget {
public:
	using QTreeWidget::QTreeWidget;

protected:
	QStringList mimeTypes() const override { return {AudioLibRemoteServerSoundDrag::format()}; }
	QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override
	{
		if (items.isEmpty())
			return nullptr;
		return new AudioLibRemoteServerSoundDrag(items.first()->data(0, kSoundRole).value<AudioLibRemoteServerSound>());
	}
};

} // namespace

AudioLibRemoteServerBrowser::AudioLibRemoteServerBrowser(const QString &action, bool draggable, QWidget *parent)
	: QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 6, 0, 0);
	auto *searchRow = new QHBoxLayout;
	query_ = new QLineEdit(this);
	query_->setObjectName(QStringLiteral("xa1Search"));
	query_->setAccessibleName(QStringLiteral("Search XA1 audio"));
	query_->setPlaceholderText(QStringLiteral("Search sounds by name or folder…"));
	query_->setToolTip(QStringLiteral("Use double quotes for a whole word or phrase."));
	query_->setClearButtonEnabled(true);
	searchRow->addWidget(query_, 1);
	auto *filterToggle = new QPushButton(QStringLiteral("Filters"), this);
	filterToggle->setCheckable(true);
	filterToggle->setAutoDefault(false);
	searchRow->addWidget(filterToggle);
	auto *connection = new QPushButton(QStringLiteral("Connection…"), this);
	connection->setAutoDefault(false);
	searchRow->addWidget(connection);
	layout->addLayout(searchRow);

	auto *sortRow = new QHBoxLayout;
	auto *sortLabel = new QLabel(QStringLiteral("Order by"), this);
	sort_ = new QComboBox(this);
	sort_->setObjectName(QStringLiteral("xa1OrderBy"));
	sortLabel->setBuddy(sort_);
	sort_->setAccessibleName(sortLabel->text());
	sort_->addItem(QStringLiteral("Name"), QStringLiteral("name"));
	sort_->addItem(QStringLiteral("Duration"), QStringLiteral("duration"));
	sort_->addItem(QStringLiteral("Path"), QStringLiteral("path"));
	direction_ = new QComboBox(this);
	direction_->setObjectName(QStringLiteral("xa1Order"));
	direction_->setAccessibleName(QStringLiteral("Sort direction"));
	direction_->addItems({QStringLiteral("Ascending"), QStringLiteral("Descending")});
	auto *refresh = new QPushButton(QStringLiteral("Refresh"), this);
	refresh->setAutoDefault(false);
	sortRow->addWidget(sortLabel);
	sortRow->addWidget(sort_);
	sortRow->addWidget(direction_);
	sortRow->addStretch();
	sortRow->addWidget(refresh);
	layout->addLayout(sortRow);
	buildFilters(layout, filterToggle);

	results_ = new AudioLibRemoteServerResults(this);
	results_->setObjectName(QStringLiteral("xa1Results"));
	results_->setHeaderLabels({QStringLiteral("Sound"), QStringLiteral("Duration"), QStringLiteral("Folder")});
	results_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	results_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	results_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
	results_->setRootIsDecorated(false);
	results_->setUniformRowHeights(true);
	results_->setAlternatingRowColors(true);
	if (draggable) {
		results_->setDragDropMode(QAbstractItemView::DragOnly);
		results_->setDefaultDropAction(Qt::CopyAction);
		results_->setToolTip(QStringLiteral("Drag a sound onto an audio track or add it at the playhead."));
	}
	layout->addWidget(results_, 1);

	auto *navigation = new QHBoxLayout;
	status_ = new QLabel(this);
	status_->setTextFormat(Qt::PlainText);
	status_->setWordWrap(true);
	previous_ = new QPushButton(QStringLiteral("Previous"), this);
	next_ = new QPushButton(QStringLiteral("Next"), this);
	previous_->setAutoDefault(false);
	next_->setAutoDefault(false);
	navigation->addWidget(status_, 1);
	navigation->addWidget(previous_);
	navigation->addWidget(next_);
	layout->addLayout(navigation);

	auto *buttons = new QHBoxLayout;
	previewButton_ = new QPushButton(QStringLiteral("Preview"), this);
	previewButton_->setToolTip(QStringLiteral("Listen to the first 15 seconds of the selected sound."));
	action_ = new QPushButton(action, this);
	copyErrorDetails_ = new QPushButton(QStringLiteral("Copy error details"), this);
	copyErrorDetails_->setObjectName(QStringLiteral("xa1CopyErrorDetails"));
	copyErrorDetails_->setAutoDefault(false);
	copyErrorDetails_->hide();
	previewButton_->setAutoDefault(false);
	action_->setAutoDefault(false);
	buttons->addWidget(previewButton_);
	buttons->addWidget(copyErrorDetails_);
	buttons->addStretch();
	buttons->addWidget(action_);
	layout->addLayout(buttons);

	searchDelay_ = new QTimer(this);
	searchDelay_->setInterval(250);
	searchDelay_->setSingleShot(true);
	preview_ = new AudioPreview(this);
	preview_->setVolume(0.9);
	previewTimer_ = new QTimer(this);
	previewTimer_->setInterval(50);
	connect(previewTimer_, &QTimer::timeout, this, [this]() {
		if (!preview_->isPlaying() || preview_->positionMs() >= preview_->durationMs())
			stopPreview();
	});

	connect(connection, &QPushButton::clicked, this, &AudioLibRemoteServerBrowser::editConnection);
	connect(refresh, &QPushButton::clicked, this, [this]() { searchPage(offset_); });
	connect(searchDelay_, &QTimer::timeout, this, [this]() { searchPage(0); });
	connect(query_, &QLineEdit::returnPressed, this, [this]() { searchPage(0); });
	connect(query_, &QLineEdit::textChanged, this, &AudioLibRemoteServerBrowser::scheduleSearch);
	connect(sort_, &QComboBox::currentIndexChanged, this, [this]() { searchPage(0); });
	connect(direction_, &QComboBox::currentIndexChanged, this, [this]() { searchPage(0); });
	connect(previous_, &QPushButton::clicked, this, [this]() { searchPage(offset_ - AudioLibRemoteServer::PageSize); });
	connect(next_, &QPushButton::clicked, this, [this]() { searchPage(offset_ + AudioLibRemoteServer::PageSize); });
	connect(&AudioLibRemoteServer::shared(), &AudioLibRemoteServer::connectionChanged, this, [this]() {
		offset_ = 0;
		if (isVisible())
			searchPage(0);
	});
	connect(results_, &QTreeWidget::currentItemChanged, this, [this]() {
		stopPreview();
		updateActions();
	});
	connect(previewButton_, &QPushButton::clicked, this, &AudioLibRemoteServerBrowser::togglePreview);
	connect(copyErrorDetails_, &QPushButton::clicked, this, [this]() {
		QGuiApplication::clipboard()->setText(errorReport_);
		copyErrorDetails_->setText(QStringLiteral("Copied"));
		QTimer::singleShot(1500, copyErrorDetails_, [button = copyErrorDetails_]() {
			button->setText(QStringLiteral("Copy error details"));
		});
	});
	connect(results_, &QTreeWidget::itemDoubleClicked, action_, &QPushButton::click);
	connect(action_, &QPushButton::clicked, this, [this]() {
		const AudioLibRemoteServerSound sound = selectedSound();
		if (sound.downloadUrl.isEmpty())
			return;
		stopPreview();
		emit soundChosen(sound);
	});
	updateActions();
}

void AudioLibRemoteServerBrowser::buildFilters(QVBoxLayout *layout, QPushButton *toggle)
{
	auto *filters = new QWidget(this);
	auto *grid = new QGridLayout(filters);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setColumnStretch(0, 1);
	grid->setColumnStretch(1, 1);

	nameFilter_ = new QLineEdit(filters);
	nameFilter_->setObjectName(QStringLiteral("xa1FilterName"));
	nameFilter_->setClearButtonEnabled(true);
	auto *nameLabel = new QLabel(QStringLiteral("File name contains"), filters);
	nameLabel->setBuddy(nameFilter_);
	grid->addWidget(nameLabel, 0, 0);
	grid->addWidget(nameFilter_, 1, 0);

	pathFilter_ = new QLineEdit(filters);
	pathFilter_->setObjectName(QStringLiteral("xa1FilterPath"));
	pathFilter_->setClearButtonEnabled(true);
	auto *pathLabel = new QLabel(QStringLiteral("Path contains"), filters);
	pathLabel->setBuddy(pathFilter_);
	grid->addWidget(pathLabel, 0, 1);
	grid->addWidget(pathFilter_, 1, 1);

	extensionFilter_ = new QComboBox(filters);
	extensionFilter_->setObjectName(QStringLiteral("xa1FilterExtension"));
	extensionFilter_->addItem(QStringLiteral("Any extension"));
	QStringList extensions = audioFileExtensions();
	extensions.sort();
	for (const QString &extension : extensions)
		extensionFilter_->addItem(extension.toUpper(), extension);
	auto *extensionLabel = new QLabel(QStringLiteral("Extension"), filters);
	extensionLabel->setBuddy(extensionFilter_);
	grid->addWidget(extensionLabel, 2, 0);
	grid->addWidget(extensionFilter_, 3, 0);

	minDurationFilter_ = new QLineEdit(filters);
	minDurationFilter_->setObjectName(QStringLiteral("xa1FilterMinDuration"));
	minDurationFilter_->setAccessibleName(QStringLiteral("Minimum duration in seconds"));
	minDurationFilter_->setPlaceholderText(QStringLiteral("Min"));
	maxDurationFilter_ = new QLineEdit(filters);
	maxDurationFilter_->setObjectName(QStringLiteral("xa1FilterMaxDuration"));
	maxDurationFilter_->setAccessibleName(QStringLiteral("Maximum duration in seconds"));
	maxDurationFilter_->setPlaceholderText(QStringLiteral("Max"));
	auto *range = new QHBoxLayout;
	range->addWidget(minDurationFilter_);
	range->addWidget(maxDurationFilter_);
	auto *durationLabel = new QLabel(QStringLiteral("Duration (seconds)"), filters);
	durationLabel->setBuddy(minDurationFilter_);
	grid->addWidget(durationLabel, 2, 1);
	grid->addLayout(range, 3, 1);

	playableFilter_ = new QCheckBox(QStringLiteral("XA1 player formats only"), filters);
	playableFilter_->setObjectName(QStringLiteral("xa1FilterPlayable"));
	grid->addWidget(playableFilter_, 4, 0);
	auto *clear = new QPushButton(QStringLiteral("Clear Filters"), filters);
	clear->setAutoDefault(false);
	clear->setEnabled(false);
	grid->addWidget(clear, 4, 1, Qt::AlignRight);

	const QList<QLineEdit *> fields = {nameFilter_, pathFilter_, minDurationFilter_, maxDurationFilter_};
	const auto changed = [this, toggle, clear, fields]() {
		int active = extensionFilter_->currentIndex() != 0;
		active += playableFilter_->isChecked();
		for (const auto *field : fields)
			active += !field->text().trimmed().isEmpty();
		toggle->setText(active ? QStringLiteral("Filters (%1)").arg(active) : QStringLiteral("Filters"));
		clear->setEnabled(active > 0);
		scheduleSearch();
	};
	for (auto *field : fields) {
		connect(field, &QLineEdit::textChanged, this, changed);
		connect(field, &QLineEdit::returnPressed, this, [this]() { searchPage(0); });
	}
	connect(extensionFilter_, &QComboBox::currentIndexChanged, this, changed);
	connect(playableFilter_, &QCheckBox::toggled, this, changed);
	connect(clear, &QPushButton::clicked, this, [this, fields]() {
		for (auto *field : fields)
			field->clear();
		extensionFilter_->setCurrentIndex(0);
		playableFilter_->setChecked(false);
	});
	connect(toggle, &QPushButton::toggled, filters, &QWidget::setVisible);
	filters->hide();
	layout->addWidget(filters);
}

AudioLibRemoteServerBrowser::~AudioLibRemoteServerBrowser()
{
	cancelSearch();
	stopPreview();
}

void AudioLibRemoteServerBrowser::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	searchPage(offset_);
}

void AudioLibRemoteServerBrowser::hideEvent(QHideEvent *event)
{
	searchDelay_->stop();
	cancelSearch();
	stopPreview();
	QWidget::hideEvent(event);
}

void AudioLibRemoteServerBrowser::focusSearch()
{
	query_->setFocus(Qt::OtherFocusReason);
}

void AudioLibRemoteServerBrowser::editConnection()
{
	auto &catalog = AudioLibRemoteServer::shared();
	QDialog dialog(this);
	dialog.setWindowTitle(QStringLiteral("XA1 connection"));
	dialog.setWindowModality(Qt::WindowModal);
	auto *form = new QFormLayout(&dialog);
	auto *address = new QLineEdit(catalog.address().toString(), &dialog);
	address->setPlaceholderText(QStringLiteral("https://XA1-address:8443"));
	form->addRow(QStringLiteral("Server address"), address);
	auto *password = new QLineEdit(&dialog);
	password->setEchoMode(QLineEdit::Password);
	auto *forgetPassword = new QPushButton(QStringLiteral("Forget"), &dialog);
	forgetPassword->setAutoDefault(false);
	forgetPassword->setToolTip(QStringLiteral("Remove the saved password when you save this connection."));
	auto *passwordRow = new QHBoxLayout;
	passwordRow->addWidget(password, 1);
	passwordRow->addWidget(forgetPassword);
	form->addRow(QStringLiteral("XA1 password"), passwordRow);
	bool forget = false;
	const auto canKeepPassword = [&]() {
		return !forget && !catalog.password().isEmpty() &&
		       AudioLibRemoteServer::serverAddress(address->text()) == catalog.address();
	};
	const auto updatePassword = [&]() {
		const bool keep = canKeepPassword();
		password->setPlaceholderText(keep ? QStringLiteral("Leave blank to keep current password")
						  : QStringLiteral("Optional"));
		forgetPassword->setEnabled(keep);
	};
	connect(address, &QLineEdit::textChanged, &dialog, updatePassword);
	connect(forgetPassword, &QPushButton::clicked, &dialog, [&]() {
		forget = true;
		password->clear();
		updatePassword();
	});
	updatePassword();

	QList<QSslCertificate> certificates = catalog.certificates();
	QString certificateName = catalog.certificateName();
	auto *certificate = new QPushButton(&dialog);
	certificate->setAutoDefault(false);
	const auto updateCertificate = [&]() {
		QString label = certificateName;
		if (label.isEmpty() && !certificates.isEmpty())
			label = certificates.first().subjectInfo(QSslCertificate::CommonName).value(0);
		certificate->setText(label.isEmpty() ? QStringLiteral("Choose certificate…") : label);
	};
	updateCertificate();
	certificate->setToolTip(QStringLiteral("Choose the certificate Harpia uses for this connection."));
	auto *clear = new QPushButton(QStringLiteral("Use system trust"), &dialog);
	clear->setAutoDefault(false);
	clear->setEnabled(!certificates.isEmpty());
	auto *caRow = new QHBoxLayout;
	caRow->addWidget(certificate);
	caRow->addWidget(clear);
	form->addRow(QStringLiteral("XA1 CA certificate"), caRow);
	connect(certificate, &QPushButton::clicked, &dialog, [&]() {
		const QString file = editor_dialogs::getOpenFileName(
			&dialog, QStringLiteral("Select the XA1 network CA certificate"), {},
			QStringLiteral("Certificates (*.cer *.crt *.pem);;All files (*)"));
		if (file.isEmpty())
			return;
		QFile cert(file);
		QList<QSslCertificate> loaded;
		if (cert.open(QIODevice::ReadOnly)) {
			const QByteArray bytes = cert.readAll();
			loaded = QSslCertificate::fromData(bytes, QSsl::Pem);
			if (loaded.isEmpty())
				loaded = QSslCertificate::fromData(bytes, QSsl::Der);
		}
		if (loaded.isEmpty()) {
			QMessageBox::warning(&dialog, dialog.windowTitle(), QStringLiteral("Could not read that CA certificate."));
			return;
		}
		certificates = std::move(loaded);
		certificateName = QFileInfo(file).fileName();
		updateCertificate();
		clear->setEnabled(true);
	});
	connect(clear, &QPushButton::clicked, &dialog, [&]() {
		certificates.clear();
		certificateName.clear();
		updateCertificate();
		clear->setEnabled(false);
	});
	auto *hint = new QLabel(QStringLiteral("Use the address shown in Servidor XA1. For the LAN certificate, "
					      "select certificado-rede-xa1.cer."), &dialog);
	hint->setWordWrap(true);
	form->addRow(hint);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
		const QUrl url = AudioLibRemoteServer::serverAddress(address->text());
		if (url.isEmpty()) {
			QMessageBox::warning(&dialog, dialog.windowTitle(),
					     QStringLiteral("Enter the HTTPS address shown in Servidor XA1."));
			return;
		}
		const QString secret = password->text().isEmpty() && canKeepPassword() ? catalog.password()
										     : password->text();
		if (!catalog.setConnection(url, certificates, secret, certificateName)) {
			QMessageBox::warning(&dialog, dialog.windowTitle(),
					     QStringLiteral("Could not save the XA1 connection for this user."));
			return;
		}
		dialog.accept();
	});
	form->addRow(buttons);
	dialog.setMinimumWidth(440);
	address->setFocus();
	dialog.exec();
}

void AudioLibRemoteServerBrowser::cancelSearch()
{
	const QPointer<QNetworkReply> reply = searchReply_;
	searchReply_.clear();
	if (reply)
		reply->abort();
}

void AudioLibRemoteServerBrowser::scheduleSearch()
{
	cancelSearch();
	results_->clear();
	total_ = offset_ = 0;
	updateActions();
	searchDelay_->start();
}

void AudioLibRemoteServerBrowser::searchPage(int offset)
{
	searchDelay_->stop();
	cancelSearch();
	stopPreview();
	results_->clear();
	setErrorReport({});
	offset_ = std::max(0, offset);
	total_ = 0;

	AudioLibRemoteServer::SearchOptions options;
	options.query = query_->text();
	options.name = nameFilter_->text().trimmed();
	options.path = pathFilter_->text().trimmed();
	options.extension = extensionFilter_->currentData().toString();
	options.playableOnly = playableFilter_->isChecked();
	options.orderBy = sort_->currentData().toString();
	options.descending = direction_->currentIndex() == 1;
	QString filterError;
	if (!readDuration(minDurationFilter_->text(), options.minDuration) ||
	    !readDuration(maxDurationFilter_->text(), options.maxDuration))
		filterError = QStringLiteral("Enter non-negative durations in seconds.");
	else if (options.minDuration && options.maxDuration && *options.minDuration > *options.maxDuration)
		filterError = QStringLiteral("Minimum duration must be at most the maximum.");

	auto &catalog = AudioLibRemoteServer::shared();
	if (!filterError.isEmpty() || catalog.address().isEmpty()) {
		pageStatus_ = !filterError.isEmpty() ? filterError
						    : QStringLiteral("Set up the XA1 in Connection… to browse sounds.");
		status_->setText(pageStatus_);
		updateActions();
		return;
	}
	auto *reply = catalog.search(options, offset_);
	searchReply_ = reply;
	pageStatus_ = QStringLiteral("Searching the XA1…");
	status_->setText(pageStatus_);
	updateActions();
	connect(reply, &QNetworkReply::finished, this, [this, reply, &catalog]() {
		if (searchReply_ != reply)
			return;
		searchReply_.clear();
		AudioLibRemoteServer::Failure failure;
		const AudioLibRemoteServer::Page page = catalog.readPage(*reply, failure);
		total_ = page.total;
		if (failure.message.isEmpty() && offset_ > 0 && offset_ >= total_) {
			searchPage(total_ == 0 ? 0 : ((total_ - 1) / AudioLibRemoteServer::PageSize) * AudioLibRemoteServer::PageSize);
			return;
		}
		for (const AudioLibRemoteServerSound &sound : page.sounds) {
			const int slash = sound.path.lastIndexOf(QLatin1Char('/'));
			const QString folder = slash < 0 ? QStringLiteral("Internal storage") : sound.path.left(slash);
			QString time = QStringLiteral("—");
			if (sound.durationSeconds >= 0) {
				time = QStringLiteral("%1 s").arg(sound.durationSeconds, 0, 'f', 2);
				if (!sound.durationExact)
					time.prepend(QLatin1Char('~'));
			}
			auto *row = new QTreeWidgetItem(results_, {sound.name, time, folder});
			row->setData(0, kSoundRole, QVariant::fromValue(sound));
			row->setToolTip(0, sound.name);
			row->setToolTip(2, sound.path);
			row->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
		}
		if (!failure.message.isEmpty())
			pageStatus_ = failure.message;
		else if (total_ == 0)
			pageStatus_ = QStringLiteral("No results. Try a different search.");
		else
			pageStatus_ = QStringLiteral("Page %1 of %2 · %3 sounds")
					      .arg(1 + offset_ / AudioLibRemoteServer::PageSize)
					      .arg(1 + (total_ - 1) / AudioLibRemoteServer::PageSize)
					      .arg(page.sounds.size());
		status_->setText(pageStatus_);
		setErrorReport(failure.report);
		if (results_->topLevelItemCount() > 0)
			results_->setCurrentItem(results_->topLevelItem(0));
		updateActions();
	});
}

AudioLibRemoteServerSound AudioLibRemoteServerBrowser::selectedSound() const
{
	const auto *row = results_->currentItem();
	return row ? row->data(0, kSoundRole).value<AudioLibRemoteServerSound>() : AudioLibRemoteServerSound{};
}

void AudioLibRemoteServerBrowser::updateActions()
{
	const bool selected = results_->currentItem() != nullptr;
	previewButton_->setEnabled(selected);
	action_->setEnabled(selected);
	previous_->setEnabled(!searchReply_ && offset_ > 0);
	next_->setEnabled(!searchReply_ && offset_ + AudioLibRemoteServer::PageSize < total_);
}

void AudioLibRemoteServerBrowser::setErrorReport(const QString &report)
{
	errorReport_ = report;
	copyErrorDetails_->setText(QStringLiteral("Copy error details"));
	copyErrorDetails_->setVisible(!report.isEmpty());
	if (!report.isEmpty())
		editorLog(EditorLogLevel::Error, QStringLiteral("XA1"), report);
}

void AudioLibRemoteServerBrowser::stopPreview()
{
	++previewGeneration_;
	const QPointer<QNetworkReply> reply = previewReply_;
	previewReply_.clear();
	if (reply)
		reply->abort();
	previewTimer_->stop();
	preview_->clear();
	previewing_ = false;
	previewButton_->setText(QStringLiteral("Preview"));
	status_->setText(pageStatus_);
}

void AudioLibRemoteServerBrowser::togglePreview()
{
	if (previewing_) {
		stopPreview();
		return;
	}
	const AudioLibRemoteServerSound sound = selectedSound();
	if (sound.downloadUrl.isEmpty())
		return;
	setErrorReport({});
	const auto temporary = std::make_shared<QTemporaryDir>();
	if (!temporary->isValid()) {
		status_->setText(QStringLiteral("Could not create a temporary folder for the preview."));
		return;
	}
	previewing_ = true;
	previewButton_->setText(QStringLiteral("Stop Preview"));
	status_->setText(QStringLiteral("Loading preview…"));
	const quint64 generation = ++previewGeneration_;
	QString error;
	previewReply_ = AudioLibRemoteServer::shared().download(sound, temporary->path(), this,
		[this, temporary, generation](const QString &file, const AudioLibRemoteServer::Failure &failure) {
			if (generation != previewGeneration_)
				return;
			previewReply_.clear();
			if (file.isEmpty()) {
				stopPreview();
				status_->setText(failure.message);
				setErrorReport(failure.report);
				return;
			}
			auto samples = std::make_shared<std::vector<float>>();
			auto *worker = QThread::create([temporary, file, samples]() {
				*samples = VoiceoverMixer::decodePreview(file, kPreviewMs);
			});
			connect(worker, &QThread::finished, worker, &QObject::deleteLater);
			connect(worker, &QThread::finished, this, [this, samples, generation]() {
				if (generation != previewGeneration_)
					return;
				if (samples->empty() || !AudioPreview::available()) {
					stopPreview();
					status_->setText(samples->empty() ? QStringLiteral("Could not decode this sound.")
									  : QStringLiteral("No audio output device."));
					return;
				}
				emit previewStarted();
				preview_->setBuffer(std::move(*samples));
				preview_->start(0);
				status_->setText(preview_->durationMs() >= kPreviewMs
							 ? QStringLiteral("Playing the first 15 seconds…")
							 : QStringLiteral("Playing preview…"));
				previewTimer_->start();
			});
			worker->start();
		}, error);
	if (!previewReply_) {
		stopPreview();
		status_->setText(error);
	}
}

QString AudioLibRemoteServerBrowser::importSound(QWidget *parent, const AudioLibRemoteServerSound &sound)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(QStringLiteral("Import XA1 audio"));
	dialog.setWindowModality(Qt::WindowModal);
	auto *layout = new QVBoxLayout(&dialog);
	auto *label = new QLabel(&dialog);
	label->setTextFormat(Qt::PlainText);
	label->setWordWrap(true);
	label->setText(QStringLiteral("Downloading %1…").arg(sound.name));
	layout->addWidget(label);
	auto *progress = new QProgressBar(&dialog);
	progress->setRange(0, 0);
	layout->addWidget(progress);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);
	dialog.setMinimumWidth(320);

	QString file;
	AudioLibRemoteServer::Failure failure;
	const QPointer<QNetworkReply> reply =
		AudioLibRemoteServer::shared().download(sound, AudioLibRemoteServer::importsDir(), &dialog,
			[&](const QString &path, const AudioLibRemoteServer::Failure &result) {
				file = path;
				failure = result;
				dialog.accept();
			}, failure.message);
	if (reply) {
		connect(reply, &QNetworkReply::downloadProgress, progress, [progress](qint64 got, qint64 total) {
			if (total > 0) {
				progress->setRange(0, 100);
				progress->setValue(int(100.0 * double(got) / double(total)));
			}
		});
		if (dialog.exec() != QDialog::Accepted) {
			if (reply) {
				reply->disconnect(&dialog);
				reply->abort();
			}
			return {};
		}
	}
	if (!failure.message.isEmpty()) {
		QMessageBox box(QMessageBox::Warning, dialog.windowTitle(), failure.message, QMessageBox::Close, parent);
		box.setTextFormat(Qt::PlainText);
		QPushButton *copy = nullptr;
		if (!failure.report.isEmpty()) {
			editorLog(EditorLogLevel::Error, QStringLiteral("XA1"), failure.report);
			box.setDetailedText(failure.report);
			copy = box.addButton(QStringLiteral("Copy error details"), QMessageBox::ActionRole);
			copy->setAutoDefault(false);
		}
		box.exec();
		if (copy && box.clickedButton() == copy)
			QGuiApplication::clipboard()->setText(failure.report);
	}
	return file;
}

QString AudioLibRemoteServerBrowser::chooseSound(QWidget *parent, const QString &action)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(QStringLiteral("XA1 Audio"));
	dialog.setWindowModality(Qt::WindowModal);
	dialog.resize(650, 460);
	auto *layout = new QVBoxLayout(&dialog);
	auto *browser = new AudioLibRemoteServerBrowser(action, false, &dialog);
	layout->addWidget(browser);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);
	QString path;
	connect(browser, &AudioLibRemoteServerBrowser::soundChosen, &dialog, [&](const AudioLibRemoteServerSound &sound) {
		path = importSound(&dialog, sound);
		if (!path.isEmpty())
			dialog.accept();
	});
	browser->focusSearch();
	return dialog.exec() == QDialog::Accepted ? path : QString();
}

} // namespace harpia
