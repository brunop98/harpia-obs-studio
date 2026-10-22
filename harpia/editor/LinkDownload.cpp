#include "LinkDownload.hpp"

#include "ClipboardPaste.hpp"
#include "MediaFiles.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

namespace harpia {

LinkDownload::LinkDownload(QObject *parent) : QObject(parent) {}

LinkDownload::~LinkDownload()
{
	cancel();
}

void LinkDownload::start(const QUrl &url, const QString &folder)
{
	cancel();
	url_ = url;
	folder_ = folder;
	decided_ = dropped_ = false;
	QNetworkRequest req(url);
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	// Some hosts refuse a request with no browser-like agent at all.
	req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (Harpia)"));
	reply_ = nam_.get(req);
	connect(reply_, &QNetworkReply::metaDataChanged, this, &LinkDownload::decide);
	connect(reply_, &QNetworkReply::readyRead, this, [this]() {
		decide();
		if (reply_ && file_.isOpen())
			file_.write(reply_->readAll());
	});
	connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
		if (decided_ && !dropped_)
			emit progress(got, total > 0 ? total : -1);
	});
	connect(reply_, &QNetworkReply::finished, this, &LinkDownload::finish);
}

void LinkDownload::cancel()
{
	if (!reply_)
		return;
	dropped_ = true;
	QNetworkReply *r = reply_;
	reply_ = nullptr;
	r->disconnect(this);
	r->abort();
	r->deleteLater();
	if (file_.isOpen()) {
		file_.close();
		file_.remove(); // half a file is worse than none
	}
}

void LinkDownload::decide()
{
	if (decided_ || !reply_)
		return;
	const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	// A redirect's headers describe the redirect, and an error page's describe
	// the error page: wait for the real answer, or for finish() to report it.
	if ((status >= 300 && status < 400) || status >= 400)
		return;
	const QString type = reply_->header(QNetworkRequest::ContentTypeHeader).toString();
	if (type.isEmpty() && status == 0)
		return; // headers not in yet
	decided_ = true;

	QString ext = extensionForContentType(type);
	if (ext.isEmpty()) {
		// "application/octet-stream" and friends: the address's own ending is
		// the best evidence there is. An HTML page is a page whatever it is
		// called.
		const QString name = reply_->url().path();
		const bool html = type.contains(QLatin1String("html"), Qt::CaseInsensitive);
		if (!html && isMediaFile(name))
			ext = QFileInfo(name).suffix().toLower();
	}
	if (ext.isEmpty()) {
		dropped_ = true;
		QNetworkReply *r = reply_;
		reply_ = nullptr;
		r->disconnect(this);
		r->abort();
		r->deleteLater();
		emit notMedia(url_, type);
		return;
	}
	if (!QDir().mkpath(folder_)) {
		dropped_ = true;
		QNetworkReply *r = reply_;
		reply_ = nullptr;
		r->disconnect(this);
		r->abort();
		r->deleteLater();
		emit failed(QStringLiteral("Could not create the folder for pasted files:\n%1")
				    .arg(QDir::toNativeSeparators(folder_)));
		return;
	}
	file_.setFileName(freePath(ext, reply_->url()));
	if (!file_.open(QIODevice::WriteOnly)) {
		dropped_ = true;
		QNetworkReply *r = reply_;
		reply_ = nullptr;
		r->disconnect(this);
		r->abort();
		r->deleteLater();
		emit failed(QStringLiteral("Could not write the download to:\n%1")
				    .arg(QDir::toNativeSeparators(file_.fileName())));
	}
}

void LinkDownload::finish()
{
	QNetworkReply *r = reply_;
	if (!r)
		return;
	decide(); // a body short enough to arrive with its headers
	if (!reply_)
		return; // decide() dropped it and has said why
	reply_ = nullptr;
	r->deleteLater();
	if (dropped_)
		return;
	if (file_.isOpen())
		file_.write(r->readAll());
	const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (r->error() != QNetworkReply::NoError || status >= 400) {
		if (file_.isOpen()) {
			file_.close();
			file_.remove();
		}
		emit failed(status >= 400 ? QStringLiteral("The server answered %1 for that link.").arg(status)
					  : r->errorString());
		return;
	}
	if (!file_.isOpen()) {
		emit failed(QStringLiteral("The link gave nothing to save."));
		return;
	}
	const qint64 size = file_.size();
	file_.close();
	if (size <= 0) {
		file_.remove();
		emit failed(QStringLiteral("The link gave an empty file."));
		return;
	}
	emit fileReady(file_.fileName());
}

QString LinkDownload::freePath(const QString &ext, const QUrl &from) const
{
	// The address's own name when it has a usable one -- the address it ended
	// at, so a short link is named for what it pointed to -- so the folder
	// stays browsable; otherwise a timestamp. A counter keeps a second paste of
	// the same link from overwriting the first, which a project may still use.
	QString base = QFileInfo(from.path()).completeBaseName();
	base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9 _.-]")), QStringLiteral("_"));
	base = base.trimmed().left(60);
	if (base.isEmpty())
		base = QStringLiteral("pasted-") +
		       QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
	QString path;
	for (int n = 0; n < 10000; ++n) {
		path = n == 0 ? QStringLiteral("%1/%2.%3").arg(folder_, base, ext)
			      : QStringLiteral("%1/%2-%3.%4").arg(folder_, base).arg(n).arg(ext);
		if (!QFileInfo::exists(path))
			break;
	}
	return path;
}

} // namespace harpia
