#pragma once

#include <onnxruntime_cxx_api.h>
#include <dlib/matrix.h>
#include <dlib/pixel.h>
#include <dlib/image_processing.h>
#include <string>
#include <vector>
#include <memory>
#include <optional>
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

    // Preprocess: direct resize to 640×640 (no letterbox), BGR planar,
    // normalized to [-1, 1] with (pixel-127.5)/128. Matches the model's
    // native input; insightface SCRFD is exported this way.
    // Because the resize DISTORTS non-square frames (e.g. 1280×720 → 640×640),
    // the x and y scales are DIFFERENT. scaleX/scaleY map 640-space back to
    // source pixels: srcX = detX * scaleX, srcY = detY * scaleY.
    std::vector<float> Preprocess(const dlib::matrix<dlib::rgb_pixel>& image,
                                   float& outScaleX, float& outScaleY);
};

// Silent anti-spoofing detection (MiniFASNetV2).
// Distinguishes real faces from printed photos, screen replays, and 3D masks.
// Input: aligned face chip (80x80 RGB)
// Output: scalar score (higher = more likely real face)
class OnnxAntiSpoof {
public:
    OnnxAntiSpoof() = default;
    ~OnnxAntiSpoof();

    bool Initialize(const std::wstring& modelPath);

    // Predict liveness score for an aligned face chip.
    // Returns score in [0.0, 1.0]; higher = more likely real.
    // Returns -1.0f on error.
    float Predict(const dlib::matrix<dlib::rgb_pixel>& faceChip);

    // Convenience: align from landmarks + full image, then predict.
    float Predict(const dlib::matrix<dlib::rgb_pixel>& image,
                  const dlib::full_object_detection& landmarks);

    // Thresholded convenience: returns true if face is judged real.
    bool IsReal(const dlib::matrix<dlib::rgb_pixel>& faceChip, float threshold = 0.3f);

    bool IsInitialized() const { return m_initialized; }

    // Enable/disable low-light brightness normalization for dark face chips.
    void SetLowLightEnhance(bool enable) { m_lowLightEnhance = enable; }

private:
    std::unique_ptr<Ort::Env> m_env;
    std::unique_ptr<Ort::Session> m_session;
    std::unique_ptr<Ort::MemoryInfo> m_memoryInfo;
    bool m_initialized = false;
    bool m_lowLightEnhance = false;

    std::string m_inputName;
    std::string m_outputName;
    std::vector<std::string> m_outputNames; // DeepPixBiS has 2 outputs
    int m_inputSize = 224;
};

} // namespace facelogin
