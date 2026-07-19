#pragma once

namespace harpia {

// Whether desktop applications are allowed to use the camera, as far as the OS
// privacy settings can tell us. Used to explain an empty camera list.
enum class CameraAccess {
	Allowed,        // privacy settings permit camera access
	DeniedByPrivacy, // OS privacy blocks desktop apps from the camera
	Unknown,        // couldn't determine (non-Windows, or read failed)
};

// Query the OS camera-privacy state. Windows reads the CapabilityAccessManager
// consent store; other platforms return Unknown.
CameraAccess cameraAccessStatus();

} // namespace harpia
