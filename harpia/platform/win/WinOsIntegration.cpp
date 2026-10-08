// Windows side of platform/OsIntegration.hpp: the taskbar identity, opting out
// of power throttling, and the per-app graphics preference.
#include "platform/OsIntegration.hpp"

#include <windows.h>
#include <shobjidl.h> // SetCurrentProcessExplicitAppUserModelID

#include <string>

// Windows 11 SDK (10.0.22000) and newer; the value is fixed, so an older SDK
// still builds and the call itself says whether this Windows knows it.
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace harpia::os_integration {

namespace {

// Never throttle the controlled behaviours: the bits in ControlMask are the
// ones being decided, and StateMask = 0 turns each of them off.
bool neverThrottle(ULONG controls)
{
	PROCESS_POWER_THROTTLING_STATE state{};
	state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
	state.ControlMask = controls;
	state.StateMask = 0;
	return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)) != FALSE;
}

constexpr const wchar_t *kGpuKey = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";

// The registry value name is this executable's full path, exactly as Windows
// Settings writes it.
std::wstring exePath()
{
	std::wstring buf(32768, L'\0');
	const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
	buf.resize(n);
	return buf;
}

QString readGpuValue()
{
	const std::wstring name = exePath();
	DWORD bytes = 0;
	if (RegGetValueW(HKEY_CURRENT_USER, kGpuKey, name.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &bytes) !=
		    ERROR_SUCCESS ||
	    bytes == 0)
		return {};
	std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
	if (RegGetValueW(HKEY_CURRENT_USER, kGpuKey, name.c_str(), RRF_RT_REG_SZ, nullptr, buf.data(), &bytes) !=
	    ERROR_SUCCESS)
		return {};
	return QString::fromWCharArray(buf.c_str());
}

} // namespace

QString applyAtStartup()
{
	QStringList done;
	const std::wstring id = QString::fromLatin1(kAppUserModelId).toStdWString();
	if (SUCCEEDED(SetCurrentProcessExplicitAppUserModelID(id.c_str())))
		done << QStringLiteral("taskbar id %1").arg(QLatin1String(kAppUserModelId));
	// Timer resolution is Windows 11's; an older Windows rejects the whole
	// call over the bit it does not know, so try again without it.
	if (neverThrottle(PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
			  PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION))
		done << QStringLiteral("power throttling off (speed and timers)");
	else if (neverThrottle(PROCESS_POWER_THROTTLING_EXECUTION_SPEED))
		done << QStringLiteral("power throttling off (speed)");
	else
		done << QStringLiteral("power throttling: could not opt out (error %1)").arg(GetLastError());
	done << QStringLiteral("graphics: %1").arg(QLatin1String(gpuPreferenceName(gpuPreference())));
	return QStringLiteral("Windows: ") + done.join(QStringLiteral(", "));
}

bool gpuPreferenceSupported()
{
	return true;
}

GpuPreference gpuPreference()
{
	return gpuPreferenceIn(readGpuValue());
}

bool setGpuPreference(GpuPreference p)
{
	const std::wstring name = exePath();
	const QString next = withGpuPreference(readGpuValue(), p);
	if (next.isEmpty()) {
		const LSTATUS st = RegDeleteKeyValueW(HKEY_CURRENT_USER, kGpuKey, name.c_str());
		return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
	}
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kGpuKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
	    ERROR_SUCCESS)
		return false;
	const std::wstring data = next.toStdWString();
	const LSTATUS st = RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE *>(data.c_str()),
					  DWORD((data.size() + 1) * sizeof(wchar_t)));
	RegCloseKey(key);
	return st == ERROR_SUCCESS;
}

} // namespace harpia::os_integration
