#include "platform/CameraAccess.hpp"

namespace harpia {

// Linux has no equivalent global camera-privacy switch to query here.
CameraAccess cameraAccessStatus()
{
	return CameraAccess::Unknown;
}

} // namespace harpia
