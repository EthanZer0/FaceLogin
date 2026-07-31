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

namespace facelogin {

// Webcam capture using Media Foundation IMFSourceReader.
// Captures frames in NV12 format and converts to RGB for dlib.
// Falls back to native camera format if NV12 is not available.

class WebcamCapture {
public:
    WebcamCapture() = default;
    ~WebcamCapture();

    WebcamCapture(const WebcamCapture&) = delete;
    WebcamCapture& operator=(const WebcamCapture&) = delete;

    bool Initialize(int preferredWidth = 1280, int preferredHeight = 720);
    bool IsInitialized() const { return m_initialized; }
    bool GrabFrame(dlib::matrix<dlib::rgb_pixel>& outFrame);
    bool IsFrameReady();
    void Shutdown();

    static bool InitializeMF();
    static void ShutdownMF();

private:
    bool FindFirstCamera(IMFMediaSource** ppSource);
    bool ConfigureReader(int width, int height);
    bool ConvertNV12toRGB(IMFSample* pSample, dlib::matrix<dlib::rgb_pixel>& outFrame);
    bool ConvertYUY2toRGB(IMFSample* pSample, dlib::matrix<dlib::rgb_pixel>& outFrame);

    IMFMediaSource* m_pSource = nullptr;
    IMFSourceReader* m_pReader = nullptr;
    int m_width = 1280;
    int m_height = 720;
    bool m_initialized = false;
    bool m_isNV12 = true;
    static bool s_mfInitialized;
    static int s_mfRefCount;
};

} // namespace facelogin
