#pragma once

#include <string>
#include <vector>

namespace facelogin {

// A video capture device. devicePath is the stable symbolic link
// (e.g. "\\?\usb#vid_046d...") — identical between the Media Foundation and
// DirectShow enumeration APIs for the same physical camera. friendlyName is
// the human-readable name shown in the enrollment UI.
struct CameraDeviceInfo {
    std::wstring devicePath;   // stable identifier (symbolic link)
    std::wstring friendlyName; // display name
};

} // namespace facelogin
