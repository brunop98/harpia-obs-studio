#include "SecretStore.hpp"
#include "../../core/RecorderSettings.hpp"

#include "../../ui/EditorLog.hpp"

#include <QByteArray>
#include <QSettings>
#include <QSysInfo>

#if defined(Q_OS_MACOS) || defined(HARPIA_TEST_MAC_SECRET_STORE)
#include "MacSecretStore.hpp"
#define HARPIA_MAC_KEYCHAIN 1
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif

namespace harpia {

namespace {

QSettings settings()
{
#ifdef HARPIA_TEST_MAC_SECRET_STORE
	// macOS native preferences ignore QSettings::setPath. The mock-backend
	// tests must never read or overwrite the user's real application settings.
	return QSettings(QSettings::IniFormat, QSettings::UserScope,
		QStringLiteral("Harpia"), QStringLiteral("Recorder"));
#else
	return harpia::recorderSettings();
#endif
}

#ifndef Q_OS_WIN
// Not encryption: a per-machine XOR so the key is not readable by eye in the
// settings file. Windows gets the real thing below.
QByteArray scramble(const QByteArray &in)
{
	const QByteArray salt = QSysInfo::machineUniqueId().isEmpty() ? QByteArrayLiteral("harpia-recorder")
								      : QSysInfo::machineUniqueId();
	QByteArray out = in;
	for (int i = 0; i < out.size(); ++i)
		out[i] = char(out[i] ^ salt[i % salt.size()] ^ 0x5a);
	return out;
}
#endif
} // namespace

QString SecretStore::settingsName(SpeechProvider p)
{
	if (p == SpeechProvider::OpenAI)
		return QStringLiteral("subtitles/apiKey");
	return QStringLiteral("subtitles/apiKey_%1").arg(QString::fromLatin1(speechProviderInfo(p).id));
}

bool SecretStore::saveApiKey(SpeechProvider p, const QString &key)
{
	return saveNamedKey(settingsName(p), speechProviderName(p), key);
}

QString SecretStore::loadApiKey(SpeechProvider p)
{
	return loadNamedKey(settingsName(p), speechProviderName(p));
}

bool SecretStore::saveNamedKey(const QString &settingsName, const QString &display, const QString &key)
{
	const QString kKey = settingsName;
	const QString kKind = kKey + QStringLiteral("Kind"); // "dpapi" | "scrambled"
	const QString &name = display;
#ifdef HARPIA_MAC_KEYCHAIN
	QSettings s = settings();
	QString why;
	const QString trimmed = key.trimmed();
	if (trimmed.isEmpty()) {
		if (!mac_secret_store::remove(kKey, &why)) {
			editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
				QStringLiteral("%1 API key could not be removed: %2").arg(name, why));
			return false;
		}
		s.remove(kKey);
		s.remove(kKind);
	} else {
		if (!mac_secret_store::write(kKey, trimmed.toUtf8(), &why)) {
			editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
				QStringLiteral("%1 API key could not be saved: %2").arg(name, why));
			return false; // never fall back to a newly scrambled key on macOS
		}
		const auto back = mac_secret_store::read(kKey);
		if (back.status != mac_secret_store::ReadStatus::Found || back.secret != trimmed.toUtf8()) {
			editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
				QStringLiteral("%1 API key could not be verified in macOS Keychain. %2")
					.arg(name, back.error));
			return false; // preserve an existing legacy key until verification succeeds
		}
		s.remove(kKey);
		s.setValue(kKind, QStringLiteral("keychain"));
	}
	s.sync();
	if (s.status() != QSettings::NoError) {
		editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
			QStringLiteral("%1 API key: the settings could not be updated (%2)").arg(name, s.fileName()));
		return false;
	}
	editorLog(EditorLogLevel::Info, QStringLiteral("Subtitles"), trimmed.isEmpty()
		? QStringLiteral("%1 API key removed").arg(name)
		: QStringLiteral("%1 API key saved in macOS Keychain (%2)").arg(name, maskedKey(trimmed)));
	return true;
#else
	bool stored = false;
	{
		QSettings s = settings();
		if (key.trimmed().isEmpty()) {
			s.remove(kKey);
			s.remove(kKind);
			editorLog(EditorLogLevel::Info, QStringLiteral("Subtitles"),
				  QStringLiteral("%1 API key removed").arg(name));
			return true;
		}
		stored = storeSealed(s, kKey, kKind, key.trimmed().toUtf8());
		s.sync(); // written now, not whenever the settings object gets round to it
		if (s.status() != QSettings::NoError) {
			editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
				  QStringLiteral("%1 API key: the settings could not be written (%2)")
					  .arg(name)
					  .arg(s.fileName()));
			return false;
		}
	}
	// Read it back the way the next launch will: a key that cannot be
	// recovered was not saved, whatever the write said.
	const bool readable = stored && loadNamedKey(settingsName, display) == key.trimmed();
	if (readable)
		editorLog(EditorLogLevel::Info, QStringLiteral("Subtitles"),
			  QStringLiteral("%1 API key saved (%2)").arg(name, maskedKey(key)));
	else
		editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
			  QStringLiteral("%1 API key could not be saved: %2")
				  .arg(name, stored ? QStringLiteral("it did not read back the same")
						    : QStringLiteral("Windows refused to protect it (DPAPI)")));
	return readable;
#endif
}

bool SecretStore::storeSealed(QSettings &s, const QString &kKey, const QString &kKind, const QByteArray &plain)
{
#ifdef Q_OS_WIN
	DATA_BLOB in;
	in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()));
	in.cbData = DWORD(plain.size());
	DATA_BLOB out{};
	if (CryptProtectData(&in, L"Harpia transcription key", nullptr, nullptr, nullptr,
			     CRYPTPROTECT_UI_FORBIDDEN, &out)) {
		// Plain base64 text, not a QByteArray: the registry keeps a string as a
		// string, where a byte array goes through Qt's @ByteArray() wrapping.
		s.setValue(kKey, QString::fromLatin1(
					 QByteArray(reinterpret_cast<const char *>(out.pbData), int(out.cbData)).toBase64()));
		s.setValue(kKind, QStringLiteral("dpapi"));
		LocalFree(out.pbData);
		return true;
	}
	// DPAPI refused (rare): keep nothing rather than keep it in the clear.
	s.remove(kKey);
	s.remove(kKind);
	return false;
#else
	s.setValue(kKey, QString::fromLatin1(scramble(plain).toBase64()));
	s.setValue(kKind, QStringLiteral("scrambled"));
	return true;
#endif
}

QString SecretStore::loadNamedKey(const QString &settingsName, const QString &display)
{
	const QString kKey = settingsName;
	const QString kKind = kKey + QStringLiteral("Kind");
	QSettings s = settings();
#ifdef HARPIA_MAC_KEYCHAIN
	const auto result = mac_secret_store::read(kKey);
	if (result.status == mac_secret_store::ReadStatus::Found) {
		const QString key = QString::fromUtf8(result.secret);
		if (key.isEmpty() || key.toUtf8() != result.secret) {
			editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
				QStringLiteral("The saved %1 API key in macOS Keychain is invalid. Enter it again.").arg(display));
			return QString();
		}
		// Recover cleanup after an earlier write succeeded but settings sync or
		// verification was interrupted. The native credential is authoritative.
		if (s.contains(kKey) || s.value(kKind).toString() != QLatin1String("keychain")) {
			s.remove(kKey);
			s.setValue(kKind, QStringLiteral("keychain"));
			s.sync();
			if (s.status() != QSettings::NoError)
				editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
					QStringLiteral("The %1 API key is in macOS Keychain, but legacy settings cleanup "
						"could not finish (%2).").arg(display, s.fileName()));
		}
		return key;
	}
	if (result.status == mac_secret_store::ReadStatus::Failed) {
		editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
			QStringLiteral("The saved %1 API key could not be read: %2").arg(display, result.error));
		return QString();
	}
	// Older Mac releases kept reversible settings. Only remove those bytes
	// after the Keychain has saved and read back the exact same value.
	const QString kind = s.value(kKind).toString();
	if (kind != QLatin1String("scrambled"))
		return QString();
	const QByteArray stored = QByteArray::fromBase64(s.value(kKey).toByteArray());
	const QByteArray plain = scramble(stored);
	const QString legacy = QString::fromUtf8(plain);
	if (legacy.isEmpty() || legacy.toUtf8() != plain) {
		editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
			QStringLiteral("The legacy %1 API key could not be read on this Mac. Enter it again.").arg(display));
		return QString();
	}
	if (!saveNamedKey(settingsName, display, legacy))
		editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
			QStringLiteral("The existing %1 API key is still in legacy settings because Keychain migration "
				"could not finish. Unlock the login keychain and try again.").arg(display));
	return legacy;
#else
	const QByteArray stored = QByteArray::fromBase64(s.value(kKey).toByteArray());
	if (stored.isEmpty())
		return QString();
	const QString kind = s.value(kKind).toString();
#ifdef Q_OS_WIN
	if (kind == QLatin1String("dpapi")) {
		DATA_BLOB in;
		in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(stored.constData()));
		in.cbData = DWORD(stored.size());
		DATA_BLOB out{};
		if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
			editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
				  QStringLiteral("the saved %1 API key could not be unsealed (Windows error %2); "
						 "it was saved by another Windows user or machine. Save it again.")
					  .arg(display)
					  .arg(qulonglong(GetLastError())));
			return QString();
		}
		const QString key = QString::fromUtf8(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
		LocalFree(out.pbData);
		return key;
	}
	return QString(); // a scrambled key from another OS is not ours to read
#else
	if (kind == QLatin1String("scrambled"))
		return QString::fromUtf8(scramble(stored));
	return QString();
#endif
#endif
}

QString SecretStore::maskedKey(const QString &key)
{
	const QString k = key.trimmed();
	if (k.size() <= 8)
		return k.isEmpty() ? QString() : QStringLiteral("••••");
	return k.left(3) + QStringLiteral("…") + k.right(4);
}

} // namespace harpia
