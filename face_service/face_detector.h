#pragma once

#include <dlib/image_processing/shape_predictor.h>
#include <dlib/image_processing.h>

#include <string>
#include <optional>

namespace facelogin {

// A detected face with its 68-point landmarks. Used by the enrollment preview
// overlay to draw the face box.
struct FaceWithLandmarks {
    dlib::rectangle rect;
    dlib::full_object_detection landmarks; // 68 points
};

class FaceDetector {
public:
    FaceDetector() = default;
    ~FaceDetector() = default;

    // Load the shape predictor model file (68-point landmarks).
    // This is the ONLY remaining dlib component — it is required by both the
    // ONNX recognizer (face alignment) and the blink liveness detector.
    // The dlib HOG face detector and dlib ResNet-34 recognizer were removed.
    bool Initialize(const std::wstring& shapePredictorPath);

    bool IsInitialized() const { return m_initialized; }

    // Compute 68-point landmarks for an externally-detected face rectangle
    // (from the SCRFD detector).
    dlib::full_object_detection GetLandmarks(const dlib::matrix<dlib::rgb_pixel>& image,
                                              const dlib::rectangle& rect);

private:
    dlib::shape_predictor m_shapePredictor;
    bool m_initialized = false;
};

} // namespace facelogin
