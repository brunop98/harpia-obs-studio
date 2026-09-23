#pragma once

// Speech to text over the network: OpenAI's transcription endpoint.
//
// One request per audio chunk, multipart/form-data, asking for verbose_json
// with word timestamps. The reply is parsed by Transcript.hpp; this class
// only moves bytes and reports. It never stores the key; the caller passes it
// in for each job and SecretStore keeps it between sessions.
//
// Why whisper-1 and not a newer model: as of this writing it is the one that
// returns WORD-level timestamps (timestamp_granularities), and word times are
// the whole point -- they are what "one word at a time" is made of. The model
// name is a single constant, so moving is a one-line change when that changes.

#include "Transcript.hpp"

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVector>

class QNetworkReply;

namespace harpia {

class OpenAiTranscriber : public QObject {
	Q_OBJECT
public:
	explicit OpenAiTranscriber(QObject *parent = nullptr);

	static constexpr const char *kModel = "whisper-1";
	static constexpr const char *kEndpoint = "https://api.openai.com/v1/audio/transcriptions";

	struct Job {
		QString wavPath;
		QString apiKey;
		QString language; // ISO-639-1 ("pt", "en"); empty = let the model detect
		QString prompt;   // optional vocabulary hint (names, jargon)
	};

	// Starts one request. Exactly one of finished()/failed() follows, once.
	void transcribe(const Job &job);
	void cancel();
	bool busy() const { return reply_ != nullptr; }

	// The request body, exposed so a test can check what goes on the wire
	// without a server. Boundary is fixed for the test's sake.
	static QByteArray multipartBody(const Job &job, const QByteArray &wavBytes, const QByteArray &boundary);

signals:
	void finished(const Transcript &t);
	void failed(const QString &why);
	void progress(const QString &what);

private:
	QNetworkAccessManager nam_;
	QNetworkReply *reply_ = nullptr;
};

} // namespace harpia
