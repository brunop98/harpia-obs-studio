#include "UrlDownloadDialog.hpp"

#include "YtDlpSettingsWidget.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace harpia {

namespace {
QString bytesText(qint64 b)
{
	if (b < 0)
		return QString();
	if (b >= 1024LL * 1024 * 1024)
		return QStringLiteral("%1 GB").arg(double(b) / (1024.0 * 1024 * 1024), 0, 'f', 2);
	if (b >= 1024 * 1024)
		return QStringLiteral("%1 MB").arg(double(b) / (1024.0 * 1024), 0, 'f', 0);
	return QStringLiteral("%1 KB").arg(double(b) / 1024.0, 0, 'f', 0);
}
QString dateText(const QString &yyyymmdd)
{
	if (yyyymmdd.size() != 8)
		return yyyymmdd;
	return QStringLiteral("%1-%2-%3").arg(yyyymmdd.left(4), yyyymmdd.mid(4, 2), yyyymmdd.mid(6, 2));
}
} // namespace

UrlDownloadDialog::UrlDownloadDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Add video from URL"));
	setModal(false);
	settings_ = YtDlpSettings::load();
	buildUi();
	setStage(Stage::Idle);
	// A link on the clipboard is almost certainly the one you mean.
	const QString clip = QApplication::clipboard()->text().trimmed();
	if (YtDlp::looksLikeUrl(clip))
		setUrl(clip);
}

UrlDownloadDialog::~UrlDownloadDialog()
{
	if (proc_) {
		proc_->kill();
		proc_->waitForFinished(1000);
	}
	if (!printPathFile_.isEmpty())
		QFile::remove(printPathFile_);
}

void UrlDownloadDialog::buildUi()
{
	auto *lay = new QVBoxLayout(this);
	lay->setContentsMargins(14, 12, 14, 12);
	lay->setSpacing(10);

	auto *urlRow = new QHBoxLayout;
	url_ = new QLineEdit(this);
	url_->setPlaceholderText(QStringLiteral("Paste a video link (YouTube, Vimeo, Twitch, and anything else yt-dlp knows)"));
	url_->setClearButtonEnabled(true);
	connect(url_, &QLineEdit::returnPressed, this, &UrlDownloadDialog::fetchInfo);
	urlRow->addWidget(url_, 1);
	fetch_ = new QPushButton(QStringLiteral("Check"), this);
	fetch_->setToolTip(QStringLiteral("Ask yt-dlp what this is before downloading anything."));
	connect(fetch_, &QPushButton::clicked, this, &UrlDownloadDialog::fetchInfo);
	urlRow->addWidget(fetch_);
	lay->addLayout(urlRow);

	// ---- the video's facts ----
	infoBox_ = new QWidget(this);
	auto *ib = new QHBoxLayout(infoBox_);
	ib->setContentsMargins(0, 0, 0, 0);
	ib->setSpacing(12);
	thumb_ = new QLabel(infoBox_);
	thumb_->setFixedSize(240, 135);
	thumb_->setAlignment(Qt::AlignCenter);
	thumb_->setStyleSheet(QStringLiteral("background:#15171a;border:1px solid #2b2f36;color:#5f6570;"));
	thumb_->setText(QStringLiteral("no thumbnail"));
	ib->addWidget(thumb_, 0, Qt::AlignTop);
	auto *facts = new QVBoxLayout;
	facts->setSpacing(4);
	title_ = new QLabel(infoBox_);
	title_->setWordWrap(true);
	title_->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed; font-size:14px;"));
	facts->addWidget(title_);
	meta_ = new QLabel(infoBox_);
	meta_->setWordWrap(true);
	meta_->setStyleSheet(QStringLiteral("color:#c8ccd4;"));
	facts->addWidget(meta_);
	subsInfo_ = new QLabel(infoBox_);
	subsInfo_->setWordWrap(true);
	subsInfo_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	facts->addWidget(subsInfo_);
	facts->addStretch(1);
	ib->addLayout(facts, 1);
	lay->addWidget(infoBox_);

	// ---- options ----
	optionsBox_ = new QWidget(this);
	auto *of = new QFormLayout(optionsBox_);
	of->setContentsMargins(0, 0, 0, 0);
	of->setHorizontalSpacing(10);
	of->setVerticalSpacing(6);
	quality_ = new QComboBox(optionsBox_);
	quality_->setToolTip(QStringLiteral("The qualities this video is actually offered in."));
	of->addRow(QStringLiteral("Quality"), quality_);
	audio_ = new QCheckBox(QStringLiteral("Include audio"), optionsBox_);
	audio_->setChecked(true);
	audio_->setToolTip(QStringLiteral("Off downloads the picture only: a silent clip, smaller and faster."));
	of->addRow(QString(), audio_);
	subtitles_ = new QComboBox(optionsBox_);
	subtitles_->setToolTip(QStringLiteral(
		"Also download this subtitle track as an .srt beside the video. (Placing it on the "
		"timeline as captions is a later version; the file is yours to keep.)"));
	of->addRow(QStringLiteral("Subtitles"), subtitles_);
	auto *dirRow = new QHBoxLayout;
	folder_ = new QLabel(optionsBox_);
	folder_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	folder_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	dirRow->addWidget(folder_, 1);
	auto *changeDir = new QPushButton(QStringLiteral("Change…"), optionsBox_);
	connect(changeDir, &QPushButton::clicked, this, [this]() {
		const QString d = QFileDialog::getExistingDirectory(
			this, QStringLiteral("Download folder"),
			settings_.downloadDir.isEmpty() ? YtDlpSettings::defaultDownloadDir() : settings_.downloadDir);
		if (d.isEmpty())
			return;
		settings_.downloadDir = d;
		settings_.save();
		refreshFolder();
	});
	dirRow->addWidget(changeDir);
	auto *settingsBtn = new QPushButton(QStringLiteral("Settings…"), optionsBox_);
	settingsBtn->setToolTip(QStringLiteral("Cookies, the yt-dlp path, extra arguments."));
	connect(settingsBtn, &QPushButton::clicked, this, &UrlDownloadDialog::openSettings);
	dirRow->addWidget(settingsBtn);
	of->addRow(QStringLiteral("Save to"), dirRow);
	lay->addWidget(optionsBox_);

	// ---- progress ----
	progress_ = new QProgressBar(this);
	progress_->setRange(0, 1000);
	progress_->setValue(0);
	progress_->setTextVisible(true);
	progress_->setFormat(QStringLiteral("%p%"));
	lay->addWidget(progress_);
	status_ = new QLabel(this);
	status_->setWordWrap(true);
	status_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	lay->addWidget(status_);

	auto *btns = new QHBoxLayout;
	btns->addStretch(1);
	cancel_ = new QPushButton(QStringLiteral("Cancel"), this);
	connect(cancel_, &QPushButton::clicked, this, [this]() {
		if (proc_ && proc_->state() != QProcess::NotRunning) {
			proc_->kill(); // finished() follows and reports the cancel
			return;
		}
		reject();
	});
	btns->addWidget(cancel_);
	download_ = new QPushButton(QStringLiteral("Download and add"), this);
	download_->setDefault(true);
	download_->setMinimumHeight(32);
	connect(download_, &QPushButton::clicked, this, &UrlDownloadDialog::startDownload);
	btns->addWidget(download_);
	lay->addLayout(btns);
	setMinimumWidth(620);
	refreshFolder();
}

void UrlDownloadDialog::refreshFolder()
{
	folder_->setText(QDir::toNativeSeparators(settings_.downloadDir.isEmpty() ? YtDlpSettings::defaultDownloadDir()
										: settings_.downloadDir));
}

void UrlDownloadDialog::setStage(Stage s)
{
	stage_ = s;
	const bool busy = s == Stage::Fetching || s == Stage::Downloading;
	url_->setEnabled(!busy);
	fetch_->setEnabled(!busy);
	infoBox_->setVisible(s == Stage::Ready || s == Stage::Downloading || s == Stage::Done);
	optionsBox_->setVisible(s == Stage::Ready || s == Stage::Downloading || s == Stage::Done);
	progress_->setVisible(s == Stage::Downloading || s == Stage::Done);
	download_->setVisible(s == Stage::Ready);
	optionsBox_->setEnabled(s == Stage::Ready);
	cancel_->setText(s == Stage::Downloading ? QStringLiteral("Stop") : s == Stage::Done ? QStringLiteral("Close")
											       : QStringLiteral("Cancel"));
	if (s == Stage::Idle)
		status_->setText(QStringLiteral("Paste a link and press Check."));
	adjustSize();
}

void UrlDownloadDialog::setUrl(const QString &url)
{
	url_->setText(url.trimmed());
	if (YtDlp::looksLikeUrl(url))
		fetchInfo();
}

void UrlDownloadDialog::fetchInfo()
{
	const QString url = url_->text().trimmed();
	if (!YtDlp::looksLikeUrl(url)) {
		status_->setText(QStringLiteral("That does not look like a link (it should start with http:// or https://)."));
		return;
	}
	const QString exe = YtDlp::locate(settings_);
	if (exe.isEmpty()) {
		status_->setText(QStringLiteral("yt-dlp was not found. Point at it in Settings…"));
		return;
	}
	if (proc_) {
		proc_->kill();
		proc_->deleteLater();
		proc_ = nullptr;
	}
	setStage(Stage::Fetching);
	status_->setText(QStringLiteral("Asking yt-dlp about this link…"));
	fetchOut_.clear();
	proc_ = new QProcess(this);
	proc_->setProgram(exe);
	proc_->setArguments(YtDlp::infoArgs(url, settings_));
	connect(proc_, &QProcess::readyReadStandardOutput, this, [this]() { fetchOut_ += proc_->readAllStandardOutput(); });
	connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
		const QString errText = QString::fromUtf8(proc_->readAllStandardError()).trimmed();
		proc_->deleteLater();
		proc_ = nullptr;
		QString err;
		const YtVideoInfo v = (st == QProcess::NormalExit && code == 0) ? YtDlp::parseInfo(fetchOut_, &err) : YtVideoInfo();
		if (!v.valid()) {
			// yt-dlp's own last ERROR line is the useful one.
			QString why = err;
			for (const QString &l : errText.split(QLatin1Char('\n')))
				if (l.startsWith(QLatin1String("ERROR")))
					why = l.mid(6).trimmed();
			if (why.isEmpty())
				why = errText.isEmpty() ? QStringLiteral("yt-dlp gave no information") : errText;
			setStage(Stage::Idle);
			status_->setText(QStringLiteral("Could not read that link: %1").arg(why));
			return;
		}
		showInfo(v);
	});
	connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
		if (stage_ == Stage::Fetching) {
			setStage(Stage::Idle);
			status_->setText(QStringLiteral("yt-dlp could not be started (%1).").arg(proc_ ? proc_->errorString() : QString()));
		}
	});
	proc_->start();
}

void UrlDownloadDialog::showInfo(const YtVideoInfo &v)
{
	info_ = v;
	title_->setText(v.title);
	QStringList m;
	if (!v.channel.isEmpty())
		m << v.channel;
	if (v.durationS > 0)
		m << v.durationText();
	if (v.viewCount >= 0)
		m << QStringLiteral("%1 views").arg(QLocale().toString(qlonglong(v.viewCount)));
	if (!v.uploadDate.isEmpty())
		m << dateText(v.uploadDate);
	meta_->setText(m.join(QStringLiteral("  ·  ")));
	QStringList subs;
	if (!v.subtitleLangs.isEmpty())
		subs << QStringLiteral("Subtitles: %1").arg(v.subtitleLangs.join(QStringLiteral(", ")));
	if (!v.autoCaptionLangs.isEmpty())
		subs << QStringLiteral("Auto captions: %1").arg(v.autoCaptionLangs.size() > 8
								  ? QStringLiteral("%1 languages").arg(v.autoCaptionLangs.size())
								  : v.autoCaptionLangs.join(QStringLiteral(", ")));
	subsInfo_->setText(subs.isEmpty() ? QStringLiteral("No subtitles offered.") : subs.join(QStringLiteral("   ")));

	quality_->clear();
	for (const YtQuality &q : v.qualities) {
		const QString size = bytesText(q.sizeBytes);
		quality_->addItem(size.isEmpty() ? q.label : QStringLiteral("%1   (%2 video)").arg(q.label, size), q.height);
	}
	// Default to the best height at or under 1080: 4K into a 1080p project
	// is a slower download for nothing you would see.
	for (int i = 0; i < quality_->count(); ++i)
		if (quality_->itemData(i).toInt() > 0 && quality_->itemData(i).toInt() <= 1080) {
			quality_->setCurrentIndex(i);
			break;
		}
	subtitles_->clear();
	subtitles_->addItem(QStringLiteral("None"), QString());
	for (const QString &l : v.subtitleLangs)
		subtitles_->addItem(QStringLiteral("%1 (uploaded)").arg(l), l);
	for (const QString &l : v.autoCaptionLangs)
		if (!v.subtitleLangs.contains(l))
			subtitles_->addItem(QStringLiteral("%1 (auto)").arg(l), l);

	thumb_->setPixmap(QPixmap());
	thumb_->setText(QStringLiteral("loading…"));
	if (!v.thumbnailUrl.isEmpty())
		loadThumbnail(v.thumbnailUrl);
	setStage(Stage::Ready);
	status_->setText(QStringLiteral("Is this the one? Pick a quality and press Download and add."));
}

void UrlDownloadDialog::loadThumbnail(const QString &url)
{
	QNetworkReply *r = nam_.get(QNetworkRequest(QUrl(url)));
	connect(r, &QNetworkReply::finished, this, [this, r]() {
		r->deleteLater();
		QPixmap pm;
		if (r->error() == QNetworkReply::NoError && pm.loadFromData(r->readAll())) {
			thumb_->setPixmap(pm.scaled(thumb_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
			thumb_->setText(QString());
		} else {
			thumb_->setText(QStringLiteral("no thumbnail"));
		}
	});
}

void UrlDownloadDialog::startDownload()
{
	if (stage_ != Stage::Ready)
		return;
	const QString exe = YtDlp::locate(settings_);
	if (exe.isEmpty()) {
		status_->setText(QStringLiteral("yt-dlp was not found. Point at it in Settings…"));
		return;
	}
	YtDownloadOptions o;
	o.maxHeight = quality_->currentData().toInt();
	o.includeAudio = audio_->isChecked();
	o.subtitleLang = subtitles_->currentData().toString();
	o.outDir = settings_.downloadDir.isEmpty() ? YtDlpSettings::defaultDownloadDir() : settings_.downloadDir;
	QDir().mkpath(o.outDir);
	printPathFile_ = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
				 .filePath(QStringLiteral("harpia-ytdlp-%1.path").arg(QCoreApplication::applicationPid()));
	QFile::remove(printPathFile_);
	o.printPathTo = printPathFile_;

	lastError_.clear();
	progress_->setValue(0);
	setStage(Stage::Downloading);
	status_->setText(QStringLiteral("Starting…"));
	proc_ = new QProcess(this);
	proc_->setProgram(exe);
	proc_->setArguments(YtDlp::downloadArgs(url_->text().trimmed(), settings_, o));
	proc_->setProcessChannelMode(QProcess::MergedChannels);
	connect(proc_, &QProcess::readyRead, this, [this]() {
		while (proc_ && proc_->canReadLine())
			onDownloadLine(QString::fromUtf8(proc_->readLine()).trimmed());
	});
	connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
		while (proc_->canReadLine())
			onDownloadLine(QString::fromUtf8(proc_->readLine()).trimmed());
		const QString rest = QString::fromUtf8(proc_->readAll()).trimmed();
		if (!rest.isEmpty())
			onDownloadLine(rest);
		proc_->deleteLater();
		proc_ = nullptr;
		finishDownload(st == QProcess::NormalExit ? code : -1);
	});
	proc_->start();
}

void UrlDownloadDialog::onDownloadLine(const QString &line)
{
	if (line.isEmpty())
		return;
	double pc = 0;
	QString detail;
	if (YtDlp::parseProgress(line, &pc, &detail)) {
		progress_->setValue(int(pc * 10));
		status_->setText(detail.isEmpty() ? QStringLiteral("Downloading…") : QStringLiteral("Downloading  %1").arg(detail));
		return;
	}
	if (line.startsWith(QLatin1String("ERROR")))
		lastError_ = line.mid(6).trimmed();
	else if (line.startsWith(QLatin1String("[Merger]")))
		status_->setText(QStringLiteral("Merging video and audio…"));
	else if (line.startsWith(QLatin1String("[info]")) && line.contains(QLatin1String("subtitle")))
		status_->setText(QStringLiteral("Fetching subtitles…"));
}

void UrlDownloadDialog::finishDownload(int exitCode)
{
	QString path;
	{
		QFile f(printPathFile_);
		if (f.open(QIODevice::ReadOnly | QIODevice::Text))
			path = QString::fromUtf8(f.readAll()).trimmed().section(QLatin1Char('\n'), -1);
		QFile::remove(printPathFile_);
		printPathFile_.clear();
	}
	if (exitCode != 0 || path.isEmpty() || !QFileInfo::exists(path)) {
		setStage(Stage::Ready);
		if (exitCode == -1 || exitCode == 9 || exitCode == 137)
			status_->setText(QStringLiteral("Stopped."));
		else
			status_->setText(QStringLiteral("Download failed: %1").arg(
				lastError_.isEmpty() ? QStringLiteral("yt-dlp exited with code %1").arg(exitCode) : lastError_));
		return;
	}
	progress_->setValue(1000);
	setStage(Stage::Done);
	status_->setText(QStringLiteral("Done: %1\nAdded to the timeline at the playhead.").arg(QDir::toNativeSeparators(path)));
	emit downloaded(path, info_);
}

void UrlDownloadDialog::openSettings()
{
	YtDlpSettingsDialog d(this);
	d.exec();
	settings_ = YtDlpSettings::load();
	refreshFolder();
}

} // namespace harpia
