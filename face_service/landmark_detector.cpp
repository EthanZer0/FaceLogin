#include "landmark_detector.h"
#include "../common/logger.h"
#include <algorithm>
#include <cmath>
#include <array>

namespace facelogin {

OnnxLandmarkDetector::~OnnxLandmarkDetector() = default;

bool OnnxLandmarkDetector::Initialize(const std::wstring& modelPath) {
    try {
        m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "FaceLogin");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // See onnx_models.cpp — disable the arena so the one-shot auth doesn't
        // pin intermediate-tensor memory in RSS after the first inference.
        opts.DisableCpuMemArena();
        opts.DisableMemPattern();

        std::wstring wpath(modelPath.begin(), modelPath.end());
        m_session = std::make_unique<Ort::Session>(*m_env, wpath.c_str(), opts);

        m_memoryInfo = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

        Ort::AllocatorWithDefaultOptions alloc;
        m_inputName = m_session->GetInputNameAllocated(0, alloc).get();
        m_outputName = m_session->GetOutputNameAllocated(0, alloc).get();

        // Allocate reusable buffers for the hot inference path.
        m_tensor.assign(1 * 3 * kInputSize * kInputSize, 0.0f);
        m_parts.clear();
        m_parts.reserve(kLandmarkCount);

        FACELOGIN_INFO(L"OnnxLandmarkDetector initialized: %s", modelPath.c_str());
        m_initialized = true;
        return true;
    } catch (const std::exception& e) {
        FACELOGIN_ERROR(L"OnnxLandmarkDetector init failed: %hs", e.what());
        return false;
    }
}

// Compute the similarity-transform parameters that map the face box into a
// 192×192 crop, exactly as insightface's face_align.transform does:
//   scale = 192 / (max(w,h) * 1.5)
//   warp: crop(x) = (image(x) - center) * scale + 96
// Returns scale and translation so crop = image * scale + (tx, ty).
static void FaceBoxToCrop(const dlib::rectangle& box, int outSize,
                          double& scale, double& tx, double& ty) {
    double w = static_cast<double>(box.width());
    double h = static_cast<double>(box.height());
    double cx = (box.left() + box.right()) / 2.0;
    double cy = (box.top() + box.bottom()) / 2.0;
    scale = static_cast<double>(outSize) / (std::max(w, h) * 1.5);
    tx = outSize / 2.0 - cx * scale;
    ty = outSize / 2.0 - cy * scale;
}

// Bilinear sample of `img` at fractional coordinates (sx, sy), with
// out-of-bounds taps contributing 0. This mirrors cv2.warpAffine's
// INTER_LINEAR with borderMode=BORDER_CONSTANT/borderValue=0 exactly — the
// interpolation insightface uses to build the 2d106det crop. (A nearest-
// neighbour sampler was the original bug: its up-to-0.5px per-tap rounding
// errors skewed the dense eye/nose landmarks while leaving the sparser face
// contour and lip points looking fine.)
static void SampleBilinear(const dlib::matrix<dlib::rgb_pixel>& img,
                           double sx, double sy,
                           float& r, float& g, float& b) {
    const long W = img.nc();
    const long H = img.nr();
    const long x0 = static_cast<long>(std::floor(sx));
    const long y0 = static_cast<long>(std::floor(sy));
    const float fx = static_cast<float>(sx - x0);
    const float fy = static_cast<float>(sy - y0);
    const float wx0 = 1.0f - fx, wx1 = fx;
    const float wy0 = 1.0f - fy, wy1 = fy;

    float wr = 0.0f, wg = 0.0f, wb = 0.0f;
    if (x0 >= 0 && x0 < W && y0 >= 0 && y0 < H) {
        const auto& p = img(y0, x0);
        wr += wx0 * wy0 * static_cast<float>(p.red);
        wg += wx0 * wy0 * static_cast<float>(p.green);
        wb += wx0 * wy0 * static_cast<float>(p.blue);
    }
    if (x0 + 1 >= 0 && x0 + 1 < W && y0 >= 0 && y0 < H) {
        const auto& p = img(y0, x0 + 1);
        wr += wx1 * wy0 * static_cast<float>(p.red);
        wg += wx1 * wy0 * static_cast<float>(p.green);
        wb += wx1 * wy0 * static_cast<float>(p.blue);
    }
    if (x0 >= 0 && x0 < W && y0 + 1 >= 0 && y0 + 1 < H) {
        const auto& p = img(y0 + 1, x0);
        wr += wx0 * wy1 * static_cast<float>(p.red);
        wg += wx0 * wy1 * static_cast<float>(p.green);
        wb += wx0 * wy1 * static_cast<float>(p.blue);
    }
    if (x0 + 1 >= 0 && x0 + 1 < W && y0 + 1 >= 0 && y0 + 1 < H) {
        const auto& p = img(y0 + 1, x0 + 1);
        wr += wx1 * wy1 * static_cast<float>(p.red);
        wg += wx1 * wy1 * static_cast<float>(p.green);
        wb += wx1 * wy1 * static_cast<float>(p.blue);
    }
    r = wr; g = wg; b = wb;
}

bool OnnxLandmarkDetector::DetectLandmarks(
    const dlib::matrix<dlib::rgb_pixel>& image,
    const dlib::rectangle& faceBox,
    dlib::full_object_detection& outLandmarks) {
    if (!m_initialized) return false;

    // Serialize inference: ONNX sessions are not thread-safe for concurrent
    // Run(), and the reusable buffers below are shared mutable state. The
    // Console's frame thread and capture thread share this session.
    std::lock_guard<std::mutex> lock(m_runMutex);

    try {
        // --- Warp the face box into a 192×192 crop (insightface transform) ---
        const int S = kInputSize;
        double scale, tx, ty;
        FaceBoxToCrop(faceBox, S, scale, tx, ty);

        // Build the 192×192 RGB input tensor. The model's blobFromImage uses
        // swapRB=True on a BGR source, i.e. it feeds RGB channel order.
        //
        // NORMALIZATION: 2d106det is a plain ONNX export (Pytorch), NOT an
        // MXNet ArcFace model. Its graph begins with Sub/Mul nodes, so
        // insightface's Landmark.__init__ picks input_mean=0, input_std=1 —
        // the raw pixel values [0,255] are fed WITHOUT the (p-127.5)/128
        // centering that SCRFD uses. Feeding (-1,1) here skews the eye
        // landmarks (right eye ~10px off in the crop); matching (0,1) makes
        // the 106 points land exactly on the SCRFD eyes.
        float* tensor = m_tensor.data();
        for (int y = 0; y < S; y++) {
            for (int x = 0; x < S; x++) {
                // Inverse of warp: source (sx, sy) for this crop pixel.
                double sx = (x - tx) / scale;
                double sy = (y - ty) / scale;
                // Bilinear sample with zero-filled borders (warpAffine semantics).
                float r, g, b;
                SampleBilinear(image, sx, sy, r, g, b);
                int base = y * S + x;
                tensor[0 * S * S + base] = r;  // R, raw [0,255]
                tensor[1 * S * S + base] = g;  // G
                tensor[2 * S * S + base] = b;  // B
            }
        }

        std::array<int64_t, 4> shape = {1, 3, S, S};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            *m_memoryInfo, tensor, m_tensor.size(), shape.data(), shape.size());

        const char* inputNames[] = {m_inputName.c_str()};
        const char* outName = m_outputName.c_str();
        const char* outputNames[] = {outName};
        auto outputs = m_session->Run(Ort::RunOptions{},
                                       inputNames, &inputTensor, 1,
                                       outputNames, 1);

        float* data = outputs[0].GetTensorMutableData<float>();
        // Output is [1, 212] = 106 points × (x,y). Fill the reusable parts buffer.
        m_parts.clear();
        for (int i = 0; i < kLandmarkCount; i++) {
            float nx = data[i * 2 + 0];   // normalized in [-1, 1]
            float ny = data[i * 2 + 1];
            // Denormalize to crop coordinates: (v + 1) * (S / 2)
            float cx = (nx + 1.0f) * (S / 2.0f);
            float cy = (ny + 1.0f) * (S / 2.0f);
            // Map back to image coordinates via inverse warp.
            float ix = static_cast<float>((cx - tx) / scale);
            float iy = static_cast<float>((cy - ty) / scale);
            m_parts.emplace_back(static_cast<long>(std::lround(ix)),
                                 static_cast<long>(std::lround(iy)));
        }
        if (static_cast<int>(m_parts.size()) != kLandmarkCount) return false;

        outLandmarks = dlib::full_object_detection(faceBox, m_parts);
        return true;
    } catch (const std::exception& e) {
        FACELOGIN_WARN(L"OnnxLandmarkDetector::DetectLandmarks error: %hs", e.what());
        return false;
    }
}

} // namespace facelogin
