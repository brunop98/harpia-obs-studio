#include "SecretStore.hpp"

#include "../../ui/EditorLog.hpp"

#include <QByteArray>
#include <QSettings>
#include <QSysInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif

namespace harpia {

namespace {

QSettings settings()
{
	return QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
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
	const QString kKey = settingsName(p);
	const QString kKind = kKey + QStringLiteral("Kind"); // "dpapi" | "scrambled"
	const QString name = speechProviderName(p);
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
	const bool readable = stored && loadApiKey(p) == key.trimmed();
	if (readable)
		editorLog(EditorLogLevel::Info, QStringLiteral("Subtitles"),
			  QStringLiteral("%1 API key saved (%2)").arg(name, maskedKey(key)));
	else
		editorLog(EditorLogLevel::Error, QStringLiteral("Subtitles"),
			  QStringLiteral("%1 API key could not be saved: %2")
				  .arg(name, stored ? QStringLiteral("it did not read back the same")
						    : QStringLiteral("Windows refused to protect it (DPAPI)")));
	return readable;
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

QString SecretStore::loadApiKey(SpeechProvider p)
{
	const QString kKey = settingsName(p);
	const QString kKind = kKey + QStringLiteral("Kind");
	QSettings s = settings();
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
					  .arg(speechProviderName(p))
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
}

QString SecretStore::maskedKey(const QString &key)
{
	const QString k = key.trimmed();
	if (k.size() <= 8)
		return k.isEmpty() ? QString() : QStringLiteral("••••");
	return k.left(3) + QStringLiteral("…") + k.right(4);
}

} // namespace harpia
