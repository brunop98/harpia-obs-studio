#include "SecretStore.hpp"

#include <QByteArray>
#include <QSettings>
#include <QSysInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif

namespace harpia {

namespace {
const QString kKey = QStringLiteral("subtitles/apiKey");
const QString kKind = QStringLiteral("subtitles/apiKeyKind"); // "dpapi" | "scrambled"

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

void SecretStore::saveApiKey(const QString &key)
{
	QSettings s = settings();
	if (key.trimmed().isEmpty()) {
		s.remove(kKey);
		s.remove(kKind);
		return;
	}
	const QByteArray plain = key.trimmed().toUtf8();
#ifdef Q_OS_WIN
	DATA_BLOB in;
	in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()));
	in.cbData = DWORD(plain.size());
	DATA_BLOB out{};
	if (CryptProtectData(&in, L"Harpia transcription key", nullptr, nullptr, nullptr,
			     CRYPTPROTECT_UI_FORBIDDEN, &out)) {
		s.setValue(kKey, QByteArray(reinterpret_cast<const char *>(out.pbData), int(out.cbData)).toBase64());
		s.setValue(kKind, QStringLiteral("dpapi"));
		LocalFree(out.pbData);
		return;
	}
	// DPAPI refused (rare): keep nothing rather than keep it in the clear.
	s.remove(kKey);
	s.remove(kKind);
#else
	s.setValue(kKey, scramble(plain).toBase64());
	s.setValue(kKind, QStringLiteral("scrambled"));
#endif
}

QString SecretStore::loadApiKey()
{
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
		if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
			return QString();
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
