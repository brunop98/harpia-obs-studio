#include "MacSecretStore.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace harpia::mac_secret_store {
namespace {

template<typename T> class CfOwner {
public:
	explicit CfOwner(T value) : value_(value) {}
	~CfOwner() { if (value_) CFRelease(value_); }
	CfOwner(const CfOwner &) = delete;
	CfOwner &operator=(const CfOwner &) = delete;
	T get() const { return value_; }
private:
	T value_;
};

QString statusMessage(OSStatus status)
{
	CfOwner<CFStringRef> message(SecCopyErrorMessageString(status, nullptr));
	QString detail;
	if (message.get()) {
		const CFIndex count = CFStringGetLength(message.get());
		detail.resize(int(count));
		CFStringGetCharacters(message.get(), CFRangeMake(0, count),
			reinterpret_cast<UniChar *>(detail.data()));
	}
	return QStringLiteral("macOS Keychain error %1%2. Unlock the login keychain and allow Harpia access, "
		"then try again.").arg(qint64(status)).arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
}

CFMutableDictionaryRef query(const QString &account)
{
	const QByteArray utf8 = account.toUtf8();
	CfOwner<CFStringRef> accountString(CFStringCreateWithBytes(kCFAllocatorDefault,
		reinterpret_cast<const UInt8 *>(utf8.constData()), utf8.size(), kCFStringEncodingUTF8, false));
	if (!accountString.get())
		return nullptr;
	CFMutableDictionaryRef out = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (!out)
		return nullptr;
	CFDictionarySetValue(out, kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(out, kSecAttrService, CFSTR("com.harpia.recorder.api-keys"));
	CFDictionarySetValue(out, kSecAttrAccount, accountString.get());
	return out;
}

bool allocationError(QString *error)
{
	if (error)
		*error = QStringLiteral("Could not prepare the macOS Keychain request. Try again.");
	return false;
}
} // namespace

ReadResult read(const QString &account)
{
	CfOwner<CFMutableDictionaryRef> request(query(account));
	if (!request.get())
		return {ReadStatus::Failed, {}, QStringLiteral("Could not prepare the macOS Keychain request.")};
	CFDictionarySetValue(request.get(), kSecReturnData, kCFBooleanTrue);
	CFDictionarySetValue(request.get(), kSecMatchLimit, kSecMatchLimitOne);
	CFTypeRef raw = nullptr;
	const OSStatus status = SecItemCopyMatching(request.get(), &raw);
	CfOwner<CFTypeRef> result(raw);
	if (status == errSecItemNotFound)
		return {};
	if (status != errSecSuccess)
		return {ReadStatus::Failed, {}, statusMessage(status)};
	if (!result.get() || CFGetTypeID(result.get()) != CFDataGetTypeID())
		return {ReadStatus::Failed, {}, QStringLiteral("The macOS Keychain returned an invalid credential. Save the API key again.")};
	const CFDataRef bytes = static_cast<CFDataRef>(result.get());
	return {ReadStatus::Found,
		QByteArray(reinterpret_cast<const char *>(CFDataGetBytePtr(bytes)), CFDataGetLength(bytes)), {}};
}

bool write(const QString &account, const QByteArray &secret, QString *error)
{
	if (error)
		error->clear();
	CfOwner<CFMutableDictionaryRef> request(query(account));
	CfOwner<CFDataRef> value(CFDataCreate(kCFAllocatorDefault,
		reinterpret_cast<const UInt8 *>(secret.constData()), secret.size()));
	CfOwner<CFMutableDictionaryRef> attributes(CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
	if (!request.get() || !value.get() || !attributes.get())
		return allocationError(error);
	CFDictionarySetValue(attributes.get(), kSecValueData, value.get());
	OSStatus status = SecItemUpdate(request.get(), attributes.get());
	if (status == errSecItemNotFound) {
		CFDictionarySetValue(request.get(), kSecValueData, value.get());
		CFDictionarySetValue(request.get(), kSecAttrLabel, CFSTR("Harpia Recorder API key"));
		status = SecItemAdd(request.get(), nullptr);
		// Another Harpia request may have inserted this account meanwhile.
		if (status == errSecDuplicateItem) {
			CFDictionaryRemoveValue(request.get(), kSecValueData);
			CFDictionaryRemoveValue(request.get(), kSecAttrLabel);
			status = SecItemUpdate(request.get(), attributes.get());
		}
	}
	if (status == errSecSuccess)
		return true;
	if (error)
		*error = statusMessage(status);
	return false;
}

bool remove(const QString &account, QString *error)
{
	if (error)
		error->clear();
	CfOwner<CFMutableDictionaryRef> request(query(account));
	if (!request.get())
		return allocationError(error);
	const OSStatus status = SecItemDelete(request.get());
	if (status == errSecSuccess || status == errSecItemNotFound)
		return true;
	if (error)
		*error = statusMessage(status);
	return false;
}

} // namespace harpia::mac_secret_store
