#pragma once

#include <onnxruntime_cxx_api.h>
#include <dlib/matrix.h>
#include <dlib/pixel.h>
#include <dlib/image_processing.h>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <mutex>
#include "liveness_types.h"

namespace facelogin {

// In-place low-light enhancement for a face chip. Detects darkness (mean
// luma below kLowLightMeanThreshold) and stretches brightness so the mean
// lands at a reference level, clamped to [0,255]. No-op for chips at normal
// brightness. Called on the RESIZED chip before the model's own normalization
// loop, so both InsightFace (recognizer) and DeepPixBiS (anti-spoof) see
// brightness-normalized input in dark scenes.
//
// Safe by construction: only affects genuinely dark chips; a normal-brightness
// chip is returned unchanged, so the match threshold and photo-rejection
// boundary are untouched.
void ApplyLowLightEnhance(dlib::matrix<dlib::rgb_pixel>& chip);

// ONNX-based face recognition using InsightFace buffalo_s (w600k_mbf).
// Replaces dlib ResNet-34 with the more accurate MobileFaceNet @ WebFace600K.
// Embedding dimension: 128 (compatible with existing users.dat storage).
class OnnxRecognizer {
public:
    OnnxRecognizer() = default;
    ~OnnxRecognizer();

    bool Initialize(const std::wstring& modelPath);

    // Compute 128-D embedding from a face chip (already aligned, 112x112 RGB).
    // Returns empty vector on failure.
    std::vector<float> ComputeEmbedding(const dlib::matrix<dlib::rgb_pixel>& faceChip);

    // Convenience: compute embedding from a full frame + landmarks.
    // Handles alignment to 112x112 internally.
    std::vector<float> ComputeEmbedding(
        const dlib::matrix<dlib::rgb_pixel>& image,
        const dlib::full_object_detection& landmarks);

    // Euclidean distance between two embeddings.
    static float Distance(const std::vector<float>& a, const std::vector<float>& b);
    static float Distance(const std::vector<float>& a, const float* b);

    bool IsInitialized() const { return m_initialized; }

    // Enable/disable low-light brightness normalization for dark face chips.
    void SetLowLightEnhance(bool enable) { m_lowLightEnhance = enable; }

private:
    std::unique_ptr<Ort::Env> m_env;
    std::unique_ptr<Ort::Session> m_session;
    std::unique_ptr<Ort::MemoryInfo> m_memoryInfo;
    bool m_initialized = false;
    bool m_lowLightEnhance = false;

    // Input/output names (cached after session creation)
    std::string m_inputName;
    std::string m_outputName;

    // ONNX Runtime sessions are not thread-safe for concurrent Run(), and the
    // reusable buffers below are shared mutable state. m_runMutex serializes
    // the whole inference path (preprocess → Run → postprocess) so the frame
    // thread and the capture thread can share these models safely.
    std::mutex m_runMutex;

    // Reusable buffers (perf: avoid re-allocating the ~38KB input tensor and
    // the 112×112 chip on every inference). Allocated in Initialize(), reused
    // across calls, guarded by m_runMutex.
    dlib::matrix<dlib::rgb_pixel> m_faceChip;   // 112×112 warp target
    std::vector<float> m_input;                 // [1,3,112,112] NCHW
    std::vector<float> m_embedding;             // output copy (L2-normalized)
};

// ONNX-based face detection using InsightFace SCRFD.
// Much faster than dlib HOG and more robust against non-live faces.
class OnnxDetector {
public:
    OnnxDetector() = default;
    ~OnnxDetector();

    bool Initialize(const std::wstring& modelPath);

    struct Detection {
        float x1, y1, x2, y2;  // bounding box in pixel coordinates
        float score;             // confidence
        float kps[10];           // 5 keypoints (x,y pairs): left-eye, right-eye, nose, left-mouth, right-mouth
    };

    // Detect faces. Returns detections sorted by confidence (highest first).
    std::vector<Detection> Detect(const dlib::matrix<dlib::rgb_pixel>& image);

    // Detect the largest face (by area). Returns nullopt if none found.
    std::optional<Detection> DetectLargestFace(const dlib::matrix<dlib::rgb_pixel>& image);

    bool IsInitialized() const { return m_initialized; }

private:
    std::unique_ptr<Ort::Env> m_env;
    std::unique_ptr<Ort::Session> m_session;
    std::unique_ptr<Ort::MemoryInfo> m_memoryInfo;
    bool m_initialized = false;

    std::string m_inputName;
    std::vector<std::string> m_outputNames;

    // ONNX Runtime sessions are not thread-safe for concurrent Run(), and the
    // reusable buffers below are shared mutable state. m_runMutex serializes
    // the whole inference path (preprocess → Run → decode → NMS).
    std::mutex m_runMutex;

    // Reusable buffers (perf: avoid re-allocating the 640² resize matrix, the
    // ~1.23M-float input tensor and the decode vectors on every Detect).
    // Allocated in Initialize(), reused across calls, guarded by m_runMutex.
    dlib::matrix<dlib::rgb_pixel> m_resized;   // 640×640 resize target
    std::vector<float> m_tensor;               // [1,3,640,640] NCHW
    std::vector<float> m_centerX, m_centerY;   // per-stride anchor centers
    std::vector<Detection> m_results;          // output collection (reused)

    // Preprocess: direct resize to 640×640 (no letterbox), BGR planar,
    // normalized to [-1, 1] with (pixel-127.5)/128. Matches the model's
    // native input; insightface SCRFD is exported this way.
    // Because the resize DISTORTS non-square frames (e.g. 1280×720 → 640×640),
    // the x and y scales are DIFFERENT. scaleX/scaleY map 640-space back to
    // source pixels: srcX = detX * scaleX, srcY = detY * scaleY.
    // Returns a reference to the reusable m_tensor buffer (caller must hold
    // m_runMutex — Detect does).
    std::vector<float>& Preprocess(const dlib::matrix<dlib::rgb_pixel>& image,
                                    float& outScaleX, float& outScaleY);
};

// Silent anti-spoofing detection.
// Rejects printed photos, screen replays, and 3D masks. Two supported models:
//   - facenox MiniFAS (default, 1.6.0): input 128×128 RGB, output [1,2] logits
//     (real, spoof). Score = real_logit - spoof_logit; >= 0 is real.
//   - DeepPixBiS (OULU): input 224×224 RGB, dual-head output (pixel map +
//     binary); score = pixel-map mean.
// The model's output shape auto-selects the mode at Initialize().
class OnnxAntiSpoof {
public:
    OnnxAntiSpoof() = default;
    ~OnnxAntiSpoof();

    bool Initialize(const std::wstring& modelPath);

    // Predict liveness score for an aligned face chip.
    // facenox mode: real_logit - spoof_logit (positive = real).
    // DeepPixBiS mode: pixel-map mean in [0,1] (higher = real).
    // Returns -1.0f on error.
    float Predict(const dlib::matrix<dlib::rgb_pixel>& faceChip);

    // Convenience: crop from landmarks + full image, then predict.
    float Predict(const dlib::matrix<dlib::rgb_pixel>& image,
                  const dlib::full_object_detection& landmarks);

    // Thresholded convenience: returns true if face is judged real.
    bool IsReal(const dlib::matrix<dlib::rgb_pixel>& faceChip, float threshold = 0.3f);

    bool IsInitialized() const { return m_initialized; }
    // True when running the facenox MiniFAS model (logit-diff scoring).
    bool IsFacenoxMode() const { return m_facenoxMode; }

    // Enable/disable low-light brightness normalization for dark face chips.
    void SetLowLightEnhance(bool enable) { m_lowLightEnhance = enable; }

private:
    std::unique_ptr<Ort::Env> m_env;
    std::unique_ptr<Ort::Session> m_session;
    std::unique_ptr<Ort::MemoryInfo> m_memoryInfo;
    bool m_initialized = false;
    bool m_lowLightEnhance = false;
    bool m_facenoxMode = false;   // [1,2] logit output (MiniFAS) vs pixel-map (DeepPixBiS)

    std::string m_inputName;
    std::string m_outputName;
    std::vector<std::string> m_outputNames; // DeepPixBiS has 2 outputs
    int m_inputSize = 224;

    // ONNX Runtime sessions are not thread-safe for concurrent Run(), and the
    // reusable buffers below are shared mutable state. m_runMutex serializes
    // the whole inference path.
    std::mutex m_runMutex;

    // Reusable buffers (perf: avoid re-allocating the resize matrix + input
    // tensor on every Predict). Allocated in Initialize(), guarded by m_runMutex.
    dlib::matrix<dlib::rgb_pixel> m_resized;   // model input size (128 or 224)
    std::vector<float> m_input;                // [1,3,N,N] NCHW
};

} // namespace facelogin
