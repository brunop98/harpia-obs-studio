#pragma once

#include "AudioLibRemoteServerSound.hpp"

#include <QNetworkAccessManager>
#include <QSslCertificate>

#include <functional>
#include <optional>

class QNetworkReply;

namespace harpia {

class AudioLibRemoteServer : public QObject {
	Q_OBJECT
public:
	static constexpr int PageSize = 100;
	struct SearchOptions {
		QString query;
		QString name;
		QString path;
		QString extension;
		std::optional<double> minDuration;
		std::optional<double> maxDuration;
		QString orderBy = QStringLiteral("name");
		bool descending = false;
		bool playableOnly = false;
	};

	struct Page {
		QList<AudioLibRemoteServerSound> sounds;
		int total = 0;
	};

	struct Failure {
		QString message;
		QString report;
	};

	static AudioLibRemoteServer &shared();
	static QUrl serverAddress(QString address);
	static QString importsDir();

	const QUrl &address() const { return address_; }
	const QList<QSslCertificate> &certificates() const { return certificates_; }
	const QString &certificateName() const { return certificateName_; }
	const QString &password() const { return password_; }
	bool setConnection(const QUrl &address, const QList<QSslCertificate> &certificates,
			   const QString &password, const QString &certificateName = {});

	QNetworkReply *search(const SearchOptions &options, int offset);
	Page readPage(QNetworkReply &reply, Failure &failure) const;

	// Streams to a temporary file and publishes the complete file in folder.
	// The reply can be aborted by the caller. The callback runs on completion
	// while context is alive. A null reply reports a startup error in error.
	QNetworkReply *download(const AudioLibRemoteServerSound &sound, const QString &folder, QObject *context,
				std::function<void(const QString &file, const Failure &failure)> finished,
				QString &error);

signals:
	void connectionChanged();

private:
	explicit AudioLibRemoteServer(QObject *parent);
	bool isSameServer(const QUrl &url) const;
	QNetworkReply *get(const QUrl &url, const QString &operation);

	QUrl address_;
	QList<QSslCertificate> certificates_;
	QString certificateName_;
	QString password_;
	QNetworkAccessManager network_;
};

} // namespace harpia
