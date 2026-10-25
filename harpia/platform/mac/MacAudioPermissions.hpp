#pragma once

namespace harpia::mac_audio {
// Read-only preflight. Source creation must not trigger a privacy prompt while
// the main window is being constructed. The recorder's explicit Record flow
// owns asking for permission and retrying the requested capture afterwards.
bool permissionGranted();
}
