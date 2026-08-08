#include "onnx_models.h"
#include "../common/logger.h"
#include <dlib/image_transforms.h>
#include <fstream>
#include <algorithm>
#include <cmath>

namespace facelogin {

// ============================================================================
// Low-light enhancement (shared by recognizer + anti-spoof)
// ============================================================================

// A chip is "dark" when its mean luma is below ~40/255 (0.157). Normal indoor
// faces are 100-180; genuinely dark scenes fall well below 40.
static constexpr float kLowLightMeanThreshold = 40.0f;
// Reference mean luma we stretch dark chips toward. ~110/255 ≈ mid-brightness,
// close to what InsightFace/DeepPixBiS were trained on.
static constexpr float kLowLightTargetMean   = 110.0f;

void ApplyLowLightEnhance(dlib::matrix<dlib::rgb_pixel>& chip) {
    const long n = static_cast<long>(chip.size());
    if (n == 0) return;

    // Mean luma over the chip.
    double sum = 0.0;
    for (long i = 0; i < n; i++) {
        const auto& p = chip(i);
        sum += 0.299 * p.red + 0.587 * p.green + 0.114 * p.blue;
    }
    float mean = static_cast<float>(sum / n);
    if (mean >= kLowLightMeanThreshold) return;  // not dark — no-op

    // Stretch brightness: gain brings the mean up to the target, clamped so a
    // bright pixel can't overflow past 255.
    float gain = kLowLightTargetMean / mean;
    for (long i = 0; i < n; i++) {
        auto& p = chip(i);
        int r = static_cast<int>(p.red   * gain + 0.5f);
        int g = static_cast<int>(p.green * gain + 0.5f);
        int b = static_cast<int>(p.blue  * gain + 0.5f);
        p.red   = static_cast<unsigned char>(r > 255 ? 255 : r);
        p.green = static_cast<unsigned char>(g > 255 ? 255 : g);
        p.blue  = static_cast<unsigned char>(b > 255 ? 255 : b);
    }
}

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
        // Don't retain an internal memory arena after inference: the service
        // runs one-shot auths, so the arena's "keep peak allocations for reuse"
        // behaviour just pins ~tens of MB of RSS after the first run and never
        // gives it back. Disabling the arena + mem-pattern returns intermediate
        // tensors to the heap allocator each run (ms-level cost, fine here).
        opts.DisableCpuMemArena();
        opts.DisableMemPattern();

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

        // Optional low-light enhancement (config-gated): normalize brightness
        // of dark chips so the embedding isn't distorted by a dark scene.
        if (m_lowLightEnhance) ApplyLowLightEnhance(resized);

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
    // Align face using a 5-point similarity transform (arcface template) and
    // the 106-point landmark indices, then ONNX infer. The 106-point model's
    // "subject-first-person" eye layout:
    //   right eye outer corner = 39, left eye outer corner = 93
    //   nose bridge end (nose tip) = 80
    //   mouth corners = 52 (left, image-left) / 69 (right, image-right)
    // Matches insightface's arcface_dst template:
    //   [right eye, left eye, nose, left mouth, right mouth]
    const int kArc[5] = {39, 93, 80, 52, 69};
    std::vector<dlib::vector<double, 2>> src, dst;
    src.reserve(5); dst.reserve(5);
    const double arcface_dst[5][2] = {
        {38.2946, 51.6963}, {73.5318, 51.5014}, {56.0252, 71.7366},
        {41.5493, 92.3655}, {70.7299, 92.2041}
    };
    for (int i = 0; i < 5; i++) {
        auto& p = landmarks.part(kArc[i]);
        src.emplace_back(static_cast<double>(p.x()), static_cast<double>(p.y()));
        dst.emplace_back(arcface_dst[i][0], arcface_dst[i][1]);
    }

    // Similarity transform src→dst, warp to 112×112, then embed.
    dlib::point_transform_affine tform = dlib::find_similarity_transform(src, dst);
    dlib::matrix<dlib::rgb_pixel> faceChip(112, 112);
    dlib::transform_image(image, faceChip, dlib::interpolate_bilinear(), tform);
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
        // Don't retain an internal memory arena after inference: the service
        // runs one-shot auths, so the arena's "keep peak allocations for reuse"
        // behaviour just pins ~tens of MB of RSS after the first run and never
        // gives it back. Disabling the arena + mem-pattern returns intermediate
        // tensors to the heap allocator each run (ms-level cost, fine here).
        opts.DisableCpuMemArena();
        opts.DisableMemPattern();

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
                                              float& outScaleX, float& outScaleY) {
    int srcH = static_cast<int>(image.nr());
    int srcW = static_cast<int>(image.nc());

    // SCRFD is exported for a fixed 640×640 input. insightface's official
    // SCRFD detect() does a DIRECT resize (no letterbox, no aspect-preserving
    // pad) — the 640×640 blob is a full distort of the source frame. Using the
    // same preprocessing is essential; a letterbox would shift the feature-map
    // anchors and produce misaligned boxes.
    //
    // CRITICAL: because the resize DISTORTS non-square frames (e.g. a 1280×720
    // camera frame is squeezed into 640×640), the x and y scale factors are
    // DIFFERENT. Using a single uniform scale here misplaces boxes by the
    // aspect-ratio difference — for 1280×720 a uniform factor pushes boxes past
    // the frame edge, so dlib landmark extraction fails and auth times out.
    const int targetSize = 640;
    outScaleX = static_cast<float>(srcW) / static_cast<float>(targetSize);
    outScaleY = static_cast<float>(srcH) / static_cast<float>(targetSize);

    dlib::matrix<dlib::rgb_pixel> resized(targetSize, targetSize);
    dlib::resize_image(image, resized);

    // BGR planar NCHW, normalized to [-1, 1] with (pixel - 127.5) / 128.
    // insightface normalizes with 128, NOT 255.
    std::vector<float> tensor(1 * 3 * targetSize * targetSize, 0.0f);
    for (int y = 0; y < targetSize; y++) {
        for (int x = 0; x < targetSize; x++) {
            const auto& p = resized(y, x);
            int base = y * targetSize + x;
            tensor[0 * targetSize * targetSize + base] = (static_cast<float>(p.blue)  - 127.5f) / 128.0f;
            tensor[1 * targetSize * targetSize + base] = (static_cast<float>(p.green) - 127.5f) / 128.0f;
            tensor[2 * targetSize * targetSize + base] = (static_cast<float>(p.red)   - 127.5f) / 128.0f;
        }
    }

    return tensor;
}

// SCRFD decode helpers (matching insightface's scrfd.py).
//
// SCRFD bbox output is distance-based: 4 channels [left, top, right, bottom]
// in units of stride steps from the anchor center. keypoints output is 10
// channels (5 points × 2), also relative to the anchor center.

// Convert (center, distance) predictions into a bounding box [x1,y1,x2,y2].
static inline void DistanceToBbox(float cx, float cy,
                                  const float* dist,
                                  float& x1, float& y1, float& x2, float& y2) {
    x1 = cx - dist[0];
    y1 = cy - dist[1];
    x2 = cx + dist[2];
    y2 = cy + dist[3];
}

// Convert (center, distance) predictions into a 5-keypoint list (10 floats).
static inline void DistanceToKps(float cx, float cy,
                                 const float* dist, float* kps) {
    for (int k = 0; k < 5; k++) {
        kps[k * 2]     = cx + dist[k * 2];
        kps[k * 2 + 1] = cy + dist[k * 2 + 1];
    }
}

std::vector<OnnxDetector::Detection> OnnxDetector::Detect(
    const dlib::matrix<dlib::rgb_pixel>& image) {
    std::vector<Detection> results;
    if (!m_initialized) return results;

    try {
        float scaleX = 0.0f, scaleY = 0.0f;
        auto input = Preprocess(image, scaleX, scaleY);

        // Fixed 640×640 model input.
        const int inputSize = 640;
        std::array<int64_t, 4> shape = {1, 3, inputSize, inputSize};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            *m_memoryInfo, input.data(), input.size(), shape.data(), shape.size());

        const char* inputNames2[] = {m_inputName.c_str()};
        std::vector<const char*> outNames;
        for (auto& name : m_outputNames) outNames.push_back(name.c_str());
        std::vector<Ort::Value> outputTensors = m_session->Run(
            Ort::RunOptions{},
            inputNames2, &inputTensor, 1,
            outNames.data(), outNames.size());

        // SCRFD outputs (9 tensors): for each of 3 strides {8, 16, 32}:
        //   scores  [N, 1]
        //   bboxes  [N, 4]   (distance-to-center: l,t,r,b in stride units)
        //   kps     [N, 10]  (5 points × 2, also distance-to-center)
        // N = (640/stride)² × 2 (num_anchors=2), centers repeated per anchor.
        constexpr int kStrides[3] = {8, 16, 32};
        constexpr float kScoreThreshold = 0.5f;

        struct RawDet {
            float score;
            float box[4];
            float kps[10];
        };
        std::vector<RawDet> raw;

        for (int s = 0; s < 3; s++) {
            int stride = kStrides[s];
            int grid = inputSize / stride;
            size_t numPts = static_cast<size_t>(grid) * grid;
            size_t numAnchors = numPts * 2;   // num_anchors = 2

            float* scoreData = outputTensors[s].GetTensorMutableData<float>();
            float* boxData   = outputTensors[3 + s].GetTensorMutableData<float>();
            float* kpsData   = outputTensors[6 + s].GetTensorMutableData<float>();

            // Precompute anchor centers for this stride, following insightface's
            // official scrfd.py exactly:
            //   anchor_centers = np.stack(np.mgrid[:height, :width][::-1], axis=-1)
            //                   .reshape(-1, 2) * stride
            // np.mgrid[:h,:w][::-1] produces ROW-major (x varies fastest = columns
            // inner loop). Center = (col, row) * stride, NO +0.5 offset.
            // Both anchors of a cell share this center; output is interleaved:
            //   index i → cell i/2, anchor i%2.
            std::vector<float> centerX(numPts), centerY(numPts);
            for (int r = 0; r < grid; r++) {          // row outer
                for (int c = 0; c < grid; c++) {      // col inner (x fastest)
                    centerX[r * grid + c] = c * stride;
                    centerY[r * grid + c] = r * stride;
                }
            }

            for (size_t i = 0; i < numAnchors; i++) {
                float score = scoreData[i];
                if (score < kScoreThreshold) continue;

                size_t cell = i / 2;   // both anchors share this cell's center
                float cx = centerX[cell];
                float cy = centerY[cell];

                RawDet det;
                det.score = score;

                // bbox distance is in stride units → multiply by stride.
                float dist[4] = { boxData[i * 4 + 0] * stride,
                                  boxData[i * 4 + 1] * stride,
                                  boxData[i * 4 + 2] * stride,
                                  boxData[i * 4 + 3] * stride };
                float x1, y1, x2, y2;
                DistanceToBbox(cx, cy, dist, x1, y1, x2, y2);
                det.box[0] = x1; det.box[1] = y1; det.box[2] = x2; det.box[3] = y2;

                // keypoints distance in stride units too.
                float kd[10];
                for (int k = 0; k < 10; k++) kd[k] = kpsData[i * 10 + k] * stride;
                DistanceToKps(cx, cy, kd, det.kps);

                raw.push_back(det);
            }
        }

        // Non-maximum suppression across all strides (score-descending greedy).
        std::sort(raw.begin(), raw.end(),
            [](const RawDet& a, const RawDet& b) { return a.score > b.score; });

        constexpr float kNmsIoU = 0.5f;
        std::vector<bool> suppressed(raw.size(), false);
        for (size_t i = 0; i < raw.size(); i++) {
            if (suppressed[i]) continue;
            const RawDet& a = raw[i];
            float ax1 = a.box[0], ay1 = a.box[1], ax2 = a.box[2], ay2 = a.box[3];
            float aArea = (ax2 - ax1) * (ay2 - ay1) + 1e-5f;

            for (size_t j = i + 1; j < raw.size(); j++) {
                if (suppressed[j]) continue;
                const RawDet& b = raw[j];
                float ix = std::min(ax2, b.box[2]) - std::max(ax1, b.box[0]);
                float iy = std::min(ay2, b.box[3]) - std::max(ay1, b.box[1]);
                if (ix <= 0 || iy <= 0) continue;
                float inter = ix * iy;
                float bArea = (b.box[2] - b.box[0]) * (b.box[3] - b.box[1]) + 1e-5f;
                float iou = inter / (aArea + bArea - inter + 1e-5f);
                if (iou > kNmsIoU) suppressed[j] = true;
            }
        }

        // Map surviving detections back to source-pixel coordinates. The model
        // DISTORTS non-square frames, so x and y scale independently.
        for (size_t i = 0; i < raw.size(); i++) {
            if (suppressed[i]) continue;
            const RawDet& d = raw[i];
            Detection det;
            det.x1 = d.box[0] * scaleX;
            det.y1 = d.box[1] * scaleY;
            det.x2 = d.box[2] * scaleX;
            det.y2 = d.box[3] * scaleY;
            det.score = d.score;
            for (int k = 0; k < 5; k++) {
                det.kps[k * 2]     = d.kps[k * 2]     * scaleX;
                det.kps[k * 2 + 1] = d.kps[k * 2 + 1] * scaleY;
            }
            results.push_back(det);
        }

        // Sort by confidence descending (already score-sorted after NMS, but
        // keep it explicit for the public contract).
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
        // Don't retain an internal memory arena after inference: the service
        // runs one-shot auths, so the arena's "keep peak allocations for reuse"
        // behaviour just pins ~tens of MB of RSS after the first run and never
        // gives it back. Disabling the arena + mem-pattern returns intermediate
        // tensors to the heap allocator each run (ms-level cost, fine here).
        opts.DisableCpuMemArena();
        opts.DisableMemPattern();

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

        // Optional low-light enhancement (config-gated): normalize brightness
        // of dark chips so anti-spoof scores don't drop in dark scenes.
        if (m_lowLightEnhance) ApplyLowLightEnhance(resized);

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
    // Use a tight crop of the face bbox with a small 1.1x margin. Measured
    // empirically: the previous 1.5x half-size (3x total) included too much
    // background and drove real-face scores down to ~0.3-0.45, near the
    // threshold. A tight crop raises real scores to ~0.5-0.6+.
    dlib::rectangle rect = landmarks.get_rect();
    long cx = rect.left() + rect.width() / 2;
    long cy = rect.top() + rect.height() / 2;
    long halfSize = static_cast<long>(std::max(rect.width(), rect.height()) / 2 * 1.1f);
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
