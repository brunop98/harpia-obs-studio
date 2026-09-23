#include "OpenAiTranscriber.hpp"

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
		if (r->error() != QNetworkReply::NoError || http >= 400) {
			// The API's own message when it gave one (wrong key, quota, format),
			// else Qt's. Never the key.
			QString why = err.isEmpty() ? r->errorString() : err;
			if (http == 401)
				why = QStringLiteral("the API key was rejected (401). Check it in the Subtitles window.");
			else if (http == 429)
				why = QStringLiteral("rate limited or out of credit (429): %1").arg(err);
			emit failed(why);
			return;
		}
		if (t.words.isEmpty()) {
			emit failed(err.isEmpty() ? QStringLiteral("no words came back") : err);
			return;
		}
		emit finished(t);
	});
}

void OpenAiTranscriber::cancel()
{
	if (reply_)
		reply_->abort(); // finished() follows with OperationCanceledError
}

} // namespace harpia
