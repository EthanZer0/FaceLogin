#include "CameraPreview.h"
#include <dlib/image_transforms.h>

namespace facelogin {

CameraPreview::CameraPreview() = default;
CameraPreview::~CameraPreview() {
    Stop();
}

bool CameraPreview::Initialize(HWND hPreviewWnd, int width, int height) {
    m_hPreviewWnd = hPreviewWnd;
    m_width = width;
    m_height = height;
    m_initialized = (hPreviewWnd != nullptr);
    return m_initialized;
}

bool CameraPreview::CaptureFrame(dlib::matrix<dlib::rgb_pixel>& outFrame) {
    if (!m_initialized) return false;

    // This is handled by the enrollment wizard which owns the WebcamCapture
    // We just render frames here
    return true;
}

bool CameraPreview::CaptureAndRender() {
    // Rendering is handled by the enrollment wizard which has direct access
    // to the webcam and dlib detection results.
    // This class exists for organizational clarity.
    return true;
}

void CameraPreview::Stop() {
    m_initialized = false;
    m_hPreviewWnd = nullptr;
}

} // namespace facelogin
