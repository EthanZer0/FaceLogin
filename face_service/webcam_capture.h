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
#include <dshow.h>      // IAMVideoProcAmp / IAMCameraControl (camera controls)
#include <mutex>

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
    bool IsInitialized() const;   // locked — safe against concurrent Shutdown
    bool GrabFrame(dlib::matrix<dlib::rgb_pixel>& outFrame);
    bool IsFrameReady();
    void Shutdown();

    // Camera control interfaces for the face-exposure controller (1.9.0).
    // QI'd off the media source during Initialize; null when the device does
    // not expose them. Borrowed pointers — valid until Shutdown().
    IAMVideoProcAmp* GetVideoProcAmp() const { return m_vpa; }
    IAMCameraControl* GetCameraControl() const { return m_cc; }

    static bool InitializeMF();
    static void ShutdownMF();

    // Enumerate all video capture devices. Requires InitializeMF() first.
    static std::vector<CameraDeviceInfo> ListCameras();

private:
    // Find the camera matching devicePath (fallback: first device).
    bool FindCamera(const std::wstring& devicePath, IMFMediaSource** ppSource);
    bool ConfigureReader(int width, int height);
    bool ConvertNV12toRGB(IMFSourceReader* reader, IMFSample* pSample,
                          dlib::matrix<dlib::rgb_pixel>& outFrame);
    bool ConvertYUY2toRGB(IMFSourceReader* reader, IMFSample* pSample,
                          dlib::matrix<dlib::rgb_pixel>& outFrame);

    // Serializes the camera lifecycle: Initialize / Shutdown / GrabFrame's
    // interface handoff. Shutdown() hands the MF interfaces to a detached
    // worker (bounded teardown), and GrabFrame takes a short-lived AddRef
    // under this lock — so a concurrent Shutdown can never Release the reader
    // out from under an in-flight ReadSample (double-Release / use-after-free
    // crashes on wedged camera drivers).
    mutable std::mutex m_lifecycleMutex;
    IMFMediaSource* m_pSource = nullptr;
    IMFSourceReader* m_pReader = nullptr;
    IAMVideoProcAmp* m_vpa = nullptr;    // camera controls, QI'd off the source
    IAMCameraControl* m_cc = nullptr;
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
    // Process-level counter of teardown timeouts (wedged camera driver) —
    // reported in the Shutdown() WARN so repeated occurrences are visible
    // even when individual calls recover.
    static int s_teardownTimeouts;
};

} // namespace facelogin
