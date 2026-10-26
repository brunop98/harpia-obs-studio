#include "PhoneSharingNative.hpp"

#if !defined(Q_OS_MACOS)
namespace harpia::phone_sharing::native {

bool airDropSupported()
{
	return false;
}

bool sendWithAirDrop(const QStringList &, QObject *, std::function<void(const QString &)>, QString *error)
{
	if (error)
		*error = QStringLiteral("AirDrop is available on macOS. Use LocalSend on this computer.");
	return false;
}

} // namespace harpia::phone_sharing::native
#endif
