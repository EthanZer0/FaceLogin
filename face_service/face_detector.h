#pragma once

#include <dlib/image_processing/frontal_face_detector.h>
#include <dlib/image_processing/shape_predictor.h>
#include <dlib/image_processing.h>
#include <dlib/image_transforms/image_pyramid.h>

#include <vector>
#include <string>
#include <optional>

namespace facelogin {

struct FaceWithLandmarks {
    dlib::rectangle rect;
    dlib::full_object_detection landmarks; // 68 points
};

class FaceDetector {
public:
    FaceDetector() = default;
    ~FaceDetector() = default;

    // Load the shape predictor model file.
    bool Initialize(const std::wstring& shapePredictorPath);

    bool IsInitialized() const { return m_initialized; }

    // Detect all faces in an image and compute landmarks for each.
    std::vector<FaceWithLandmarks> Detect(const dlib::matrix<dlib::rgb_pixel>& image);
    std::optional<FaceWithLandmarks> DetectLargestFace(const dlib::matrix<dlib::rgb_pixel>& image);

    // Compute landmarks for an externally-detected rectangle
    dlib::full_object_detection GetLandmarks(const dlib::matrix<dlib::rgb_pixel>& image,
                                              const dlib::rectangle& rect);

private:
    // Run HOG detection at reduced resolution, map rects back to full frame.
    std::vector<dlib::rectangle> DetectRects(const dlib::matrix<dlib::rgb_pixel>& image);

    dlib::frontal_face_detector m_hogDetector;
    dlib::shape_predictor m_shapePredictor;
    bool m_initialized = false;
};

} // namespace facelogin
