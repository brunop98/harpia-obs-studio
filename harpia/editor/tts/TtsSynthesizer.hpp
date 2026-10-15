#pragma once

// Text to speech over the network, with the service the user picked: one
// request per piece of text (TtsProviders.hpp says what each service wants
// and how its reply becomes audio and word times), and the account's voices
// for Load my voices.
//
// It never stores the key: the caller passes it in with each request and
// SecretStore keeps it between sessions. Logs carry the status and the start
// of the reply, never the key (it only ever travels in a request header).

#include "TtsProviders.hpp"

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

class QNetworkReply;
class QNetworkRequest;

namespace harpia {

class TtsSynthesizer : public QObject {
	Q_OBJECT
public:
	explicit TtsSynthesizer(QObject *parent = nullptr);

	// One piece of text. Exactly one of synthesized()/failed() follows, once.
	void synthesize(const TtsRequest &r);
	// The account's voices (ElevenLabs, Google). voicesListed() or failed().
	void listVoices(TtsProvider p, const QString &apiKey, const QString &language);
	void cancel();
	bool busy() const { return reply_ != nullptr; }

signals:
	void synthesized(const harpia::TtsReply &reply);
	void voicesListed(const QVector<harpia::TtsVoice> &voices);
	void failed(const QString &why);

private:
	void send(const QNetworkRequest &req, const QByteArray &verb, const QByteArray &body,
		  std::function<void(const QByteArray &)> onOk);
	bool httpsReady();

	QNetworkAccessManager nam_;
	QNetworkReply *reply_ = nullptr;
	TtsProvider provider_ = TtsProvider::OpenAI;
};

} // namespace harpia
