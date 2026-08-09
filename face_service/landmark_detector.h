#pragma once

#include <onnxruntime_cxx_api.h>
#include <dlib/image_processing.h>
#include <string>
#include <memory>
#include <vector>
#include <mutex>

namespace facelogin {

// A detected face with its 106-point landmarks. Used by the enrollment preview
// overlay to draw the face box and landmark points.
struct FaceWithLandmarks {
    dlib::rectangle rect;
    dlib::full_object_detection landmarks; // 106 points
};

// ONNX-based 106-point facial landmark detection (insightface 2d106det, 5MB).
// Replaces the 99.7MB dlib 68-point shape predictor. The model is trained on
// the merlin/ibug 106-point annotation; point indices used elsewhere in the
// pipeline (blink EAR, head-pose gate, 5-point alignment) follow that order.
//
// Point layout (0-based; "left/right" below are the SUBJECT's first-person
// left/right, i.e. mirrored in image coordinates):
//   face contour  0-32
//   right eyebrow 43-51    left eyebrow  97-105
//   right eye     33-42    left eye      87-96
//   outer mouth   52-71
//   nose bridge   72-86
//
// For blink EAR (per subject eye):
//   right eye:  corners 39(outer)/35(inner), upper lid 41-40-42, lower lid 36-33-37, center 38
//   left eye:   corners 93(outer)/89(inner), upper lid 96-94-95, lower lid 91-87-90, center 88
// Head-pose gate: nose bridge 72-73-74-86-80; eye-line corners 39/93.
class OnnxLandmarkDetector {
public:
    OnnxLandmarkDetector() = default;
    ~OnnxLandmarkDetector();

    bool Initialize(const std::wstring& modelPath);

    // Detect 106 landmarks for a face box (from SCRFD). The box is expanded
    // to a 192×192 crop via the same similarity transform insightface uses
    // (bbox center → crop center, scale = 192 / (max(w,h)*1.5)). The detected
    // points are mapped back to image coordinates. Returns true and fills
    // `outLandmarks` (106 parts, image coordinates) on success.
    bool DetectLandmarks(const dlib::matrix<dlib::rgb_pixel>& image,
                         const dlib::rectangle& faceBox,
                         dlib::full_object_detection& outLandmarks);

    bool IsInitialized() const { return m_initialized; }

    // 106-point indices used elsewhere (documented above).
    static constexpr int kLandmarkCount = 106;

private:
    std::unique_ptr<Ort::Env> m_env;
    std::unique_ptr<Ort::Session> m_session;
    std::unique_ptr<Ort::MemoryInfo> m_memoryInfo;
    bool m_initialized = false;

    std::string m_inputName;
    std::string m_outputName;

    // 192×192 model input.
    static constexpr int kInputSize = 192;

    // ONNX Runtime sessions are not thread-safe for concurrent Run(), and the
    // reusable buffers below are shared mutable state. m_runMutex serializes
    // the whole inference path (warp → Run → decode).
    std::mutex m_runMutex;

    // Reusable buffers (perf: avoid re-allocating the ~110KB input tensor and
    // the 106-point vector on every DetectLandmarks). Allocated in Initialize(),
    // guarded by m_runMutex.
    std::vector<float> m_tensor;   // [1,3,192,192] NCHW
    std::vector<dlib::dpoint> m_parts;  // 106 output points
};

} // namespace facelogin
