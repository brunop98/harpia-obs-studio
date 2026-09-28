#pragma once

// Speech to text over the network, with the service the user picked.
//
// One job per audio chunk. What each service wants on the wire, and how its
// reply becomes a Transcript, lives in SpeechProviders.hpp; this class only
// moves bytes, runs AssemblyAI's upload/create/poll steps, and reports. It
// never stores the key: the caller passes it in for each job and SecretStore
// keeps it between sessions. Logs carry the status and the start of the
// reply, never the key (it only ever travels in a request header).

#include "SpeechProviders.hpp"
#include "Transcript.hpp"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

class QNetworkReply;

namespace harpia {

class SpeechTranscriber : public QObject {
	Q_OBJECT
public:
	explicit SpeechTranscriber(QObject *parent = nullptr);

	using Job = SpeechJob;

	// Starts one job. Exactly one of finished()/failed() follows, once.
	void transcribe(const Job &job);
	void cancel();
	bool busy() const { return reply_ != nullptr || poll_.isActive(); }

	// The OpenAI/Groq request body, kept here so a test can check what goes on
	// the wire without a server. Boundary is fixed for the test's sake.
	static QByteArray multipartBody(const Job &job, const QByteArray &wavBytes, const QByteArray &boundary)
	{
		return openAiStyleBody(job, wavBytes, boundary);
	}

	// The request for each step, headers included: exposed so a test can check
	// that the key goes where each service expects it.
	static QNetworkRequest authorized(SpeechProvider p, const QUrl &url, const QString &apiKey);

signals:
	void finished(const Transcript &t);
	void failed(const QString &why);
	void progress(const QString &what);

private:
	// One HTTP step. onOk gets the body of a 2xx reply; everything else
	// becomes failed() with the clearest reason available, and is logged.
	void send(const QNetworkRequest &req, const QByteArray &verb, const QByteArray &body,
		  std::function<void(const QByteArray &)> onOk, bool uploadProgress);
	void finish(const QByteArray &data);
	void assemblyPoll();

	QNetworkAccessManager nam_;
	QNetworkReply *reply_ = nullptr;
	Job job_;
	QTimer poll_;          // AssemblyAI: time until the next status check
	QString assemblyId_;
	qint64 pollStartMs_ = 0;
};

} // namespace harpia
