#include "face_recognizer.h"
#include "../common/logger.h"
#include <fstream>

namespace facelogin {

bool FaceRecognizer::Initialize(const std::wstring& modelPath) {
    try {
        // dlib::deserialize needs an istream, not a wstring path
        std::string narrowPath(modelPath.begin(), modelPath.end());
        std::ifstream ifs(narrowPath, std::ios::binary);
        if (!ifs.is_open()) {
            FACELOGIN_ERROR(L"Failed to open model file: %s", modelPath.c_str());
            return false;
        }
        dlib::deserialize(m_net, ifs);
        m_initialized = true;
        FACELOGIN_INFO(L"FaceRecognizer initialized successfully");
        FACELOGIN_INFO(L"  Model: %s", modelPath.c_str());
        return true;
    }
    catch (const std::exception& e) {
        FACELOGIN_ERROR(L"FaceRecognizer init failed: %hs", e.what());
        return false;
    }
}

dlib::matrix<float, 0, 1> FaceRecognizer::ComputeEmbedding(
    const dlib::matrix<dlib::rgb_pixel>& faceChip) {
    if (!m_initialized) return {};

    try {
        // dlib face recognition network performs its own preprocessing
        return m_net(faceChip);
    }
    catch (const std::exception& e) {
        FACELOGIN_WARN(L"ComputeEmbedding error: %hs", e.what());
        return {};
    }
}

dlib::matrix<float, 0, 1> FaceRecognizer::ComputeEmbedding(
    const dlib::matrix<dlib::rgb_pixel>& image,
    const dlib::full_object_detection& landmarks) {
    if (!m_initialized) return {};

    try {
        // Get the face chip with 150x150 alignment
        auto chipDetails = dlib::get_face_chip_details(landmarks, 150, 0.25);
        dlib::matrix<dlib::rgb_pixel> faceChip;
        dlib::extract_image_chip(image, chipDetails, faceChip);
        return m_net(faceChip);
    }
    catch (const std::exception& e) {
        FACELOGIN_WARN(L"ComputeEmbedding(image+landmarks) error: %hs", e.what());
        return {};
    }
}

float FaceRecognizer::Distance(const dlib::matrix<float, 0, 1>& a,
                                const dlib::matrix<float, 0, 1>& b) {
    if (a.size() == 0 || b.size() == 0) return 1e10f;
    return static_cast<float>(dlib::length(a - b));
}

float FaceRecognizer::Distance(const dlib::matrix<float, 0, 1>& probe,
                                const float* storedEmbedding) {
    if (probe.size() == 0 || !storedEmbedding) return 1e10f;

    float sum = 0.0f;
    for (long i = 0; i < probe.size(); ++i) {
        float diff = probe(i) - storedEmbedding[i];
        sum += diff * diff;
    }
    return std::sqrt(sum);
}

std::vector<uint8_t> FaceRecognizer::SerializeEmbedding(const dlib::matrix<float, 0, 1>& emb) {
    if (emb.size() == 0) return {};

    size_t byteSize = emb.size() * sizeof(float);
    std::vector<uint8_t> result(byteSize);
    memcpy(result.data(), &emb(0), byteSize);
    return result;
}

dlib::matrix<float, 0, 1> FaceRecognizer::DeserializeEmbedding(const uint8_t* data) {
    dlib::matrix<float, 0, 1> emb(128);
    memcpy(&emb(0), data, 128 * sizeof(float));
    return emb;
}

} // namespace facelogin
