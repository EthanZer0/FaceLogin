#include "face_detector.h"
#include "../common/logger.h"
#include <fstream>

namespace facelogin {

bool FaceDetector::Initialize(const std::wstring& shapePredictorPath) {
    try {
        // Pass the wide path directly: MSVC's std::ifstream accepts a
        // std::wstring path and opens it via the wide-char Win32 API, which
        // handles non-ASCII (e.g. Chinese) install paths correctly. Converting
        // to a narrow std::string via begin()/end() would truncate each UTF-16
        // char to one byte, garbling the path (e.g. "测试" → "KmՋ").
        std::ifstream ifs(shapePredictorPath, std::ios::binary);
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
