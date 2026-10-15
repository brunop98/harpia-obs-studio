#include "TtsSynthesizer.hpp"

#include "../../ui/EditorLog.hpp"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>

namespace harpia {

namespace {
const QString kWhere = QStringLiteral("Text to speech");

QNetworkRequest request(TtsProvider p, const QUrl &url, const QString &key)
{
	QNetworkRequest req(url);
	for (const auto &h : ttsHeaders(p, key))
		req.setRawHeader(h.first, h.second);
	req.setTransferTimeout(3 * 60 * 1000); // a long piece takes a while to speak
	return req;
}
} // namespace

TtsSynthesizer::TtsSynthesizer(QObject *parent) : QObject(parent)
{
	qRegisterMetaType<harpia::TtsReply>("harpia::TtsReply");
	qRegisterMetaType<QVector<harpia::TtsVoice>>("QVector<harpia::TtsVoice>");
}

bool TtsSynthesizer::httpsReady()
{
	// No TLS backend means no HTTPS at all: say so before sending anything.
	if (!QSslSocket::availableBackends().isEmpty() && QSslSocket::supportsSsl())
		return true;
	const QString why = QStringLiteral(
		"secure connection (HTTPS) unavailable: no Qt TLS backend was found. The Qt TLS "
		"plugin (plugins/tls next to harpia.exe) is missing from this build.");
	editorLog(EditorLogLevel::Error, kWhere, why);
	emit failed(why);
	return false;
}

void TtsSynthesizer::synthesize(const TtsRequest &r)
{
	if (busy()) {
		emit failed(QStringLiteral("a request is already running"));
		return;
	}
	const QString name = ttsProviderName(r.provider);
	if (r.apiKey.trimmed().isEmpty()) {
		emit failed(QStringLiteral("no %1 API key: paste one in the Text to speech window").arg(name));
		return;
	}
	if (r.voice.trimmed().isEmpty()) {
		emit failed(QStringLiteral("no voice chosen"));
		return;
	}
	if (!httpsReady())
		return;
	provider_ = r.provider;
	editorLog(EditorLogLevel::Info, kWhere,
		  QStringLiteral("asking %1 for %2 characters, voice %3").arg(name).arg(r.text.size()).arg(r.voice));
	// Word times are found on the plain spoken words, never on the marks.
	const QString text = r.timingText.isEmpty() ? r.text : r.timingText;
	send(request(r.provider, ttsUrl(r), r.apiKey), "POST", ttsBody(r), [this, text, name](const QByteArray &data) {
		QString err;
		const TtsReply reply = parseTtsReply(provider_, data, text, &err);
		if (reply.audio.isEmpty() || !err.isEmpty()) {
			const QString why = err.isEmpty() ? QStringLiteral("%1 sent no audio").arg(name)
							  : QStringLiteral("%1: %2").arg(name, err);
			editorLog(EditorLogLevel::Error, kWhere,
				  QStringLiteral("%1 | %2 bytes back: %3")
					  .arg(why)
					  .arg(data.size())
					  .arg(QString::fromUtf8(data.left(300)).simplified()));
			emit failed(why);
			return;
		}
		editorLog(EditorLogLevel::Info, kWhere,
			  QStringLiteral("%1 sent %2 KB of %3%4")
				  .arg(name)
				  .arg(reply.audio.size() / 1024)
				  .arg(reply.ext)
				  .arg(reply.timed ? QStringLiteral(", with word times") : QString()));
		emit synthesized(reply);
	});
}

void TtsSynthesizer::listVoices(TtsProvider p, const QString &apiKey, const QString &language)
{
	if (busy()) {
		emit failed(QStringLiteral("a request is already running"));
		return;
	}
	const QString name = ttsProviderName(p);
	if (apiKey.trimmed().isEmpty()) {
		emit failed(QStringLiteral("paste your %1 API key first").arg(name));
		return;
	}
	if (!httpsReady())
		return;
	provider_ = p;
	QNetworkRequest req = request(p, ttsVoicesUrl(p, language), apiKey);
	send(req, "GET", QByteArray(), [this, p, name](const QByteArray &data) {
		const QVector<TtsVoice> voices = parseTtsVoices(p, data);
		if (voices.isEmpty()) {
			editorLog(EditorLogLevel::Warning, kWhere,
				  QStringLiteral("%1 listed no voices | %2").arg(name, QString::fromUtf8(data.left(300)).simplified()));
			emit failed(QStringLiteral("%1 listed no voices for this language").arg(name));
			return;
		}
		editorLog(EditorLogLevel::Info, kWhere, QStringLiteral("%1 listed %2 voices").arg(name).arg(voices.size()));
		emit voicesListed(voices);
	});
}

void TtsSynthesizer::send(const QNetworkRequest &req, const QByteArray &verb, const QByteArray &body,
			  std::function<void(const QByteArray &)> onOk)
{
	reply_ = verb == "GET" ? nam_.get(req) : nam_.sendCustomRequest(req, verb, body);
	connect(reply_, &QNetworkReply::finished, this, [this, onOk = std::move(onOk)]() {
		QNetworkReply *r = reply_;
		reply_ = nullptr;
		r->deleteLater();
		const QByteArray data = r->readAll();
		const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (r->error() == QNetworkReply::OperationCanceledError) {
			emit failed(QStringLiteral("cancelled"));
			return;
		}
		if (r->error() == QNetworkReply::NoError && http >= 200 && http < 300) {
			onOk(data);
			return;
		}
		// What came back, for the log: the status, Qt's network error and the
		// start of the body. Never the key (it is only in a request header).
		const QString name = ttsProviderName(provider_);
		const QString api = ttsErrorMessage(provider_, data);
		const QString bodyStart = QString::fromUtf8(data.left(400)).simplified();
		const QString detail = QStringLiteral("%1 %2: HTTP %3, network error %4 (%5), %6 bytes back: %7")
					       .arg(name, r->url().path())
					       .arg(http)
					       .arg(int(r->error()))
					       .arg(r->errorString())
					       .arg(data.size())
					       .arg(bodyStart.isEmpty() ? QStringLiteral("(empty)") : bodyStart);
		QString why = api.isEmpty() ? r->errorString() : QStringLiteral("%1: %2").arg(name, api);
		if (http == 0 && data.isEmpty())
			why = QStringLiteral("could not reach %1: %2").arg(name, r->errorString());
		if (r->errorString().contains(QLatin1String("TLS"), Qt::CaseInsensitive) ||
		    r->errorString().contains(QLatin1String("SSL"), Qt::CaseInsensitive))
			why = QStringLiteral("secure connection (HTTPS) unavailable: %1. The Qt TLS plugin "
					     "(plugins/tls next to harpia.exe) may be missing from this build.")
				      .arg(r->errorString());
		else if (http == 401 || http == 403)
			why = QStringLiteral("%1 rejected the API key (%2)%3. Check it, and that it may use text to speech.")
				      .arg(name)
				      .arg(http)
				      .arg(api.isEmpty() ? QString() : QStringLiteral(": ") + api);
		else if (http == 402 || http == 429)
			why = QStringLiteral("%1: rate limited or out of credit (%2)%3")
				      .arg(name)
				      .arg(http)
				      .arg(api.isEmpty() ? QString() : QStringLiteral(": ") + api);
		editorLog(EditorLogLevel::Error, kWhere, QStringLiteral("failed: %1 | %2").arg(why, detail));
		emit failed(why);
	});
}

void TtsSynthesizer::cancel()
{
	if (reply_)
		reply_->abort(); // finished() follows with OperationCanceledError
}

} // namespace harpia
