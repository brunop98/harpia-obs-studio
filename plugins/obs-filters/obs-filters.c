#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-filters", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "OBS core filters";
}

// Harpia uses one filter: the crop that turns a monitor capture into the
// chosen region. The rest of OBS's filters are not built in this tree.
extern struct obs_source_info crop_filter;

bool obs_module_load(void)
{
	obs_register_source(&crop_filter);
	return true;
}
