#pragma once

#define WINVER 0x0602
#define _WIN32_WINNT 0x0602

#include <dlib/matrix.h>
#include <dlib/pixel.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfobjects.h>

#include "camera_types.h"

namespace facelogin {

// GrabFrame failures beyond this count mark the SourceReader as stalled
// (typically after system resume while the USB camera is in low-power
// recovery). WebcamCapture self-shuts-down so the caller can re-init fresh.
constexpr int kMaxConsecutiveGrabFailures = 3;

// Webcam capture using Media Foundation IMFSourceReader.
// Captures frames in NV12 format and converts to RGB for dlib.
// Falls back to native camera format if NV12 is not available.

class WebcamCapture {
public:
    WebcamCapture() = default;
    ~WebcamCapture();

    WebcamCapture(const WebcamCapture&) = delete;
    WebcamCapture& operator=(const WebcamCapture&) = delete;

    // devicePath: optional stable symbolic link of the camera to use.
    // Empty (default) = first enumerated device.
    bool Initialize(int preferredWidth = 1280, int preferredHeight = 720,
                    const std::wstring& devicePath = L"");
    bool IsInitialized() const { return m_initialized; }
    bool GrabFrame(dlib::matrix<dlib::rgb_pixel>& outFrame);
    bool IsFrameReady();
    void Shutdown();

    static bool InitializeMF();
    static void ShutdownMF();

    // Enumerate all video capture devices. Requires InitializeMF() first.
    static std::vector<CameraDeviceInfo> ListCameras();

private:
    // Find the camera matching devicePath (fallback: first device).
    bool FindCamera(const std::wstring& devicePath, IMFMediaSource** ppSource);
    bool ConfigureReader(int width, int height);
    bool ConvertNV12toRGB(IMFSample* pSample, dlib::matrix<dlib::rgb_pixel>& outFrame);
    bool ConvertYUY2toRGB(IMFSample* pSample, dlib::matrix<dlib::rgb_pixel>& outFrame);

    IMFMediaSource* m_pSource = nullptr;
    IMFSourceReader* m_pReader = nullptr;
    int m_width = 1280;
    int m_height = 720;
    bool m_initialized = false;
    bool m_isNV12 = true;
    // Consecutive GrabFrame failures. When a camera stalls after system resume
    // (low-power recovery), ReadSample keeps failing on a stale SourceReader;
    // after kMaxConsecutiveFailures we self-shutdown so the caller re-inits.
    int m_consecutiveFailures = 0;
    static bool s_mfInitialized;
    static int s_mfRefCount;
};

} // namespace facelogin
