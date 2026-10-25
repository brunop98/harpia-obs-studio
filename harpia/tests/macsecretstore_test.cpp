// Exercise the real Mac SecretStore branch without touching a login keychain.
// Compile SecretStore.cpp with HARPIA_TEST_MAC_SECRET_STORE and this backend.
#include "editor/subtitles/SecretStore.hpp"
#include "editor/subtitles/MacSecretStore.hpp"
#include "ui/EditorLog.hpp"

#include <QCoreApplication>
#include <QHash>
#include <QSettings>
#include <QStringList>
#include <QSysInfo>
#include <QTemporaryDir>

#include <cstdio>

namespace {
QHash<QString, QByteArray> credentials;
bool denyRead = false;
bool denyWrite = false;
bool denyRemove = false;
bool denyVerification = false;
bool nextReadFails = false;
int writes = 0;
QStringList logs;
int failures = 0;

void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}
QSettings settings()
{
	return QSettings(QSettings::IniFormat, QSettings::UserScope,
		QStringLiteral("Harpia"), QStringLiteral("Recorder"));
}
void reset()
{
	credentials.clear();
	denyRead = denyWrite = denyRemove = denyVerification = nextReadFails = false;
	writes = 0;
	logs.clear();
	QSettings s = settings();
	s.clear();
	s.sync();
}
QByteArray legacyBytes(const QByteArray &key)
{
	const QByteArray salt = QSysInfo::machineUniqueId().isEmpty() ? QByteArrayLiteral("harpia-recorder")
		: QSysInfo::machineUniqueId();
	QByteArray data = key;
	for (int i = 0; i < data.size(); ++i)
		data[i] = char(data[i] ^ salt[i % salt.size()] ^ 0x5a);
	return data.toBase64();
}
void putLegacy(const QString &account, const QByteArray &key)
{
	QSettings s = settings();
	s.setValue(account, QString::fromLatin1(legacyBytes(key)));
	s.setValue(account + QStringLiteral("Kind"), QStringLiteral("scrambled"));
	s.sync();
}
}

namespace harpia::mac_secret_store {
ReadResult read(const QString &account)
{
	if (denyRead || nextReadFails) {
		nextReadFails = false;
		return {ReadStatus::Failed, {}, QStringLiteral("Test keychain is locked")};
	}
	const auto it = credentials.constFind(account);
	return it == credentials.cend() ? ReadResult{} : ReadResult{ReadStatus::Found, it.value(), {}};
}
bool write(const QString &account, const QByteArray &key, QString *error)
{
	++writes;
	if (denyWrite) {
		if (error)
			*error = QStringLiteral("Test keychain is locked");
		return false;
	}
	credentials.insert(account, key);
	nextReadFails = denyVerification;
	return true;
}
bool remove(const QString &account, QString *error)
{
	if (denyRemove) {
		if (error)
			*error = QStringLiteral("Test keychain is locked");
		return false;
	}
	credentials.remove(account);
	return true;
}
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	QTemporaryDir temporary;
	if (!temporary.isValid())
		return 2;
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
	harpia::editorLogSink() = [](harpia::EditorLogLevel, const QString &line) { logs << line; };
	using harpia::SecretStore;
	const QString account = QStringLiteral("subtitles/apiKey");
	const QString marker = account + QStringLiteral("Kind");
	const QString secret = QStringLiteral("token-for-primary-api-123456789");

	reset();
	check(SecretStore::loadNamedKey(account, QStringLiteral("Primary")).isEmpty(),
		"an absent Keychain entry is a missing credential");
	check(SecretStore::saveNamedKey(account, QStringLiteral("Primary"), QStringLiteral("  ") + secret + QStringLiteral("  ")),
		"saving verifies the trimmed secret in the native store");
	check(credentials.value(account) == secret.toUtf8(), "the secret belongs to its stable account");
	check(!settings().contains(account) && settings().value(marker).toString() == QStringLiteral("keychain"),
		"settings contain a Keychain marker without secret bytes");
	check(SecretStore::loadNamedKey(account, QStringLiteral("Primary")) == secret,
		"a saved native secret can be read on the next load");
	check(!logs.join(QLatin1Char('\n')).contains(secret), "neither successful saves nor reads log the full secret");
	check(SecretStore::saveNamedKey(QStringLiteral("tts/googleKey"), QStringLiteral("Google"), QStringLiteral("google-secret")) &&
		SecretStore::loadNamedKey(account, QStringLiteral("Primary")) == secret,
		"provider accounts never overwrite one another");
	check(SecretStore::saveNamedKey(account, QStringLiteral("Primary"), QString()) &&
		!credentials.contains(account) && !settings().contains(marker) && !settings().contains(account),
		"clearing a key removes both the native credential and settings metadata");
	check(credentials.contains(QStringLiteral("tts/googleKey")), "removing one provider leaves another intact");
	check(SecretStore::saveNamedKey(account, QStringLiteral("Primary"), QString()),
		"removing an already absent credential succeeds");

	reset();
	denyWrite = true;
	check(!SecretStore::saveNamedKey(account, QStringLiteral("Primary"), secret),
		"denied native storage reports failure");
	check(!settings().contains(account) && !credentials.contains(account),
		"a failed new save does not fall back to scrambled settings");
	check(logs.join(QLatin1Char('\n')).contains(QStringLiteral("locked")),
		"native storage errors reach the editor error log");

	reset();
	putLegacy(account, secret.toUtf8());
	const QVariant legacy = settings().value(account);
	check(SecretStore::loadNamedKey(account, QStringLiteral("Primary")) == secret && writes == 1,
		"a legacy key migrates to Keychain when first loaded");
	check(credentials.value(account) == secret.toUtf8() && !settings().contains(account) &&
		settings().value(marker).toString() == QStringLiteral("keychain"),
		"verified migration removes all reversible legacy secret bytes");
	check(SecretStore::loadNamedKey(account, QStringLiteral("Primary")) == secret && writes == 1,
		"subsequent loads use Keychain without repeating migration");

	reset();
	putLegacy(account, secret.toUtf8());
	denyWrite = true;
	check(SecretStore::loadNamedKey(account, QStringLiteral("Primary")) == secret,
		"a failed migration preserves use of the existing legacy key");
	check(settings().value(account) == legacy && settings().value(marker).toString() == QStringLiteral("scrambled"),
		"denied migration does not erase or relabel the legacy record");
	denyWrite = false;
	check(SecretStore::loadNamedKey(account, QStringLiteral("Primary")) == secret && !settings().contains(account),
		"migration retries successfully after native storage becomes available");

	reset();
	putLegacy(account, secret.toUtf8());
	denyVerification = true;
	check(!SecretStore::saveNamedKey(account, QStringLiteral("Primary"), secret) && settings().value(account) == legacy,
		"an unreadable verification result preserves the legacy backup");
	denyVerification = false;
	check(SecretStore::loadNamedKey(account, QStringLiteral("Primary")) == secret && !settings().contains(account),
		"a later successful native read finishes interrupted legacy cleanup");

	reset();
	putLegacy(account, secret.toUtf8());
	denyRead = true;
	check(SecretStore::loadNamedKey(account, QStringLiteral("Primary")).isEmpty() && writes == 0 && settings().value(account) == legacy,
		"a locked Keychain read neither overwrites credentials nor erases legacy data");
	denyRead = false;
	denyRemove = true;
	credentials.insert(account, secret.toUtf8());
	check(!SecretStore::saveNamedKey(account, QStringLiteral("Primary"), QString()) &&
		credentials.contains(account) && settings().value(account) == legacy,
		"a denied deletion leaves both native and legacy credentials untouched");
	check(!logs.join(QLatin1Char('\n')).contains(secret), "failure diagnostics also omit the complete secret");

	reset();
	putLegacy(account, QByteArray(1, char(0xff)));
	check(SecretStore::loadNamedKey(account, QStringLiteral("Primary")).isEmpty() && writes == 0 && settings().contains(account),
		"an invalid legacy encoding is preserved and requests re-entry rather than being migrated");
	credentials.insert(account, QByteArray(1, char(0xff)));
	check(SecretStore::loadNamedKey(account, QStringLiteral("Primary")).isEmpty(),
		"an invalid native credential is rejected instead of returning a corrupted key");

	harpia::editorLogSink() = nullptr;
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
