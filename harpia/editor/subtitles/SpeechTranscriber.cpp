#include "SpeechTranscriber.hpp"

#include "../../ui/EditorLog.hpp"

#include <QSslSocket>

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QUrl>

namespace harpia {

namespace {
constexpr int kAssemblyPollMs = 3000;
constexpr qint64 kAssemblyGiveUpMs = 30 * 60 * 1000; // a ten-minute chunk is done long before
const QString kWhere = QStringLiteral("Subtitles");
} // namespace

SpeechTranscriber::SpeechTranscriber(QObject *parent) : QObject(parent)
{
	poll_.setSingleShot(true);
	connect(&poll_, &QTimer::timeout, this, &SpeechTranscriber::assemblyPoll);
}

QNetworkRequest SpeechTranscriber::authorized(SpeechProvider p, const QUrl &url, const QString &apiKey)
{
	QNetworkRequest req(url);
	const QByteArray key = apiKey.trimmed().toUtf8();
	switch (p) {
	case SpeechProvider::OpenAI:
	case SpeechProvider::Groq: req.setRawHeader("Authorization", "Bearer " + key); break;
	case SpeechProvider::Deepgram: req.setRawHeader("Authorization", "Token " + key); break;
	case SpeechProvider::AssemblyAI: req.setRawHeader("Authorization", key); break;
	case SpeechProvider::ElevenLabs: req.setRawHeader("xi-api-key", key); break;
	}
	req.setTransferTimeout(4 * 60 * 1000); // a ten-minute chunk can take a while
	return req;
}

void SpeechTranscriber::transcribe(const Job &job)
{
	if (busy()) {
		emit failed(QStringLiteral("a transcription is already running"));
		return;
	}
	const SpeechProviderInfo &info = speechProviderInfo(job.provider);
	const QString name = speechProviderName(job.provider);
	if (job.apiKey.trimmed().isEmpty()) {
		emit failed(QStringLiteral("no %1 API key: add one in the Subtitles window").arg(name));
		return;
	}
	// No TLS backend means no HTTPS at all: say so before uploading anything.
	if (QSslSocket::availableBackends().isEmpty() || !QSslSocket::supportsSsl()) {
		const QString why = QStringLiteral(
			"secure connection (HTTPS) unavailable: no Qt TLS backend was found. The Qt TLS "
			"plugin (plugins/tls next to harpia.exe) is missing from this build.");
		editorLog(EditorLogLevel::Error, kWhere, why);
		emit failed(why);
		return;
	}
	QFile f(job.wavPath);
	if (!f.open(QIODevice::ReadOnly)) {
		emit failed(QStringLiteral("could not read %1").arg(QFileInfo(job.wavPath).fileName()));
		return;
	}
	const QByteArray wav = f.readAll();
	f.close();
	if (info.uploadLimit25Mb && wav.size() > 25 * 1024 * 1024) {
		emit failed(QStringLiteral("the audio piece is over %1's 25 MB upload limit").arg(name));
		return;
	}
	job_ = job;
	assemblyId_.clear();
	editorLog(EditorLogLevel::Info, kWhere,
		  QStringLiteral("sending %1 KB to %2%3").arg(wav.size() / 1024).arg(name,
			  *info.model ? QStringLiteral(" (%1)").arg(QString::fromUtf8(info.model)) : QString()));
	emit progress(QStringLiteral("Uploading %1 KB to %2…").arg(wav.size() / 1024).arg(name));

	const auto finalStep = [this](const QByteArray &data) { finish(data); };
	const QByteArray boundary = "----HarpiaSpeech" + QByteArray::number(qint64(QDateTime::currentMSecsSinceEpoch()), 36);
	const QString multipart = QStringLiteral("multipart/form-data; boundary=%1").arg(QString::fromLatin1(boundary));

	switch (job.provider) {
	case SpeechProvider::OpenAI:
	case SpeechProvider::Groq: {
		const QUrl url(job.provider == SpeechProvider::Groq
				       ? QStringLiteral("https://api.groq.com/openai/v1/audio/transcriptions")
				       : QStringLiteral("https://api.openai.com/v1/audio/transcriptions"));
		QNetworkRequest req = authorized(job.provider, url, job.apiKey);
		req.setHeader(QNetworkRequest::ContentTypeHeader, multipart);
		send(req, "POST", openAiStyleBody(job, wav, boundary), finalStep, true);
		break;
	}
	case SpeechProvider::ElevenLabs: {
		QNetworkRequest req =
			authorized(job.provider, QUrl(QStringLiteral("https://api.elevenlabs.io/v1/speech-to-text")), job.apiKey);
		req.setHeader(QNetworkRequest::ContentTypeHeader, multipart);
		send(req, "POST", elevenLabsBody(job, wav, boundary), finalStep, true);
		break;
	}
	case SpeechProvider::Deepgram: {
		QNetworkRequest req = authorized(job.provider, deepgramUrl(job), job.apiKey);
		req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("audio/wav"));
		send(req, "POST", wav, finalStep, true);
		break;
	}
	case SpeechProvider::AssemblyAI: {
		// 1. Upload the audio. 2. Create the job. 3. Poll until it is done.
		QNetworkRequest up =
			authorized(job.provider, QUrl(QStringLiteral("https://api.assemblyai.com/v2/upload")), job.apiKey);
		up.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
		send(up, "POST", wav,
		     [this](const QByteArray &data) {
			     const QString uploadUrl = parseAssemblyAiUploadUrl(data);
			     if (uploadUrl.isEmpty()) {
				     const QString why = QStringLiteral("AssemblyAI took the upload but did not say where it went");
				     editorLog(EditorLogLevel::Error, kWhere,
					       QStringLiteral("%1 | %2").arg(why, QString::fromUtf8(data.left(400)).simplified()));
				     emit failed(why);
				     return;
			     }
			     emit progress(QStringLiteral("Starting the AssemblyAI job…"));
			     QNetworkRequest create = authorized(
				     job_.provider, QUrl(QStringLiteral("https://api.assemblyai.com/v2/transcript")), job_.apiKey);
			     create.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
			     send(create, "POST", assemblyAiCreateBody(uploadUrl, job_.language),
				  [this](const QByteArray &created) {
					  const AssemblyAiStatus s = parseAssemblyAiStatus(created);
					  if (s.id.isEmpty()) {
						  const QString why = s.error.isEmpty()
									      ? QStringLiteral("AssemblyAI did not start a job")
									      : QStringLiteral("AssemblyAI: %1").arg(s.error);
						  editorLog(EditorLogLevel::Error, kWhere, why);
						  emit failed(why);
						  return;
					  }
					  assemblyId_ = s.id;
					  pollStartMs_ = QDateTime::currentMSecsSinceEpoch();
					  emit progress(QStringLiteral("Transcribing (AssemblyAI)…"));
					  poll_.start(kAssemblyPollMs);
				  },
				  false);
		     },
		     true);
		break;
	}
	}
}

void SpeechTranscriber::assemblyPoll()
{
	if (QDateTime::currentMSecsSinceEpoch() - pollStartMs_ > kAssemblyGiveUpMs) {
		const QString why = QStringLiteral("AssemblyAI has not finished after 30 minutes; giving up on this part");
		editorLog(EditorLogLevel::Error, kWhere, QStringLiteral("%1 (job %2)").arg(why, assemblyId_));
		emit failed(why);
		return;
	}
	const QNetworkRequest req = authorized(
		job_.provider, QUrl(QStringLiteral("https://api.assemblyai.com/v2/transcript/") + assemblyId_), job_.apiKey);
	send(req, "GET", QByteArray(),
	     [this](const QByteArray &data) {
		     const AssemblyAiStatus s = parseAssemblyAiStatus(data);
		     if (s.status == QLatin1String("completed") || s.status == QLatin1String("error")) {
			     finish(data);
			     return;
		     }
		     const qint64 waited = (QDateTime::currentMSecsSinceEpoch() - pollStartMs_) / 1000;
		     emit progress(QStringLiteral("Transcribing (AssemblyAI, %1, %2 s)…")
					   .arg(s.status.isEmpty() ? QStringLiteral("waiting") : s.status)
					   .arg(waited));
		     poll_.start(kAssemblyPollMs);
	     },
	     false);
}

void SpeechTranscriber::send(const QNetworkRequest &req, const QByteArray &verb, const QByteArray &body,
			     std::function<void(const QByteArray &)> onOk, bool uploadProgress)
{
	reply_ = verb == "GET" ? nam_.get(req) : nam_.sendCustomRequest(req, verb, body);
	if (uploadProgress)
		connect(reply_, &QNetworkReply::uploadProgress, this, [this](qint64 sent, qint64 total) {
			if (total > 0 && sent < total)
				emit progress(QStringLiteral("Uploading… %1%").arg(sent * 100 / total));
			else if (total > 0)
				emit progress(QStringLiteral("Transcribing (%1)…").arg(speechProviderName(job_.provider)));
		});
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
		// What came back, for the log: the status, Qt's network error, and the
		// start of the body. Never the key (it is only in the request header).
		const QString name = speechProviderName(job_.provider);
		const QString api = speechErrorMessage(job_.provider, data);
		const QString bodyStart = QString::fromUtf8(data.left(400)).simplified();
		const QString detail = QStringLiteral("%1 %2: HTTP %3, network error %4 (%5), %6 bytes back: %7")
					       .arg(name, r->url().path())
					       .arg(http)
					       .arg(int(r->error()))
					       .arg(r->errorString())
					       .arg(data.size())
					       .arg(bodyStart.isEmpty() ? QStringLiteral("(empty)") : bodyStart);
		// The service's own message when it gave one (wrong key, quota,
		// format). A body that is not the service talking -- empty from a
		// failed connection, or a proxy's HTML page -- shows the network error.
		QString why = api.isEmpty() ? r->errorString() : QStringLiteral("%1: %2").arg(name, api);
		if (http == 0 && data.isEmpty())
			why = QStringLiteral("could not reach %1: %2").arg(name, r->errorString());
		if (r->errorString().contains(QLatin1String("TLS"), Qt::CaseInsensitive) ||
		    r->errorString().contains(QLatin1String("SSL"), Qt::CaseInsensitive))
			why = QStringLiteral("secure connection (HTTPS) unavailable: %1. The Qt TLS plugin "
					     "(plugins/tls next to harpia.exe) may be missing from this build.")
				      .arg(r->errorString());
		else if (http == 401 || http == 403)
			why = QStringLiteral("%1 rejected the API key (%2)%3. Check it in the Subtitles window.")
				      .arg(name)
				      .arg(http)
				      .arg(api.isEmpty() ? QString() : QStringLiteral(": ") + api);
		else if (http == 402 || http == 429)
			why = QStringLiteral("%1: rate limited or out of credit (%2)%3")
				      .arg(name)
				      .arg(http)
				      .arg(api.isEmpty() ? QString() : QStringLiteral(": ") + api);
		editorLog(EditorLogLevel::Error, kWhere, QStringLiteral("transcription failed: %1 | %2").arg(why, detail));
		emit failed(why);
	});
}

void SpeechTranscriber::finish(const QByteArray &data)
{
	const QString name = speechProviderName(job_.provider);
	QString err;
	const Transcript t = parseSpeechReply(job_.provider, data, &err);
	const QString detail = QStringLiteral("%1, %2 bytes back: %3")
				       .arg(name)
				       .arg(data.size())
				       .arg(QString::fromUtf8(data.left(400)).simplified());
	if (err.startsWith(QLatin1String("not JSON"))) {
		const QString why = QStringLiteral("%1 answered, but not with a transcript. See the error log for what came back.")
					    .arg(name);
		editorLog(EditorLogLevel::Error, kWhere, QStringLiteral("%1 | %2").arg(why, detail));
		emit failed(why);
		return;
	}
	if (t.words.isEmpty()) {
		editorLog(EditorLogLevel::Warning, kWhere, QStringLiteral("no words in the reply (%1) | %2").arg(err, detail));
		emit failed(err.isEmpty() ? QStringLiteral("no words came back") : QStringLiteral("%1: %2").arg(name, err));
		return;
	}
	editorLog(EditorLogLevel::Info, kWhere,
		  QStringLiteral("%1 transcribed %2 words, language %3").arg(name).arg(t.words.size()).arg(t.language));
	emit finished(t);
}

void SpeechTranscriber::cancel()
{
	if (reply_) {
		reply_->abort(); // finished() follows with OperationCanceledError
		return;
	}
	if (poll_.isActive()) {
		poll_.stop();
		emit failed(QStringLiteral("cancelled"));
	}
}

} // namespace harpia
