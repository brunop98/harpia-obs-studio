#include <obs-module.h>
#include <strsafe.h>
#include <strmif.h>

// The virtual camera is not part of this build: Harpia uses win-dshow for
// webcam CAPTURE only. VIRTUALCAM_AVAILABLE was never defined, so what stood
// here was three blocks of unreachable code and an include of a header that no
// longer exists.

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-dshow", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Windows DirectShow source/encoder";
}

extern void RegisterDShowSource();
extern void RegisterDShowEncoders();

bool obs_module_load(void)
{
	RegisterDShowSource();
	RegisterDShowEncoders();
	return true;
}
