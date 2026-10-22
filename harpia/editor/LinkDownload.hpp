#pragma once

// A pasted web link, fetched as a file -- if it is one.
//
// A copied link is either a media file (a picture, a sound, a video: "the
// thing itself") or a page that has one in it (YouTube, a tweet). Only the
// server knows which, and it says so in the response headers, before any of
// the body has to be read. So: one GET; a picture, sound or video is streamed
// to a file and handed back; a page (or anything else) is dropped right there
// and reported, so the caller can give it to yt-dlp instead.
//
// What it is comes from the Content-Type, and from the address's own file
// extension when the server only says "octet-stream" (common on storage
// buckets and CDNs).

#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkReply;

namespace harpia {

class LinkDownload : public QObject {
	Q_OBJECT
public:
	explicit LinkDownload(QObject *parent = nullptr);
	~LinkDownload() override;

	// Fetch `url` into `folder` (made if missing). Ends in exactly one of
	// fileReady, notMedia or failed -- or nothing at all after cancel().
	void start(const QUrl &url, const QString &folder);
	void cancel();
	bool running() const { return !reply_.isNull(); }

signals:
	void progress(qint64 received, qint64 total); // total is -1 when not known
	void fileReady(const QString &path);
	// Not a picture, sound or video: nothing was saved. `contentType` as the
	// server gave it ("text/html; charset=utf-8" for a page).
	void notMedia(const QUrl &url, const QString &contentType);
	void failed(const QString &why);

private:
	void decide();
	void finish();
	QString freePath(const QString &ext, const QUrl &from) const;

	QNetworkAccessManager nam_;
	QPointer<QNetworkReply> reply_;
	QUrl url_;
	QString folder_;
	QFile file_;
	bool decided_ = false;
	bool dropped_ = false; // notMedia already said, or cancelled: say nothing more
};

} // namespace harpia
