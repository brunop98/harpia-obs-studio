#include "platform/CameraAccess.hpp"

#include <windows.h>

namespace harpia {

namespace {

// Read a REG_SZ value; returns true and fills `out` on success.
bool readString(HKEY root, const wchar_t *subkey, const wchar_t *value, wchar_t *out, DWORD outBytes)
{
	HKEY key = nullptr;
	if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
		return false;
	DWORD type = 0;
	DWORD size = outBytes;
	const LONG r = RegQueryValueExW(key, value, nullptr, &type, (LPBYTE)out, &size);
	RegCloseKey(key);
	return r == ERROR_SUCCESS && type == REG_SZ;
}

// A consent-store "Value" of "Deny" means access is blocked at that scope.
bool isDeny(HKEY root, const wchar_t *subkey)
{
	wchar_t buf[16] = {};
	if (!readString(root, subkey, L"Value", buf, sizeof(buf)))
		return false;
	return _wcsicmp(buf, L"Deny") == 0;
}

} // namespace

CameraAccess cameraAccessStatus()
{
	// The overall "camera access" toggle and the "let desktop apps access your
	// camera" toggle live in the CapabilityAccessManager consent store. If either
	// the master or the desktop-apps (NonPackaged) scope is set to Deny, desktop
	// apps like ours can't see camera frames.
	const wchar_t *kMaster =
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam";
	const wchar_t *kDesktop =
		L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam"
		L"\\NonPackaged";

	if (isDeny(HKEY_CURRENT_USER, kMaster) || isDeny(HKEY_CURRENT_USER, kDesktop) ||
	    isDeny(HKEY_LOCAL_MACHINE, kMaster))
		return CameraAccess::DeniedByPrivacy;

	// If we could read a master value and it wasn't Deny, treat as allowed;
	// otherwise we simply don't know.
	wchar_t buf[16] = {};
	if (readString(HKEY_CURRENT_USER, kMaster, L"Value", buf, sizeof(buf)))
		return CameraAccess::Allowed;
	return CameraAccess::Unknown;
}

} // namespace harpia
