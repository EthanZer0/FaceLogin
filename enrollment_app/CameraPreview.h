#pragma once

#include <windows.h>
#include <memory>
#include <dlib/matrix.h>

#include "../face_service/webcam_capture.h"

namespace facelogin {

// Simple camera preview renderer for the enrollment wizard.
// Renders webcam frames into a child window (IDC_PREVIEW) using
// SetDIBitsToDevice for basic display.

class CameraPreview {
public:
    CameraPreview();
    ~CameraPreview();

    bool Initialize(HWND hPreviewWnd, int width = 1280, int height = 720);

    // Capture a single frame. Returns true on success.
    bool CaptureFrame(dlib::matrix<dlib::rgb_pixel>& outFrame);

    // Capture a frame and render it to the preview window.
    bool CaptureAndRender();

    // Stop the preview
    void Stop();

    bool IsInitialized() const { return m_initialized; }

private:
    HWND m_hPreviewWnd = nullptr;
    int m_width = 1280;
    int m_height = 720;
    bool m_initialized = false;
};

} // namespace facelogin
