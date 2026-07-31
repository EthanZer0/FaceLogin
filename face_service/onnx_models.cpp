#include "onnx_models.h"
#include "../common/logger.h"
#include <dlib/image_transforms.h>
#include <fstream>
#include <algorithm>
#include <cmath>

namespace facelogin {

// ============================================================================
// OnnxRecognizer
// ============================================================================

OnnxRecognizer::~OnnxRecognizer() = default;

bool OnnxRecognizer::Initialize(const std::wstring& modelPath) {
    try {
        m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "FaceLogin");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(2);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        std::wstring wpath(modelPath.begin(), modelPath.end());
        m_session = std::make_unique<Ort::Session>(*m_env, wpath.c_str(), opts);

        m_memoryInfo = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

        // Cache input/output names
        Ort::AllocatorWithDefaultOptions alloc;
        m_inputName = m_session->GetInputNameAllocated(0, alloc).get();
        m_outputName = m_session->GetOutputNameAllocated(0, alloc).get();

        FACELOGIN_INFO(L"OnnxRecognizer initialized: %s", modelPath.c_str());
        FACELOGIN_INFO(L"  Input: %hs, Output: %hs", m_inputName.c_str(), m_outputName.c_str());

        m_initialized = true;
        return true;
    } catch (const std::exception& e) {
        FACELOGIN_ERROR(L"OnnxRecognizer init failed: %hs", e.what());
        return false;
    }
}

std::vector<float> OnnxRecognizer::ComputeEmbedding(
    const dlib::matrix<dlib::rgb_pixel>& faceChip) {
    if (!m_initialized) return {};

    try {
        int h = static_cast<int>(faceChip.nr());
        int w = static_cast<int>(faceChip.nc());

        // InsightFace buffalo_s expects 112x112 RGB, normalized to [-1, 1]
        // First resize to 112x112
        dlib::matrix<dlib::rgb_pixel> resized(112, 112);
        dlib::resize_image(faceChip, resized);

        // Convert to NCHW float tensor: [1, 3, 112, 112] normalized to [-1, 1]
        std::vector<float> input(1 * 3 * 112 * 112);
        const float scale = 1.0f / 127.5f;
        for (int y = 0; y < 112; y++) {
            for (int x = 0; x < 112; x++) {
                const auto& p = resized(y, x);
                int base = y * 112 + x;
                input[0 * 112 * 112 + base] = static_cast<float>(p.red)   * scale - 1.0f;
                input[1 * 112 * 112 + base] = static_cast<float>(p.green) * scale - 1.0f;
                input[2 * 112 * 112 + base] = static_cast<float>(p.blue)  * scale - 1.0f;
            }
        }

        std::array<int64_t, 4> shape = {1, 3, 112, 112};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            *m_memoryInfo, input.data(), input.size(), shape.data(), shape.size());

        const char* inputNames[] = {m_inputName.c_str()};
        const char* outName = m_outputName.c_str();
        const char* outputNames[] = {outName};
        auto outputs = m_session->Run(Ort::RunOptions{},
                                       inputNames, &inputTensor, 1,
                                       outputNames, 1);

        float* data = outputs[0].GetTensorMutableData<float>();
        auto info = outputs[0].GetTensorTypeAndShapeInfo();
        size_t dim = info.GetElementCount();

        std::vector<float> embedding(data, data + dim);

        // L2 normalize
        float norm = 0.0f;
        for (float v : embedding) norm += v * v;
        norm = std::sqrt(norm);
        if (norm > 1e-8f) {
            for (float& v : embedding) v /= norm;
        }

        return embedding;
    } catch (const std::exception& e) {
        FACELOGIN_WARN(L"OnnxRecognizer::ComputeEmbedding error: %hs", e.what());
        return {};
    }
}

std::vector<float> OnnxRecognizer::ComputeEmbedding(
    const dlib::matrix<dlib::rgb_pixel>& image,
    const dlib::full_object_detection& landmarks) {
    // Align face using dlib's chip extraction, then ONNX infer
    auto chipDetails = dlib::get_face_chip_details(landmarks, 112, 0.25);
    dlib::matrix<dlib::rgb_pixel> faceChip;
    dlib::extract_image_chip(image, chipDetails, faceChip);
    return ComputeEmbedding(faceChip);
}

float OnnxRecognizer::Distance(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 1e10f;
    float sum = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return std::sqrt(sum);
}

float OnnxRecognizer::Distance(const std::vector<float>& a, const float* b) {
    if (a.empty() || !b) return 1e10f;
    float sum = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return std::sqrt(sum);
}

// ============================================================================
// OnnxDetector
// ============================================================================

OnnxDetector::~OnnxDetector() = default;

bool OnnxDetector::Initialize(const std::wstring& modelPath) {
    try {
        m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "FaceLogin");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(2);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        std::wstring wpath(modelPath.begin(), modelPath.end());
        m_session = std::make_unique<Ort::Session>(*m_env, wpath.c_str(), opts);

        m_memoryInfo = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

        Ort::AllocatorWithDefaultOptions alloc;
        m_inputName = m_session->GetInputNameAllocated(0, alloc).get();

        size_t numOutputs = m_session->GetOutputCount();
        m_outputNames.resize(numOutputs);
        for (size_t i = 0; i < numOutputs; i++) {
            m_outputNames[i] = m_session->GetOutputNameAllocated(i, alloc).get();
        }

        FACELOGIN_INFO(L"OnnxDetector initialized: %s", modelPath.c_str());
        m_initialized = true;
        return true;
    } catch (const std::exception& e) {
        FACELOGIN_ERROR(L"OnnxDetector init failed: %hs", e.what());
        return false;
    }
}

std::vector<float> OnnxDetector::Preprocess(const dlib::matrix<dlib::rgb_pixel>& image,
                                              int& outWidth, int& outHeight,
                                              float& scaleX, float& scaleY) {
    int srcH = static_cast<int>(image.nr());
    int srcW = static_cast<int>(image.nc());

    // SCRFD uses 640x640 input
    const int targetSize = 640;
    float ratio = std::min(static_cast<float>(targetSize) / srcW,
                           static_cast<float>(targetSize) / srcH);
    int newW = static_cast<int>(srcW * ratio);
    int newH = static_cast<int>(srcH * ratio);
    // Pad to multiples of 32
    newW = ((newW + 31) / 32) * 32;
    newH = ((newH + 31) / 32) * 32;

    outWidth = newW;
    outHeight = newH;
    scaleX = static_cast<float>(srcW) / newW;
    scaleY = static_cast<float>(srcH) / newH;

    // Resize + letterbox
    dlib::matrix<dlib::rgb_pixel> resized(newH, newW);
    dlib::resize_image(image, resized);

    // BGR planar to NCHW, normalize to [0, 1]
    std::vector<float> tensor(1 * 3 * newH * newW, 0.0f);
    for (int y = 0; y < newH; y++) {
        for (int x = 0; x < newW; x++) {
            const auto& p = resized(y, x);
            int base = y * newW + x;
            tensor[0 * newH * newW + base] = p.blue  / 255.0f;
            tensor[1 * newH * newW + base] = p.green / 255.0f;
            tensor[2 * newH * newW + base] = p.red   / 255.0f;
        }
    }

    return tensor;
}

std::vector<OnnxDetector::Detection> OnnxDetector::Detect(
    const dlib::matrix<dlib::rgb_pixel>& image) {
    std::vector<Detection> results;
    if (!m_initialized) return results;

    try {
        int newW, newH;
        float scaleX, scaleY;
        auto input = Preprocess(image, newW, newH, scaleX, scaleY);

        std::array<int64_t, 4> shape = {1, 3, newH, newW};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            *m_memoryInfo, input.data(), input.size(), shape.data(), shape.size());

        const char* inputNames2[] = {m_inputName.c_str()};
        std::vector<const char*> outNames;
        for (auto& name : m_outputNames) outNames.push_back(name.c_str());
        std::vector<Ort::Value> outputTensors = m_session->Run(
            Ort::RunOptions{},
            inputNames2, &inputTensor, 1,
            outNames.data(), outNames.size());

        // SCRFD outputs: scores [1, N, 1], bboxes [1, N, 4], kps [1, N, 10]
        // Actual format depends on specific model. Common format:
        // output 0: detection boxes [1, N, 4] or [1, N, 5] (x1,y1,x2,y2,score)
        // output 1: keypoints [1, N, 10]

        auto& detOut = outputTensors[0];
        auto detInfo = detOut.GetTensorTypeAndShapeInfo();
        auto detShape = detInfo.GetShape();
        float* detData = detOut.GetTensorMutableData<float>();

        size_t numDets = detShape.size() >= 2 ? detShape[1] : 0;
        if (numDets == 0) return results;

        // Determine format: if shape[2] == 5, it's [x1,y1,x2,y2,score]
        // if shape[2] == 4, scores are separate
        size_t detDim = detShape.size() >= 3 ? detShape[2] : 0;

        float* kpsData = nullptr;
        if (outputTensors.size() > 1) {
            kpsData = outputTensors[1].GetTensorMutableData<float>();
        }

        for (size_t i = 0; i < numDets; i++) {
            float score;
            float x1, y1, x2, y2;

            if (detDim >= 5) {
                // Format: [score, x1, y1, x2, y2] or [x1, y1, x2, y2, score]
                // Try [x1, y1, x2, y2, score]
                x1 = detData[i * 5 + 0] * scaleX;
                y1 = detData[i * 5 + 1] * scaleY;
                x2 = detData[i * 5 + 2] * scaleX;
                y2 = detData[i * 5 + 3] * scaleY;
                score = detData[i * 5 + 4];
            } else if (detDim >= 4) {
                x1 = detData[i * 4 + 0] * scaleX;
                y1 = detData[i * 4 + 1] * scaleY;
                x2 = detData[i * 4 + 2] * scaleX;
                y2 = detData[i * 4 + 3] * scaleY;
                score = 0.5f; // unknown
            } else {
                continue;
            }

            if (score < 0.5f) continue;

            Detection det;
            det.x1 = x1; det.y1 = y1; det.x2 = x2; det.y2 = y2;
            det.score = score;

            if (kpsData) {
                for (int k = 0; k < 10; k++) {
                    det.kps[k] = kpsData[i * 10 + k];
                    if (k % 2 == 0) det.kps[k] *= scaleX;
                    else             det.kps[k] *= scaleY;
                }
            } else {
                std::fill(det.kps, det.kps + 10, 0.0f);
            }

            results.push_back(det);
        }

        // Sort by confidence descending
        std::sort(results.begin(), results.end(),
            [](const Detection& a, const Detection& b) { return a.score > b.score; });

    } catch (const std::exception& e) {
        FACELOGIN_WARN(L"OnnxDetector::Detect error: %hs", e.what());
    }

    return results;
}

std::optional<OnnxDetector::Detection> OnnxDetector::DetectLargestFace(
    const dlib::matrix<dlib::rgb_pixel>& image) {
    auto detections = Detect(image);
    if (detections.empty()) return std::nullopt;

    auto largest = std::max_element(detections.begin(), detections.end(),
        [](const Detection& a, const Detection& b) {
            float areaA = (a.x2 - a.x1) * (a.y2 - a.y1);
            float areaB = (b.x2 - b.x1) * (b.y2 - b.y1);
            return areaA < areaB;
        });

    return *largest;
}

// ============================================================================
// OnnxAntiSpoof (DeepPixBiS)
// ============================================================================

static constexpr float DPB_MEAN[] = {0.485f, 0.456f, 0.406f};
static constexpr float DPB_STD[]  = {0.229f, 0.224f, 0.225f};

OnnxAntiSpoof::~OnnxAntiSpoof() = default;

bool OnnxAntiSpoof::Initialize(const std::wstring& modelPath) {
    try {
        m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "FaceLogin");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(2);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        std::wstring wpath(modelPath.begin(), modelPath.end());
        m_session = std::make_unique<Ort::Session>(*m_env, wpath.c_str(), opts);

        m_memoryInfo = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

        Ort::AllocatorWithDefaultOptions alloc;
        m_inputName = m_session->GetInputNameAllocated(0, alloc).get();

        size_t numOutputs = m_session->GetOutputCount();
        m_outputNames.resize(numOutputs);
        for (size_t i = 0; i < numOutputs; i++) {
            m_outputNames[i] = m_session->GetOutputNameAllocated(i, alloc).get();
        }

        // Determine input size from session
        auto inputInfo = m_session->GetInputTypeInfo(0);
        auto tensorInfo = inputInfo.GetTensorTypeAndShapeInfo();
        auto shape = tensorInfo.GetShape();
        if (shape.size() >= 4) {
            m_inputSize = static_cast<int>(shape[2]);
        }

        FACELOGIN_INFO(L"OnnxAntiSpoof initialized: %s (input=%d, outputs=%zu)",
                      modelPath.c_str(), m_inputSize, numOutputs);
        m_initialized = true;
        return true;
    } catch (const std::exception& e) {
        FACELOGIN_ERROR(L"OnnxAntiSpoof init failed: %hs", e.what());
        return false;
    }
}

float OnnxAntiSpoof::Predict(const dlib::matrix<dlib::rgb_pixel>& faceChip) {
    if (!m_initialized) return -1.0f;
    try {
        int isize = m_inputSize; // 224 for DeepPixBiS
        dlib::matrix<dlib::rgb_pixel> resized(isize, isize);
        dlib::resize_image(faceChip, resized);

        // DeepPixBiS expects RGB NCHW, ImageNet normalization:
        //   pixel = (pixel/255 - mean) / std
        std::vector<float> input(1 * 3 * isize * isize);
        for (int y = 0; y < isize; y++) {
            for (int x = 0; x < isize; x++) {
                const auto& p = resized(y, x);
                int base = y * isize + x;
                input[0 * isize * isize + base] = (p.red   / 255.0f - DPB_MEAN[0]) / DPB_STD[0];
                input[1 * isize * isize + base] = (p.green / 255.0f - DPB_MEAN[1]) / DPB_STD[1];
                input[2 * isize * isize + base] = (p.blue  / 255.0f - DPB_MEAN[2]) / DPB_STD[2];
            }
        }

        std::array<int64_t, 4> shape = {1, 3, isize, isize};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            *m_memoryInfo, input.data(), input.size(), shape.data(), shape.size());

        const char* inputNames[] = {m_inputName.c_str()};
        std::vector<const char*> outNamePtrs;
        for (auto& name : m_outputNames) outNamePtrs.push_back(name.c_str());

        auto outputs = m_session->Run(Ort::RunOptions{},
                                       inputNames, &inputTensor, 1,
                                       outNamePtrs.data(), outNamePtrs.size());

        if (outputs.size() >= 2) {
            // DeepPixBiS dual-head: output_pixel (per-pixel map) + output_binary (scalar).
            // output_pixel is a 14×14 map where each pixel is classified as real (1) or spoof (0).
            // Its mean value discriminates: real faces ~0.5-0.7, photos ~0.2-0.4.
            // output_binary is the global classification but saturates at ~0.999 for all inputs,
            // making it useless for discrimination. Use pixel output only.

            float* pixelData = outputs[0].GetTensorMutableData<float>();
            auto pixelInfo = outputs[0].GetTensorTypeAndShapeInfo();
            size_t pixelCount = pixelInfo.GetElementCount();
            double pixelSum = 0;
            for (size_t i = 0; i < pixelCount; i++) pixelSum += pixelData[i];
            float pixelMean = static_cast<float>(pixelSum / pixelCount);

            float* binaryData = outputs[1].GetTensorMutableData<float>();
            auto binaryInfo = outputs[1].GetTensorTypeAndShapeInfo();
            size_t binaryCount = binaryInfo.GetElementCount();
            double binarySum = 0;
            for (size_t i = 0; i < binaryCount; i++) binarySum += binaryData[i];
            float binaryMean = static_cast<float>(binarySum / binaryCount);

            // Use pixel map mean as the liveness score.
            // binaryMean saturates near 1.0 for everything; pixelMean is the discriminator.
            // Threshold: >= 0.5 for real face, < 0.5 for photo/spoof.
            float score = pixelMean;
            FACELOGIN_INFO(L"Anti-spoof: pixel=%.4f binary=%.4f score=%.4f",
                          pixelMean, binaryMean, score);
            return score;
        }

        // Single output fallback
        float* data = outputs[0].GetTensorMutableData<float>();
        auto info = outputs[0].GetTensorTypeAndShapeInfo();
        size_t dim = info.GetElementCount();
        double sum = 0;
        for (size_t i = 0; i < dim; i++) sum += data[i];
        return static_cast<float>(sum / dim);
    } catch (const std::exception& e) {
        FACELOGIN_WARN(L"OnnxAntiSpoof::Predict error: %hs", e.what());
        return -1.0f;
    }
}

float OnnxAntiSpoof::Predict(const dlib::matrix<dlib::rgb_pixel>& image,
                              const dlib::full_object_detection& landmarks) {
    // DeepPixBiS works with a simple bbox crop (no landmark alignment).
    dlib::rectangle rect = landmarks.get_rect();
    long cx = rect.left() + rect.width() / 2;
    long cy = rect.top() + rect.height() / 2;
    long halfSize = std::max(rect.width(), rect.height()) / 2;
    halfSize = static_cast<long>(halfSize * 1.5); // ~3x total margin
    dlib::rectangle cropRect(cx - halfSize, cy - halfSize, cx + halfSize, cy + halfSize);
    dlib::chip_details chip(cropRect, dlib::chip_dims(m_inputSize, m_inputSize));
    dlib::matrix<dlib::rgb_pixel> crop;
    dlib::extract_image_chip(image, chip, crop);
    return Predict(crop);
}

bool OnnxAntiSpoof::IsReal(const dlib::matrix<dlib::rgb_pixel>& faceChip, float threshold) {
    float score = Predict(faceChip);
    return score >= threshold;
}

} // namespace facelogin
