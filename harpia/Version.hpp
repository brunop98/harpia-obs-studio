#pragma once

// Single source of truth for the Harpia recorder's application version.
// Semantic versioning: Major.Minor.Patch. Bump on every meaningful update.
// Referenced by the window title, startup log, Error Logs headers, and any
// diagnostic export.
// PATCH is bumped on every commit (see the version label in the window corner).
#define HARPIA_VERSION_MAJOR 0
#define HARPIA_VERSION_MINOR 1
#define HARPIA_VERSION_PATCH 221

#define HARPIA_STRINGIFY_(x) #x
#define HARPIA_STRINGIFY(x) HARPIA_STRINGIFY_(x)

#define HARPIA_VERSION_STRING                                                  \
	HARPIA_STRINGIFY(HARPIA_VERSION_MAJOR)                                  \
	"." HARPIA_STRINGIFY(HARPIA_VERSION_MINOR) "." HARPIA_STRINGIFY(HARPIA_VERSION_PATCH)

namespace harpia {
inline const char *appVersion()
{
	return HARPIA_VERSION_STRING;
}
} // namespace harpia
