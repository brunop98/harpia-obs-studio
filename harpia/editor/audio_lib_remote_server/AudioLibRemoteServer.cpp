#include "AudioLibRemoteServer.hpp"

#ifdef Q_OS_WIN
#include "AudioLibRemoteServerWindowsTrust.hpp"

#include <QHostAddress>
#endif

#include "../MediaFiles.hpp"
#include "../subtitles/SecretStore.hpp"
#include "../../Version.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMetaEnum>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <algorithm>
#include <memory>

namespace harpia {
namespace {

// Keep the settings key used by existing XA1 profiles.
constexpr auto kConnectionKey = "xa1/connection";
constexpr auto kTlsError = "xa1TlsError";
constexpr auto kTlsIssues = "xa1TlsIssues";
constexpr auto kRequestDetails = "xa1RequestDetails";

QSettings connectionSettings()
{
	return QSettings(QSettings::defaultFormat(), QSettings::UserScope,
			 QStringLiteral("Harpia"), QStringLiteral("Recorder"));
}

QString certificateError(const QList<QSslError> &errors)
{
	const auto has = [&errors](QSslError::SslError reason) {
		return std::any_of(errors.cbegin(), errors.cend(),
				   [reason](const QSslError &error) { return error.error() == reason; });
	};
	if (has(QSslError::HostNameMismatch))
		return QStringLiteral("The XA1 certificate does not match this address. Copy the HTTPS address shown in Servidor XA1.");
	if (has(QSslError::CertificateExpired) || has(QSslError::CertificateNotYetValid))
		return QStringLiteral("The XA1 certificate is outside its validity dates. Check the clocks on the XA1 and this PC.");
	return QStringLiteral("Could not verify XA1 HTTPS. In Connection…, select certificado-rede-xa1.cer.");
}

template<typename Enum>
QString enumName(Enum value)
{
	const char *key = QMetaEnum::fromType<Enum>().valueToKey(int(value));
	return key ? QString::fromLatin1(key) : QStringLiteral("Unknown");
}

QString certificateFingerprint(const QSslCertificate &certificate)
{
	return QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex(':').toUpper());
}

QStringList certificateDetails(const QString &label, const QSslCertificate &certificate)
{
	QStringList lines{label};
	const auto commonName = [](const QStringList &values) {
		const QString name = values.join(QStringLiteral(", ")).simplified();
		return name.isEmpty() ? QStringLiteral("(no common name)") : name;
	};
	lines << QStringLiteral("  Subject CN: %1").arg(commonName(certificate.subjectInfo(QSslCertificate::CommonName)))
	      << QStringLiteral("  Issuer CN: %1").arg(commonName(certificate.issuerInfo(QSslCertificate::CommonName)))
	      << QStringLiteral("  SHA-256: %1").arg(certificateFingerprint(certificate))
	      << QStringLiteral("  Validity: %1 to %2")
			     .arg(certificate.effectiveDate().toUTC().toString(Qt::ISODate),
				  certificate.expiryDate().toUTC().toString(Qt::ISODate));
	const auto alternatives = certificate.subjectAlternativeNames();
	QStringList names = alternatives.values(QSsl::DnsEntry);
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
	names.append(alternatives.values(QSsl::IpAddressEntry));
#endif
	if (!names.isEmpty())
		lines << QStringLiteral("  DNS/IP names: %1").arg(names.join(QStringLiteral(", ")).simplified());
	return lines;
}

QString serverOrigin(const QUrl &url)
{
	return url.adjusted(QUrl::RemoveUserInfo | QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment)
		.toString(QUrl::FullyEncoded);
}

QString transportError(const QNetworkReply &reply)
{
	// HTTP errors can embed the request target in errorString(). Only use
	// socket/TLS messages so searches and media paths stay out of the report.
	switch (reply.error()) {
	case QNetworkReply::ConnectionRefusedError:
	case QNetworkReply::RemoteHostClosedError:
	case QNetworkReply::HostNotFoundError:
	case QNetworkReply::TimeoutError:
	case QNetworkReply::OperationCanceledError:
	case QNetworkReply::SslHandshakeFailedError:
	case QNetworkReply::TemporaryNetworkFailureError:
	case QNetworkReply::NetworkSessionFailedError: {
		QString detail = reply.errorString();
		const QUrl url = reply.url();
		const QString origin = serverOrigin(url);
		detail.replace(url.toString(QUrl::FullyEncoded), origin);
		detail.replace(url.toString(), origin);
		return detail.simplified();
	}
	default:
		return enumName(reply.error());
	}
}

QString requestError(const QNetworkReply &reply)
{
	const QString tlsError = reply.property(kTlsError).toString();
	if (!tlsError.isEmpty())
		return tlsError;
	const int status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (reply.error() == QNetworkReply::NoError && status == 200)
		return {};
	if (status == 401)
		return QStringLiteral("Enter the XA1 password in Connection…");
	if (status == 404)
		return QStringLiteral("The XA1 could not find this item. Check the address or refresh the catalog.");
	if (status == 503)
		return QStringLiteral("The XA1 catalog is getting ready. Try Refresh in a moment.");
	if (reply.error() == QNetworkReply::SslHandshakeFailedError)
		return QStringLiteral("XA1 HTTPS failed. %1").arg(transportError(reply));
	if (status == 200)
		return QStringLiteral("The XA1 transfer did not complete. %1").arg(transportError(reply));
	if (status != 0)
		return QStringLiteral("The XA1 replied with HTTP %1.").arg(status);
	return QStringLiteral("Could not reach the XA1. %1").arg(transportError(reply));
}

AudioLibRemoteServer::Failure describeFailure(const QNetworkReply &reply, const QString &message)
{
	QStringList lines{
		QStringLiteral("XA1 request failed"),
		QStringLiteral("Time (UTC): %1").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)),
		QStringLiteral("Harpia: %1").arg(QString::fromLatin1(appVersion())),
		QStringLiteral("Message: %1").arg(message),
	};
	lines.append(reply.property(kRequestDetails).toStringList());
	const int status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	lines << QStringLiteral("HTTP status: %1").arg(status > 0 ? QString::number(status)
								 : QStringLiteral("No response"))
	      << QStringLiteral("Qt network error: %1 (%2)").arg(enumName(reply.error())).arg(int(reply.error()));
	const QString detail = transportError(reply);
	if (detail != enumName(reply.error()) && !message.contains(detail))
		lines << QStringLiteral("Transport error: %1").arg(detail);
	const auto ssl = reply.sslConfiguration();
	if (ssl.sessionProtocol() != QSsl::UnknownProtocol)
		lines << QStringLiteral("TLS protocol: %1").arg(enumName(ssl.sessionProtocol()));
	if (ssl.peerCertificate().isNull())
		lines << QStringLiteral("Peer certificate: unavailable");
	else
		lines.append(certificateDetails(QStringLiteral("Peer certificate:"), ssl.peerCertificate()));
	const QStringList issues = reply.property(kTlsIssues).toStringList();
	if (!issues.isEmpty()) {
		lines << QStringLiteral("TLS verification errors:");
		lines.append(issues);
	}
	return {message, lines.join(QLatin1Char('\n'))};
}

QString localFileName(const QString &name)
{
	const QFileInfo file(name);
	QString base = file.completeBaseName().left(100);
	static const QRegularExpression invalid(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
	static const QRegularExpression reserved(
		QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])($|\\.)"),
		QRegularExpression::CaseInsensitiveOption);
	base.replace(invalid, QStringLiteral("_"));
	base = base.trimmed();
	if (base.isEmpty() || reserved.match(base).hasMatch())
		base.prepend(QStringLiteral("XA1 "));
	return base + QLatin1Char('.') + file.suffix().toLower();
}

} // namespace

AudioLibRemoteServer &AudioLibRemoteServer::shared()
{
	static auto *catalog = new AudioLibRemoteServer(QCoreApplication::instance());
	return *catalog;
}

AudioLibRemoteServer::AudioLibRemoteServer(QObject *parent) : QObject(parent)
{
	QSettings settings = connectionSettings();
	const QVariantMap saved = settings.value(QLatin1String(kConnectionKey)).toMap();
	if (saved.isEmpty()) {
		// Older builds stored the address and certificate as separate values.
		address_ = serverAddress(settings.value(QStringLiteral("xa1/address")).toString());
		certificates_ = QSslCertificate::fromData(settings.value(QStringLiteral("xa1/caCertificates")).toByteArray());
		certificateName_ = settings.value(QStringLiteral("xa1/caCertificateName")).toString();
	} else {
		address_ = serverAddress(saved.value(QStringLiteral("address")).toString());
		certificates_ = QSslCertificate::fromData(saved.value(QStringLiteral("caCertificates")).toByteArray());
		certificateName_ = saved.value(QStringLiteral("certificateName")).toString();
		const QByteArray sealed = saved.value(QStringLiteral("sealedPassword")).toByteArray();
		password_ = QString::fromUtf8(SecretStore::unsealForUser(sealed));
	}
	network_.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
	network_.setAutoDeleteReplies(true);
}

QUrl AudioLibRemoteServer::serverAddress(QString address)
{
	address = address.trimmed();
	if (address.isEmpty())
		return {};
	if (!address.contains(QLatin1String("://")))
		address.prepend(QStringLiteral("https://"));
	QUrl url(address, QUrl::StrictMode);
	if (!url.isValid() || url.scheme() != QLatin1String("https") || url.host().isEmpty() ||
	    !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
	    (!url.path().isEmpty() && url.path() != QLatin1String("/")) || url.port() == 0)
		return {};
	url.setPort(url.port(8443));
	url.setPath(QString());
	return url;
}

bool AudioLibRemoteServer::setConnection(const QUrl &address, const QList<QSslCertificate> &certificates,
			       const QString &password, const QString &certificateName)
{
	QVariantMap saved;
	saved.insert(QStringLiteral("address"), address.toString());
	saved.insert(QStringLiteral("certificateName"), certificateName);
	QByteArray pem;
	for (const QSslCertificate &certificate : certificates)
		pem += certificate.toPem();
	saved.insert(QStringLiteral("caCertificates"), pem);
#ifdef Q_OS_WIN
	if (!password.isEmpty()) {
		const QByteArray sealed = SecretStore::sealForUser(password.toUtf8());
		if (sealed.isEmpty())
			return false;
		saved.insert(QStringLiteral("sealedPassword"), sealed);
	}
#endif
	// Save the credential and its server together so a partial update cannot
	// associate the saved password with a different endpoint.
	QSettings settings = connectionSettings();
	settings.setValue(QLatin1String(kConnectionKey), saved);
	settings.sync();
	if (settings.status() != QSettings::NoError)
		return false;

	address_ = address;
	certificates_ = certificates;
	certificateName_ = certificateName;
	password_ = password;
	network_.clearAccessCache();
	emit connectionChanged();
	return true;
}

bool AudioLibRemoteServer::isSameServer(const QUrl &url) const
{
	return !address_.isEmpty() && url.isValid() && url.scheme() == QLatin1String("https") &&
	       url.host() == address_.host() && url.port(443) == address_.port() &&
	       url.userInfo().isEmpty() && !url.hasFragment();
}

QNetworkReply *AudioLibRemoteServer::get(const QUrl &url, const QString &operation)
{
	QNetworkRequest request(url);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(15000);
	request.setRawHeader("Accept-Encoding", "identity");
	if (!password_.isEmpty())
		request.setRawHeader("Authorization", "Basic " + (QByteArray("xa1:") + password_.toUtf8()).toBase64());
	if (!certificates_.isEmpty()) {
		QSslConfiguration ssl = request.sslConfiguration();
		ssl.addCaCertificates(certificates_);
		request.setSslConfiguration(ssl);
	}
	QNetworkAccessManager *manager = &network_;
#ifdef Q_OS_WIN
	if (!certificates_.isEmpty() && QSslSocket::activeBackend() == QLatin1String("schannel") &&
	    !QHostAddress(url.host()).isNull()) {
		// Qt can retain certificate decisions on pooled connections. Give each
		// request eligible for Schannel revalidation its own connection.
		manager = new QNetworkAccessManager(this);
		manager->setProxy(network_.proxy());
		manager->setAutoDeleteReplies(true);
	}
#endif
	auto *reply = manager->get(request);
	if (manager != &network_) {
		connect(reply, &QNetworkReply::finished, manager, &QObject::deleteLater);
#ifdef Q_OS_WIN
		connect(reply, &QNetworkReply::encrypted, reply, [reply, savedAuthorities = certificates_]() {
			// Check the saved CA and the current peer before every connection
			// can send an HTTP request, including Qt's internal retries.
			if (!reply->property(kTlsError).isValid() &&
			    verifyAudioLibRemoteServerCertificate(reply->url(), reply->sslConfiguration(), savedAuthorities))
				return;
			if (!reply->property(kTlsError).isValid()) {
				reply->setProperty(kTlsError, QStringLiteral("Could not verify the XA1 certificate for the saved CA and IP address."));
				reply->setProperty(kTlsIssues, QStringList{QStringLiteral("  Windows did not verify the peer against the saved XA1 CA and IP.")});
			}
			reply->abort();
		});
#endif
	}
	// Snapshot only shareable connection settings. A reply can finish after
	// the user has edited the connection that produced it.
	QStringList details{
		QStringLiteral("Operation: %1").arg(operation),
		QStringLiteral("Server: %1").arg(serverOrigin(url)),
		QStringLiteral("Password configured: %1").arg(password_.isEmpty() ? QStringLiteral("No")
									      : QStringLiteral("Yes")),
		QStringLiteral("TLS backend: %1").arg(QSslSocket::activeBackend()),
	};
	if (certificates_.isEmpty()) {
		details << QStringLiteral("Configured trust: system certificates");
	} else {
		details << QStringLiteral("Selected certificate file: %1")
				   .arg(certificateName_.isEmpty() ? QStringLiteral("(name unavailable)")
								  : QFileInfo(certificateName_).fileName().simplified());
		for (const QSslCertificate &certificate : certificates_)
			details.append(certificateDetails(QStringLiteral("Configured certificate:"), certificate));
	}
	reply->setProperty(kRequestDetails, details);
	connect(reply, &QNetworkReply::sslErrors, reply,
		[reply, savedAuthorities = certificates_, verifyIp = manager != &network_](const QList<QSslError> &errors) {
		if (errors.isEmpty())
			return;
#ifdef Q_OS_WIN
		if (verifyIp &&
		    verifyAudioLibRemoteServerCertificate(reply->url(), reply->sslConfiguration(), savedAuthorities, errors)) {
			reply->ignoreSslErrors(errors);
			return;
		}
		if (verifyIp)
			reply->ignoreSslErrors(QList<QSslError>{});
#else
		Q_UNUSED(savedAuthorities);
		Q_UNUSED(verifyIp);
#endif
		reply->setProperty(kTlsError, certificateError(errors));
		QStringList issues;
		for (const QSslError &error : errors) {
			QString issue = QStringLiteral("  %1 (%2): %3")
						.arg(enumName(error.error())).arg(int(error.error()))
						.arg(error.errorString().simplified());
			if (!error.certificate().isNull())
				issue += QStringLiteral(" [certificate SHA-256: %1]").arg(certificateFingerprint(error.certificate()));
			issues << issue;
		}
		reply->setProperty(kTlsIssues, issues);
	});
	return reply;
}

QNetworkReply *AudioLibRemoteServer::search(const SearchOptions &options, int offset)
{
	QUrl url = address_;
	url.setPath(QStringLiteral("/api/v1/files/search"));
	QStringList parameters;
	const auto add = [&parameters](const QString &key, const QString &value) {
		// Encode literal '+' as well as spaces for the Go query parser.
		if (!value.isEmpty())
			parameters << key + QLatin1Char('=') + QString::fromLatin1(QUrl::toPercentEncoding(value));
	};
	add(QStringLiteral("q"), options.query);
	add(QStringLiteral("name"), options.name);
	add(QStringLiteral("path"), options.path);
	add(QStringLiteral("ext"), options.extension);
	if (options.minDuration)
		add(QStringLiteral("minDuration"), QString::number(*options.minDuration, 'g', QLocale::FloatingPointShortest));
	if (options.maxDuration)
		add(QStringLiteral("maxDuration"), QString::number(*options.maxDuration, 'g', QLocale::FloatingPointShortest));
	if (options.playableOnly)
		add(QStringLiteral("playable"), QStringLiteral("1"));
	add(QStringLiteral("orderBy"), options.orderBy);
	add(QStringLiteral("order"), options.descending ? QStringLiteral("desc") : QStringLiteral("asc"));
	add(QStringLiteral("limit"), QString::number(PageSize));
	add(QStringLiteral("offset"), QString::number(offset));
	url.setQuery(parameters.join(QLatin1Char('&')), QUrl::StrictMode);
	return get(url, QStringLiteral("Catalog search"));
}

AudioLibRemoteServer::Page AudioLibRemoteServer::readPage(QNetworkReply &reply, Failure &failure) const
{
	failure = {};
	const QString error = requestError(reply);
	if (!error.isEmpty()) {
		failure = describeFailure(reply, error);
		return {};
	}
	const QJsonObject body = QJsonDocument::fromJson(reply.readAll()).object();
	const QJsonValue items = body.value(QStringLiteral("items"));
	const int total = body.value(QStringLiteral("total")).toInt(-1);
	if (!items.isArray() || total < 0) {
		failure = describeFailure(reply, QStringLiteral("The XA1 returned an invalid catalog response."));
		return {};
	}
	Page page;
	page.total = total;
	// The server's web-player filter omits audio formats that Harpia can decode.
	// Keep the server's pagination and filter for the editor's formats here.
	for (const QJsonValue &item : items.toArray()) {
		const QJsonObject row = item.toObject();
		AudioLibRemoteServerSound sound;
		sound.name = row.value(QStringLiteral("name")).toString();
		sound.path = row.value(QStringLiteral("path")).toString();
		const QString downloadUrl = row.value(QStringLiteral("downloadUrl")).toString();
		if (downloadUrl.isEmpty())
			continue;
		sound.downloadUrl = reply.url().resolved(QUrl(downloadUrl));
		if (sound.name.startsWith(QLatin1String("._")) || sound.path.contains(QLatin1String("__MACOSX/")) ||
		    !isAudioFile(sound.name) || !isSameServer(sound.downloadUrl) || sound.path.isEmpty())
			continue;
		sound.sizeBytes = row.value(QStringLiteral("sizeBytes")).toInteger(-1);
		sound.durationSeconds = row.value(QStringLiteral("durationSeconds")).toDouble(-1);
		sound.durationExact = row.value(QStringLiteral("durationExact")).toBool();
		page.sounds.append(std::move(sound));
	}
	return page;
}

QString AudioLibRemoteServer::importsDir()
{
	return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
		.filePath(QStringLiteral("XA1 Audio"));
}

QNetworkReply *AudioLibRemoteServer::download(const AudioLibRemoteServerSound &sound, const QString &folder,
					     QObject *context,
					     std::function<void(const QString &, const Failure &)> finished,
					     QString &error)
{
	error.clear();
	if (!isSameServer(sound.downloadUrl) || !isAudioFile(sound.name)) {
		error = QStringLiteral("This sound is from a different connection. Refresh the XA1 catalog.");
		return nullptr;
	}
	const QDir dir(folder);
	auto file = std::make_shared<QTemporaryFile>(dir.filePath(QStringLiteral(".download-XXXXXX")));
	if (!QDir().mkpath(folder) || !file->open()) {
		error = QStringLiteral("Could not open a local file for the XA1 download.");
		return nullptr;
	}
	auto *reply = get(sound.downloadUrl, QStringLiteral("Sound download"));
	reply->setReadBufferSize(256 * 1024);
	auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
	const auto writeAvailable = [reply, file, hash]() {
		const QByteArray data = reply->readAll();
		if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
			return true;
		if (file->write(data) != data.size())
			return false;
		hash->addData(data);
		return true;
	};
	connect(reply, &QNetworkReply::readyRead, reply, [writeAvailable, reply]() {
		if (!writeAvailable())
			reply->abort();
	});
	connect(reply, &QNetworkReply::finished, context,
		[reply, file, hash, writeAvailable, sound, dir, finished = std::move(finished)]() {
			QString failure = requestError(*reply);
			if (file->error() != QFileDevice::NoError ||
			    (failure.isEmpty() && (!writeAvailable() || !file->flush())))
				failure = QStringLiteral("Could not save the XA1 download to this PC.");
			if (failure.isEmpty() && (file->size() <= 0 ||
			    (sound.sizeBytes >= 0 && file->size() != sound.sizeBytes)))
				failure = QStringLiteral("The XA1 file changed or arrived incomplete. Refresh and try again.");
			if (!failure.isEmpty()) {
				finished({}, describeFailure(*reply, failure));
				return;
			}
			// A new version of a sound gets its own path. Existing projects
			// keep their original bytes, and repeat imports reuse the copy.
			const QString digest = QString::fromLatin1(hash->result().toHex());
			const QString destination = dir.filePath(digest + QLatin1Char('/') + localFileName(sound.name));
			if (dir.mkpath(digest) && file->rename(destination)) {
				file->setAutoRemove(false);
			} else {
				QFile existing(destination);
				QCryptographicHash verify(QCryptographicHash::Sha256);
				if (!existing.open(QIODevice::ReadOnly) || !verify.addData(&existing) ||
				    verify.result() != hash->result()) {
					finished({}, describeFailure(*reply, QStringLiteral("Could not finalize the local copy of this XA1 sound.")));
					return;
				}
			}
			finished(destination, {});
		});
	return reply;
}

} // namespace harpia
