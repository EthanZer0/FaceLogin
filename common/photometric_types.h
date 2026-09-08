#pragma once

#include <dlib/image_processing.h>
#include <dlib/matrix.h>
#include <dlib/pixel.h>
#include <cmath>
#include <cstdint>
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

enum class HeadPoseQuality {
    Invalid,
    Valid,
};

enum class HeadPoseRange {
    Invalid,
    Normal, // Approximate yaw is within the reliable <=45 degree range.
    Wide,   // Large-angle estimate; direction is useful, exact value is not.
};

enum class HeadPoseLegality {
    Invalid,
    Front,
    Acceptable,
    AdjustRequired,
    Severe,
};

enum class HeadPoseViolation : uint32_t {
    None      = 0,
    YawLeft   = 1u << 0,
    YawRight  = 1u << 1,
    PitchUp   = 1u << 2,
    PitchDown = 1u << 3,
    RollLeft  = 1u << 4,
    RollRight = 1u << 5,
};

inline constexpr uint32_t PoseViolationBit(HeadPoseViolation value) {
    return static_cast<uint32_t>(value);
}

// Appearance-based pose shared by Console and service. Signs are
// subject-centric: subject-right yaw is positive, looking up is positive, and
// subject-left roll is positive.
struct HeadPoseStats {
    bool valid = false;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    float inferenceMs = 0.0f;
    // Diagnostics for the detector-to-pose crop. They are intentionally
    // separate from the angle result so we can distinguish model error from
    // an extreme side-face input.
    float faceWidth = 0.0f;
    float faceHeight = 0.0f;
    float faceAspect = 0.0f;
    float cropWidth = 0.0f;
    float cropHeight = 0.0f;
    float cropAspect = 0.0f;
    HeadPoseQuality quality = HeadPoseQuality::Invalid;
    HeadPoseRange range = HeadPoseRange::Invalid;
};

struct HeadPoseEvaluation {
    HeadPoseLegality legality = HeadPoseLegality::Invalid;
    uint32_t violations = PoseViolationBit(HeadPoseViolation::None);
    bool accepted = false;
    bool front = false;
};

// The pose model is reliable enough for a bounded quality gate, not for an
// exact large-angle measurement. Keep all policy thresholds in one shared
// evaluator so the service, Console and enrollment cannot drift apart.
inline HeadPoseEvaluation EvaluateHeadPose(const HeadPoseStats& pose) {
    HeadPoseEvaluation result;
    if (!pose.valid || !std::isfinite(pose.yaw) ||
        !std::isfinite(pose.pitch) || !std::isfinite(pose.roll)) {
        return result;
    }

    const float yaw = std::abs(pose.yaw);
    const float pitch = std::abs(pose.pitch);
    const float roll = std::abs(pose.roll);

    // Pitch is more sensitive for the recognizer than the current yaw/roll
    // limits, so keep a tighter frontal and accepted band while preserving
    // the existing severe-pose boundary for user guidance.
    if (yaw <= 15.0f && pitch <= 10.0f && roll <= 12.0f) {
        result.legality = HeadPoseLegality::Front;
        result.accepted = true;
        result.front = true;
        return result;
    }

    if (yaw <= 25.0f && pitch <= 15.0f && roll <= 18.0f) {
        result.legality = HeadPoseLegality::Acceptable;
        result.accepted = true;
        return result;
    }

    if (pose.yaw > 25.0f) {
        result.violations |= PoseViolationBit(HeadPoseViolation::YawLeft);
    } else if (pose.yaw < -25.0f) {
        result.violations |= PoseViolationBit(HeadPoseViolation::YawRight);
    }
    if (pose.pitch > 15.0f) {
        result.violations |= PoseViolationBit(HeadPoseViolation::PitchDown);
    } else if (pose.pitch < -15.0f) {
        result.violations |= PoseViolationBit(HeadPoseViolation::PitchUp);
    }
    if (pose.roll > 18.0f) {
        result.violations |= PoseViolationBit(HeadPoseViolation::RollRight);
    } else if (pose.roll < -18.0f) {
        result.violations |= PoseViolationBit(HeadPoseViolation::RollLeft);
    }

    const bool severe = yaw > 30.0f || pitch > 25.0f || roll > 25.0f;
    result.legality = severe ? HeadPoseLegality::Severe
                             : HeadPoseLegality::AdjustRequired;
    return result;
}

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
    HeadPoseStats pose;
    bool faceDetected = false;
    bool qualityAccepted = false;
};

} // namespace facelogin
