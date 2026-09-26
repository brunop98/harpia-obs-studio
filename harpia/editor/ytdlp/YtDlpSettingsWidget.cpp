#include "YtDlpSettingsWidget.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace harpia {

YtDlpSettingsWidget::YtDlpSettingsWidget(QWidget *parent) : QWidget(parent)
{
	auto *lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(8);

	auto *intro = new QLabel(QStringLiteral(
		"yt-dlp is your own copy, found on the PATH. The editor's Add menu offers "
		"\"Video from URL…\" only when it is found. Nothing here is required for anything else."), this);
	intro->setWordWrap(true);
	intro->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	lay->addWidget(intro);

	auto *form = new QFormLayout;
	form->setHorizontalSpacing(10);
	form->setVerticalSpacing(6);

	auto *exeRow = new QHBoxLayout;
	exePath_ = new QLineEdit(this);
	exePath_->setPlaceholderText(QStringLiteral("(found on the PATH)"));
	exePath_->setToolTip(QStringLiteral("Leave empty to use the yt-dlp on your PATH, or point at yt-dlp.exe."));
	exeRow->addWidget(exePath_, 1);
	auto *browseExe = new QPushButton(QStringLiteral("Browse…"), this);
	connect(browseExe, &QPushButton::clicked, this, [this]() {
		const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("yt-dlp executable"));
		if (!f.isEmpty())
			exePath_->setText(f);
	});
	exeRow->addWidget(browseExe);
	auto *test = new QPushButton(QStringLiteral("Test"), this);
	connect(test, &QPushButton::clicked, this, &YtDlpSettingsWidget::runTest);
	exeRow->addWidget(test);
	form->addRow(QStringLiteral("yt-dlp"), exeRow);
	state_ = new QLabel(this);
	state_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	state_->setWordWrap(true);
	form->addRow(QString(), state_);

	cookiesBrowser_ = new QComboBox(this);
	cookiesBrowser_->addItem(QStringLiteral("None"), QString());
	for (const QString &b : YtDlpSettings::cookieBrowsers())
		cookiesBrowser_->addItem(b, b);
	cookiesBrowser_->setToolTip(QStringLiteral(
		"Use the cookies of a browser you are signed in with (--cookies-from-browser): needed "
		"for age-restricted, members-only or private videos. Close the browser first on Windows."));
	form->addRow(QStringLiteral("Cookies from browser"), cookiesBrowser_);

	auto *ckRow = new QHBoxLayout;
	cookiesFile_ = new QLineEdit(this);
	cookiesFile_->setPlaceholderText(QStringLiteral("cookies.txt (used when no browser is chosen)"));
	ckRow->addWidget(cookiesFile_, 1);
	auto *browseCk = new QPushButton(QStringLiteral("Browse…"), this);
	connect(browseCk, &QPushButton::clicked, this, [this]() {
		const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Cookies file"), QString(),
							       QStringLiteral("Cookies (*.txt);;All files (*)"));
		if (!f.isEmpty())
			cookiesFile_->setText(f);
	});
	ckRow->addWidget(browseCk);
	form->addRow(QStringLiteral("Cookies file"), ckRow);

	auto *dirRow = new QHBoxLayout;
	downloadDir_ = new QLineEdit(this);
	downloadDir_->setPlaceholderText(YtDlpSettings::defaultDownloadDir());
	dirRow->addWidget(downloadDir_, 1);
	auto *browseDir = new QPushButton(QStringLiteral("Browse…"), this);
	connect(browseDir, &QPushButton::clicked, this, [this]() {
		const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Download folder"),
								    downloadDir_->text());
		if (!d.isEmpty())
			downloadDir_->setText(d);
	});
	dirRow->addWidget(browseDir);
	form->addRow(QStringLiteral("Download folder"), dirRow);

	preferMp4_ = new QCheckBox(QStringLiteral("Merge into MP4 (recommended for the editor)"), this);
	form->addRow(QString(), preferMp4_);

	extraArgs_ = new QLineEdit(this);
	extraArgs_->setPlaceholderText(QStringLiteral("e.g.  --proxy http://127.0.0.1:8080  -4"));
	extraArgs_->setToolTip(QStringLiteral("Added to every yt-dlp call, for anything not covered above."));
	form->addRow(QStringLiteral("Extra arguments"), extraArgs_);
	lay->addLayout(form);
	lay->addStretch(1);

	reload();
	for (QLineEdit *e : {exePath_, cookiesFile_, downloadDir_, extraArgs_})
		connect(e, &QLineEdit::editingFinished, this, &YtDlpSettingsWidget::save);
	connect(cookiesBrowser_, &QComboBox::currentIndexChanged, this, [this](int) { save(); });
	connect(preferMp4_, &QCheckBox::toggled, this, [this](bool) { save(); });
}

void YtDlpSettingsWidget::reload()
{
	loading_ = true;
	const YtDlpSettings s = YtDlpSettings::load();
	exePath_->setText(s.exePath);
	const int bi = cookiesBrowser_->findData(s.cookiesBrowser);
	cookiesBrowser_->setCurrentIndex(bi < 0 ? 0 : bi);
	cookiesFile_->setText(s.cookiesFile);
	downloadDir_->setText(s.downloadDir);
	preferMp4_->setChecked(s.preferMp4);
	extraArgs_->setText(s.extraArgs);
	loading_ = false;
	if (YtDlp::available())
		state_->setText(QStringLiteral("Found: %1  (version %2)").arg(YtDlp::foundPath(), YtDlp::version()));
	else
		state_->setText(QStringLiteral("Not found. Install yt-dlp (or point at it above) and press Test."));
}

void YtDlpSettingsWidget::save()
{
	if (loading_)
		return;
	YtDlpSettings s;
	s.exePath = exePath_->text().trimmed();
	s.cookiesBrowser = cookiesBrowser_->currentData().toString();
	s.cookiesFile = cookiesFile_->text().trimmed();
	s.downloadDir = downloadDir_->text().trimmed();
	s.preferMp4 = preferMp4_->isChecked();
	s.extraArgs = extraArgs_->text().trimmed();
	s.save();
	emit changed();
}

void YtDlpSettingsWidget::runTest()
{
	save();
	state_->setText(QStringLiteral("Testing…"));
	const bool ok = YtDlp::probe(/*force=*/true);
	if (ok)
		state_->setText(QStringLiteral("Found: %1  (version %2)").arg(YtDlp::foundPath(), YtDlp::version()));
	else if (YtDlp::foundPath().isEmpty())
		state_->setText(QStringLiteral("Not found on the PATH or at the path above."));
	else
		state_->setText(QStringLiteral("Found %1 but it did not answer --version.").arg(YtDlp::foundPath()));
	emit changed();
}

YtDlpSettingsDialog::YtDlpSettingsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("yt-dlp settings"));
	auto *lay = new QVBoxLayout(this);
	lay->setContentsMargins(14, 12, 14, 12);
	lay->addWidget(new YtDlpSettingsWidget(this));
	auto *close = new QPushButton(QStringLiteral("Close"), this);
	connect(close, &QPushButton::clicked, this, &QDialog::accept);
	auto *row = new QHBoxLayout;
	row->addStretch(1);
	row->addWidget(close);
	lay->addLayout(row);
	setMinimumWidth(520);
}

} // namespace harpia
