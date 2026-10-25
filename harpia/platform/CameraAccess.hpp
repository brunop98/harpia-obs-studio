#pragma once

namespace harpia {

// Whether desktop applications are allowed to use the camera, as far as the OS
// privacy settings can tell us. Used to explain an empty camera list.
enum class CameraAccess {
	Allowed,        // privacy settings permit camera access
	DeniedByPrivacy, // OS privacy blocks desktop apps from the camera
	Unknown,        // permission not requested yet, unsupported or query failed
};

// Query the OS camera-privacy state. Windows reads the CapabilityAccessManager
// consent store; macOS queries AVCaptureDevice authorization.
CameraAccess cameraAccessStatus();

} // namespace harpia
