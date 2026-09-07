#pragma once

#include <dlib/image_processing.h>
#include <dlib/matrix.h>
#include <dlib/pixel.h>
#include <string>

namespace facelogin {

// Photometric processing is deliberately separate from face recognition.  A
// camera may not expose any reliable manual controls, but recognition should
// still receive the same bounded, per-frame brightness domain.
enum class PhotometricMode {
    Hybrid,
    SoftwareOnly,
    Off,
};

PhotometricMode PhotometricModeFromString(const std::string& value);
std::string PhotometricModeToString(PhotometricMode mode);

struct PhotometricConfig {
    PhotometricMode mode = PhotometricMode::Hybrid;
    float targetLuma = 110.0f;
    float toleranceBand = 15.0f;
    float minDigitalGain = 0.5f;
    float maxDigitalGain = 2.0f;
    int hardwareStepIntervalMs = 500;
};

struct FacePhotometricStats {
    bool valid = false;
    float meanLuma = 0.0f;
    float medianLuma = 0.0f;
    float p10Luma = 0.0f;
    float p90Luma = 0.0f;
    float clippedRatio = 0.0f;
    float shadowRatio = 0.0f;
    float leftRightDelta = 0.0f;
    float uniformity = 0.0f;
    float redMean = 0.0f;
    float greenMean = 0.0f;
    float blueMean = 0.0f;
};

struct FramePhotometricTransform {
    float gain = 1.0f;
    float gamma = 1.0f;
    bool applied = false;
    bool unrecoverable = false;
};

enum class HardwareControlState {
    Disabled,
    Probing,
    Active,
    SoftwareOnly,
    Unresponsive,
    Reversed,
    Restored,
};

// Shared result passed between detection, photometric processing, recognition,
// liveness and enrollment.  The two applications use the same object shape so
// a frame cannot silently take a different brightness path in one of them.
struct UnifiedFaceFrame {
    dlib::matrix<dlib::rgb_pixel> rawFrame;
    dlib::matrix<dlib::rgb_pixel> normalizedFrame;
    dlib::full_object_detection landmarks;
    dlib::rectangle faceRect;
    FacePhotometricStats stats;
    FramePhotometricTransform transform;
    bool faceDetected = false;
    bool qualityAccepted = false;
};

} // namespace facelogin
