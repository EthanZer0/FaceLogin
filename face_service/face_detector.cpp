#include "face_detector.h"
#include "../common/logger.h"
#include <fstream>

namespace facelogin {

bool FaceDetector::Initialize(const std::wstring& shapePredictorPath) {
    try {
        std::string narrowPath(shapePredictorPath.begin(), shapePredictorPath.end());
        std::ifstream ifs(narrowPath, std::ios::binary);
        if (!ifs.is_open()) {
            FACELOGIN_ERROR(L"Failed to open shape predictor: %s", shapePredictorPath.c_str());
            return false;
        }
        dlib::deserialize(m_shapePredictor, ifs);

        m_initialized = true;
        FACELOGIN_INFO(L"FaceDetector initialized (68-point shape predictor)");
        return true;
    }
    catch (const std::exception& e) {
        FACELOGIN_ERROR(L"FaceDetector init failed: %hs", e.what());
        return false;
    }
}

dlib::full_object_detection FaceDetector::GetLandmarks(
    const dlib::matrix<dlib::rgb_pixel>& image,
    const dlib::rectangle& rect) {
    if (!m_initialized) return {};
    return m_shapePredictor(image, rect);
}

} // namespace facelogin
