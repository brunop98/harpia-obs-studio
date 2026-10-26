#include "PhoneSharingDialog.hpp"

#include "core/LocalSend.hpp"
#include "core/RecorderSettings.hpp"
#include "platform/PhoneSharingNative.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace harpia {
namespace {
PhoneSharingServices defaultServices()
{
	PhoneSharingServices services;
	services.findLocalSend = []() {
		auto settings = recorderSettings();
		const QString configured = settings.value(QStringLiteral("sharing/localSendPath")).toString();
		const QString application = local_send::findApplication(configured);
		return application.isEmpty() && !configured.isEmpty() ? local_send::findApplication() : application;
	};
	services.chooseLocalSend = [](QWidget *parent) {
#if defined(Q_OS_MACOS)
		const QString filter = QStringLiteral("Applications (*.app);;All files (*)");
#elif defined(Q_OS_WIN)
		const QString filter = QStringLiteral("Applications (*.exe);;All files (*)");
#else
		const QString filter = QStringLiteral("All files (*)");
#endif
		const QString selected = QFileDialog::getOpenFileName(parent, QStringLiteral("Locate LocalSend"), {}, filter);
		return selected.isEmpty() ? QString() : local_send::findApplication(selected);
	};
	services.rememberLocalSend = [](const QString &path) {
		auto settings = recorderSettings();
		settings.setValue(QStringLiteral("sharing/localSendPath"), path);
	};
	services.startLocalSend = local_send::sendFiles;
	services.airDropAvailable = phone_sharing::native::airDropSupported;
	services.startAirDrop = phone_sharing::native::sendWithAirDrop;
	services.openUrl = [](const QUrl &url) { return QDesktopServices::openUrl(url); };
	return services;
}
} // namespace

PhoneSharingDialog::PhoneSharingDialog(const QStringList &files, QWidget *parent)
	: PhoneSharingDialog(files, defaultServices(), parent)
{
}

PhoneSharingDialog::PhoneSharingDialog(const QStringList &files, PhoneSharingServices services, QWidget *parent)
	: QDialog(parent), services_(std::move(services))
{
	for (const QString &file : files) {
		if (!file.isEmpty())
			files_.append(QFileInfo(file).absoluteFilePath());
	}
	files_.removeDuplicates();
	setWindowTitle(QStringLiteral("Send to phone"));
	setObjectName(QStringLiteral("phoneSharingDialog"));
	setMinimumWidth(460);
	auto *layout = new QVBoxLayout(this);
	auto *summary = new QLabel(files_.size() == 1 ? QFileInfo(files_.first()).fileName()
		: QStringLiteral("%1 files selected").arg(files_.size()), this);
	summary->setObjectName(QStringLiteral("phoneSharingFiles"));
	summary->setTextFormat(Qt::PlainText);
	summary->setWordWrap(true);
	summary->setToolTip(files_.join(QLatin1Char('\n')));
	layout->addWidget(summary);

	if (services_.airDropAvailable && services_.airDropAvailable()) {
		auto *group = new QGroupBox(QStringLiteral("AirDrop"), this);
		auto *airDropLayout = new QVBoxLayout(group);
		auto *help = new QLabel(QStringLiteral("Send directly from your Mac to your iPhone. "
			"Turn on Wi-Fi and Bluetooth, and allow AirDrop receiving on the iPhone."), group);
		help->setWordWrap(true);
		airDropLayout->addWidget(help);
		airDropButton_ = new QPushButton(QStringLiteral("Send with AirDrop…"), group);
		airDropButton_->setObjectName(QStringLiteral("sendWithAirDrop"));
		airDropLayout->addWidget(airDropButton_);
		connect(airDropButton_, &QPushButton::clicked, this, &PhoneSharingDialog::sendAirDrop);
		layout->addWidget(group);
	}

	auto *local = new QGroupBox(QStringLiteral("LocalSend (optional)"), this);
	auto *localLayout = new QVBoxLayout(local);
	auto *help = new QLabel(QStringLiteral("Open LocalSend on your iPhone and connect both devices to the same Wi-Fi. "
		"The selected files will be ready in LocalSend; choose your iPhone to start the transfer."), local);
	help->setWordWrap(true);
	localLayout->addWidget(help);
	localSendInfo_ = new QLabel(local);
	localSendInfo_->setObjectName(QStringLiteral("localSendRecommendation"));
	localSendInfo_->setTextFormat(Qt::PlainText);
	localSendInfo_->setWordWrap(true);
	localLayout->addWidget(localSendInfo_);
	localSendButton_ = new QPushButton(QStringLiteral("Open in LocalSend"), local);
	localSendButton_->setObjectName(QStringLiteral("sendWithLocalSend"));
	connect(localSendButton_, &QPushButton::clicked, this, &PhoneSharingDialog::sendLocalSend);
	localLayout->addWidget(localSendButton_);
	auto *tools = new QHBoxLayout;
	auto *download = new QPushButton(QStringLiteral("Get LocalSend…"), local);
	download->setObjectName(QStringLiteral("downloadLocalSend"));
	connect(download, &QPushButton::clicked, this, [this]() {
		if (!services_.openUrl || !services_.openUrl(QUrl(QStringLiteral("https://localsend.org/download"))))
			setStatus(QStringLiteral("Open https://localsend.org/download to install LocalSend."), true);
	});
	tools->addWidget(download);
	auto *locate = new QPushButton(QStringLiteral("Locate LocalSend…"), local);
	locate->setObjectName(QStringLiteral("locateLocalSend"));
	connect(locate, &QPushButton::clicked, this, [this]() {
		const QString application = services_.chooseLocalSend ? services_.chooseLocalSend(this) : QString();
		if (application.isEmpty())
			return;
		localSend_ = application;
		if (services_.rememberLocalSend)
			services_.rememberLocalSend(application);
		updateAvailability();
		setStatus(QStringLiteral("LocalSend selected. Click Open in LocalSend to continue."));
	});
	tools->addWidget(locate);
	auto *refresh = new QPushButton(QStringLiteral("Refresh"), local);
	refresh->setObjectName(QStringLiteral("refreshLocalSend"));
	connect(refresh, &QPushButton::clicked, this, &PhoneSharingDialog::refreshLocalSend);
	tools->addWidget(refresh);
	localLayout->addLayout(tools);
	layout->addWidget(local);
	status_ = new QLabel(this);
	status_->setObjectName(QStringLiteral("phoneSharingStatus"));
	status_->setTextFormat(Qt::PlainText);
	status_->setWordWrap(true);
	status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(status_);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);
	refreshLocalSend();
}

void PhoneSharingDialog::runModal(const QStringList &files, QWidget *parent)
{
	PhoneSharingDialog dialog(files, parent);
	dialog.exec();
}

bool PhoneSharingDialog::filesReady(QString *error) const
{
	if (files_.isEmpty()) {
		if (error)
			*error = QStringLiteral("No files were selected.");
		return false;
	}
	for (const QString &file : files_) {
		const QFileInfo info(file);
		if (!info.isFile() || !info.isReadable()) {
			if (error)
				*error = QStringLiteral("This file is missing or cannot be read:\n%1").arg(QDir::toNativeSeparators(file));
			return false;
		}
	}
	return true;
}

void PhoneSharingDialog::refreshLocalSend()
{
	localSend_ = services_.findLocalSend ? services_.findLocalSend() : QString();
	updateAvailability();
}

void PhoneSharingDialog::updateAvailability()
{
	localSendInfo_->setText(localSend_.isEmpty()
		? QStringLiteral("LocalSend was not found. Recommended: install the free LocalSend app on this "
			"computer and your iPhone, then click Refresh. It preserves the original file quality.")
		: QStringLiteral("LocalSend found: %1").arg(QDir::toNativeSeparators(localSend_)));
	QString error;
	const bool ready = filesReady(&error);
	localSendButton_->setEnabled(ready && !localSend_.isEmpty());
	if (airDropButton_)
		airDropButton_->setEnabled(ready && !airDropInFlight_);
	if (!ready)
		setStatus(error, true);
}

void PhoneSharingDialog::setStatus(const QString &message, bool error)
{
	status_->setText(message);
	status_->setStyleSheet(error ? QStringLiteral("color:#e5484d;") : QString());
}

void PhoneSharingDialog::sendLocalSend()
{
	QString error;
	if (!filesReady(&error)) {
		setStatus(error, true);
		updateAvailability();
		return;
	}
	if (!services_.startLocalSend || !services_.startLocalSend(localSend_, files_, &error)) {
		setStatus(error.isEmpty() ? QStringLiteral("Could not open LocalSend. Try Refresh or Locate LocalSend.") : error, true);
		return;
	}
	setStatus(QStringLiteral("Opening LocalSend with your files. Choose your iPhone in LocalSend to start sending."));
}

void PhoneSharingDialog::sendAirDrop()
{
	if (airDropInFlight_)
		return;
	QString error;
	if (!filesReady(&error)) {
		setStatus(error, true);
		updateAvailability();
		return;
	}
	QPointer<PhoneSharingDialog> guard(this);
	// AppKit may finish sharing after this dialog closes. The service belongs
	// to the application; its callback updates only a surviving dialog.
	const auto completion = [guard](const QString &message) {
		if (guard) {
			guard->airDropInFlight_ = false;
			guard->updateAvailability();
			guard->setStatus(message.isEmpty() ? QStringLiteral("AirDrop transfer completed.") : message, !message.isEmpty());
		}
	};
	airDropInFlight_ = true;
	updateAvailability();
	if (!services_.startAirDrop || !services_.startAirDrop(files_, qApp, completion, &error)) {
		airDropInFlight_ = false;
		updateAvailability();
		setStatus(error.isEmpty() ? QStringLiteral("AirDrop is unavailable. You can use LocalSend instead.") : error, true);
		return;
	}
	if (airDropInFlight_)
		setStatus(QStringLiteral("Choose your iPhone in the AirDrop window and accept the transfer on your phone."));
}

} // namespace harpia
