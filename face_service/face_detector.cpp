#include "face_detector.h"
#include "../common/logger.h"
#include <fstream>

namespace facelogin {

bool FaceDetector::Initialize(const std::wstring& shapePredictorPath) {
    try {
        m_hogDetector = dlib::get_frontal_face_detector();

        std::string narrowPath(shapePredictorPath.begin(), shapePredictorPath.end());
        std::ifstream ifs(narrowPath, std::ios::binary);
        if (!ifs.is_open()) {
            FACELOGIN_ERROR(L"Failed to open shape predictor: %s", shapePredictorPath.c_str());
            return false;
        }
        dlib::deserialize(m_shapePredictor, ifs);

        m_initialized = true;
        FACELOGIN_INFO(L"FaceDetector initialized (HOG, detect-scale: pyramid-down)");
        return true;
    }
    catch (const std::exception& e) {
        FACELOGIN_ERROR(L"FaceDetector init failed: %hs", e.what());
        return false;
    }
}

// Run HOG detection on a downscaled copy to reduce CPU cost.
// Coordinates are mapped back to full-frame for landmark extraction.
std::vector<dlib::rectangle> FaceDetector::DetectRects(
    const dlib::matrix<dlib::rgb_pixel>& image) {
    std::vector<dlib::rectangle> rects;

    int srcW = static_cast<int>(image.nc());
    int srcH = static_cast<int>(image.nr());

    // Detect at 640×360 — one pyramid_down from 1280×720.
    dlib::matrix<dlib::rgb_pixel> small;
    float scale = 1.0f;
    if (srcW > 800) {
        dlib::pyramid_down<2> pd;
        small.set_size(srcH, srcW);
        dlib::assign_image(small, image);
        pd(small);
        scale = static_cast<float>(small.nc()) / static_cast<float>(srcW);
    } else {
        small = image;
    }

    std::vector<dlib::rectangle> smallRects;
    try {
        smallRects = m_hogDetector(small, 1);
    } catch (const std::exception& e) {
        FACELOGIN_WARN(L"HOG detection error: %hs", e.what());
        return rects;
    }

    float invScale = 1.0f / scale;
    for (auto& r : smallRects) {
        dlib::rectangle fullRect(
            static_cast<long>(r.left()   * invScale),
            static_cast<long>(r.top()    * invScale),
            static_cast<long>(r.right()  * invScale),
            static_cast<long>(r.bottom() * invScale));
        if (fullRect.left()   < 0)     fullRect.set_left(0);
        if (fullRect.top()    < 0)     fullRect.set_top(0);
        if (fullRect.right()  >= srcW) fullRect.set_right(srcW - 1);
        if (fullRect.bottom() >= srcH) fullRect.set_bottom(srcH - 1);
        rects.push_back(fullRect);
    }

    return rects;
}

std::vector<FaceWithLandmarks> FaceDetector::Detect(const dlib::matrix<dlib::rgb_pixel>& image) {
    std::vector<FaceWithLandmarks> results;

    if (!m_initialized) return results;

    try {
        // Detect faces at reduced resolution → full-frame coordinates
        std::vector<dlib::rectangle> faces = DetectRects(image);

        for (const auto& rect : faces) {
            FaceWithLandmarks fwl;
            fwl.rect = rect;
            // Landmarks computed on the full-resolution image
            fwl.landmarks = m_shapePredictor(image, rect);
            results.push_back(std::move(fwl));
        }
    }
    catch (const std::exception& e) {
        FACELOGIN_WARN(L"Face detection error: %hs", e.what());
    }

    return results;
}

std::optional<FaceWithLandmarks> FaceDetector::DetectLargestFace(const dlib::matrix<dlib::rgb_pixel>& image) {
    auto faces = Detect(image);
    if (faces.empty()) return std::nullopt;

    // Return the face with the largest area (closest to camera)
    auto largest = std::max_element(faces.begin(), faces.end(),
        [](const FaceWithLandmarks& a, const FaceWithLandmarks& b) {
            return a.rect.area() < b.rect.area();
        });

    return *largest;
}

dlib::full_object_detection FaceDetector::GetLandmarks(
    const dlib::matrix<dlib::rgb_pixel>& image,
    const dlib::rectangle& rect) {
    if (!m_initialized) return {};
    return m_shapePredictor(image, rect);
}

} // namespace facelogin
