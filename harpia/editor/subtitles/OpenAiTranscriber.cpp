#include "OpenAiTranscriber.hpp"

#include "../../ui/EditorLog.hpp"

#include <QSslSocket>

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace harpia {

OpenAiTranscriber::OpenAiTranscriber(QObject *parent) : QObject(parent) {}

QByteArray OpenAiTranscriber::multipartBody(const Job &job, const QByteArray &wavBytes, const QByteArray &boundary)
{
	QByteArray b;
	const auto field = [&](const char *name, const QString &value) {
		b += "--" + boundary + "\r\n";
		b += "Content-Disposition: form-data; name=\"" + QByteArray(name) + "\"\r\n\r\n";
		b += value.toUtf8() + "\r\n";
	};
	field("model", QString::fromLatin1(kModel));
	field("response_format", QStringLiteral("verbose_json"));
	field("timestamp_granularities[]", QStringLiteral("word"));
	field("timestamp_granularities[]", QStringLiteral("segment"));
	if (!job.language.trimmed().isEmpty())
		field("language", job.language.trimmed());
	if (!job.prompt.trimmed().isEmpty())
		field("prompt", job.prompt.trimmed());
	b += "--" + boundary + "\r\n";
	b += "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n";
	b += "Content-Type: audio/wav\r\n\r\n";
	b += wavBytes;
	b += "\r\n--" + boundary + "--\r\n";
	return b;
}

void OpenAiTranscriber::transcribe(const Job &job)
{
	if (reply_) {
		emit failed(QStringLiteral("a transcription is already running"));
		return;
	}
	if (job.apiKey.trimmed().isEmpty()) {
		emit failed(QStringLiteral("no API key: add one in the Subtitles window"));
		return;
	}
	// No TLS backend means no HTTPS at all: say so before uploading anything.
	if (QSslSocket::availableBackends().isEmpty() || !QSslSocket::supportsSsl()) {
		const QString why = QStringLiteral(
			"secure connection (HTTPS) unavailable: no Qt TLS backend was found. The Qt TLS "
			"plugin (plugins/tls next to harpia.exe) is missing from this build.");
		editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"), why);
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
	if (wav.size() > 25 * 1024 * 1024) {
		emit failed(QStringLiteral("the audio piece is over the 25 MB upload limit"));
		return;
	}

	const QByteArray boundary = "----HarpiaSpeech" + QByteArray::number(qint64(QDateTime::currentMSecsSinceEpoch()), 36);
	const QByteArray body = multipartBody(job, wav, boundary);

	QNetworkRequest req(QUrl(QString::fromLatin1(kEndpoint)));
	req.setRawHeader("Authorization", "Bearer " + job.apiKey.trimmed().toUtf8());
	req.setHeader(QNetworkRequest::ContentTypeHeader,
		      QStringLiteral("multipart/form-data; boundary=%1").arg(QString::fromLatin1(boundary)));
	req.setTransferTimeout(4 * 60 * 1000); // a ten-minute chunk can take a while

	emit progress(QStringLiteral("Uploading %1 KB…").arg(wav.size() / 1024));
	reply_ = nam_.post(req, body);
	connect(reply_, &QNetworkReply::uploadProgress, this, [this](qint64 sent, qint64 total) {
		if (total > 0 && sent < total)
			emit progress(QStringLiteral("Uploading… %1%").arg(sent * 100 / total));
		else if (total > 0)
			emit progress(QStringLiteral("Transcribing…"));
	});
	connect(reply_, &QNetworkReply::finished, this, [this]() {
		QNetworkReply *r = reply_;
		reply_ = nullptr;
		r->deleteLater();
		const QByteArray data = r->readAll();
		const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (r->error() == QNetworkReply::OperationCanceledError) {
			emit failed(QStringLiteral("cancelled"));
			return;
		}
		QString err;
		const Transcript t = parseOpenAiVerboseJson(data, &err);
		// What came back, for the log: the status, Qt's network error, and the
		// start of the body. Never the key (it is only in the request header).
		const QString bodyStart = QString::fromUtf8(data.left(400)).simplified();
		const QString detail = QStringLiteral("HTTP %1, network error %2 (%3), %4 bytes back: %5")
					       .arg(http)
					       .arg(int(r->error()))
					       .arg(r->errorString())
					       .arg(data.size())
					       .arg(bodyStart.isEmpty() ? QStringLiteral("(empty)") : bodyStart);
		const bool notJson = err.startsWith(QLatin1String("not JSON"));
		if (r->error() != QNetworkReply::NoError || http >= 400 || notJson) {
			// The API's own message when it gave one (wrong key, quota, format).
			// A reply that is not JSON at all is not the API talking -- an
			// empty body from a failed connection, or a proxy's HTML page --
			// so the network error is what is shown then, not the parser's.
			QString why = (err.isEmpty() || notJson) ? r->errorString() : err;
			if (http == 0 && data.isEmpty())
				why = QStringLiteral("could not reach OpenAI: %1").arg(r->errorString());
			if (r->errorString().contains(QLatin1String("TLS"), Qt::CaseInsensitive) ||
			    r->errorString().contains(QLatin1String("SSL"), Qt::CaseInsensitive))
				why = QStringLiteral("secure connection (HTTPS) unavailable: %1. The Qt TLS plugin "
						     "(plugins/tls next to harpia.exe) may be missing from this build.")
					      .arg(r->errorString());
			else if (http == 401)
				why = QStringLiteral("the API key was rejected (401). Check it in the Subtitles window.");
			else if (http == 429)
				why = QStringLiteral("rate limited or out of credit (429): %1").arg(err);
			else if (notJson && http >= 200 && http < 300)
				why = QStringLiteral("OpenAI answered, but not with a transcript (HTTP %1). See the error "
						     "log for what came back.")
					      .arg(http);
			editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
				  QStringLiteral("transcription failed: %1 | %2").arg(why, detail));
			emit failed(why);
			return;
		}
		if (t.words.isEmpty()) {
			editorLog(EditorLogLevel::Warning, QStringLiteral("Subtitles"),
				  QStringLiteral("no words in the reply (%1) | %2").arg(err, detail));
			emit failed(err.isEmpty() ? QStringLiteral("no words came back") : err);
			return;
		}
		editorLog(EditorLogLevel::Info, QStringLiteral("Subtitles"),
			  QStringLiteral("transcribed %1 words, language %2").arg(t.words.size()).arg(t.language));
		emit finished(t);
	});
}

void OpenAiTranscriber::cancel()
{
	if (reply_)
		reply_->abort(); // finished() follows with OperationCanceledError
}

} // namespace harpia
