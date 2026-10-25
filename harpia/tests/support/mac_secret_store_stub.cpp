// Test editor archives never access or modify the user's login Keychain.
#include "editor/subtitles/MacSecretStore.hpp"
#include <QHash>
#include <mutex>

namespace harpia::mac_secret_store {
namespace {
QHash<QString, QByteArray> entries;
std::mutex lock;
}
ReadResult read(const QString &account)
{
	std::lock_guard<std::mutex> guard(lock);
	const auto value = entries.constFind(account);
	return value == entries.cend() ? ReadResult{} : ReadResult{ReadStatus::Found, value.value(), {}};
}
bool write(const QString &account, const QByteArray &secret, QString *)
{
	std::lock_guard<std::mutex> guard(lock);
	entries.insert(account, secret);
	return true;
}
bool remove(const QString &account, QString *)
{
	std::lock_guard<std::mutex> guard(lock);
	entries.remove(account);
	return true;
}
} // namespace harpia::mac_secret_store
