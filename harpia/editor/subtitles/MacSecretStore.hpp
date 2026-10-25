#pragma once

#include <QByteArray>
#include <QString>

namespace harpia::mac_secret_store {

enum class ReadStatus { Found, NotFound, Failed };
struct ReadResult {
	ReadStatus status = ReadStatus::NotFound;
	QByteArray secret;
	QString error;
};

// The account is the stable SecretStore settings name. The service name is
// fixed across app versions, so updates keep using the same Keychain entries.
ReadResult read(const QString &account);
bool write(const QString &account, const QByteArray &secret, QString *error);
bool remove(const QString &account, QString *error);

} // namespace harpia::mac_secret_store
