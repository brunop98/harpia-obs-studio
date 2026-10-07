#include <windows.h>
#include <obs-module.h>
#include <util/windows/win-version.h>
#include <util/platform.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-capture", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Windows screen/window capture";
}

extern struct obs_source_info duplicator_capture_info;
extern struct obs_source_info monitor_capture_info;
extern struct obs_source_info window_capture_info;

bool graphics_uses_d3d11 = false;
bool wgc_supported = false;

bool obs_module_load(void)
{
	struct win_version_info ver;
	bool win8_or_above = false;

	struct win_version_info win1903 = {.major = 10, .minor = 0, .build = 18362, .revis = 0};

	get_win_ver(&ver);

	win8_or_above = ver.major > 6 || (ver.major == 6 && ver.minor >= 2);

	obs_enter_graphics();
	graphics_uses_d3d11 = gs_get_device_type() == GS_DEVICE_DIRECT3D_11;
	obs_leave_graphics();

	if (graphics_uses_d3d11)
		wgc_supported = win_version_compare(&ver, &win1903) >= 0;

	if (win8_or_above && graphics_uses_d3d11)
		obs_register_source(&duplicator_capture_info);
	else
		obs_register_source(&monitor_capture_info);

	obs_register_source(&window_capture_info);

	return true;
}

// No obs_module_unload: upstream's only work there was the compatibility-list
// updater, which fetched a list of games from obsproject.com on every start
// to show warnings in OBS's own properties dialog. Harpia has no such dialog.
