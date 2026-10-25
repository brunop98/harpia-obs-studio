#include "platform/CameraAccess.hpp"
#include "MacPlatform.hpp"

namespace harpia {

CameraAccess cameraAccessStatus()
{
	switch (mac_platform::cameraStatus()) {
	case mac_platform::PermissionStatus::Granted: return CameraAccess::Allowed;
	case mac_platform::PermissionStatus::Denied:
	case mac_platform::PermissionStatus::Restricted: return CameraAccess::DeniedByPrivacy;
	case mac_platform::PermissionStatus::NotDetermined: return CameraAccess::Unknown;
	}
	return CameraAccess::Unknown;
}

} // namespace harpia
