#include "platform/CameraAccess.hpp"

namespace harpia {

// macOS camera authorization could be queried via AVCaptureDevice; not wired
// here yet, so report Unknown.
CameraAccess cameraAccessStatus()
{
	return CameraAccess::Unknown;
}

} // namespace harpia
