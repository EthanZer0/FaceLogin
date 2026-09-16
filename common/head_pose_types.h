#pragma once

#include <cmath>
#include <cstdint>

namespace facelogin {

enum class HeadPoseQuality { Invalid, Valid };
enum class HeadPoseRange { Invalid, Normal, Wide };
enum class HeadPoseLegality { Invalid, Front, Acceptable, AdjustRequired, Severe };

enum class HeadPoseViolation : uint32_t {
    None = 0, YawLeft = 1u << 0, YawRight = 1u << 1,
    PitchUp = 1u << 2, PitchDown = 1u << 3,
    RollLeft = 1u << 4, RollRight = 1u << 5,
};

inline constexpr uint32_t PoseViolationBit(HeadPoseViolation value) {
    return static_cast<uint32_t>(value);
}

struct HeadPoseStats {
    bool valid = false;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    float inferenceMs = 0.0f;
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

inline HeadPoseEvaluation EvaluateHeadPose(const HeadPoseStats& pose) {
    HeadPoseEvaluation result;
    if (!pose.valid || !std::isfinite(pose.yaw) ||
        !std::isfinite(pose.pitch) || !std::isfinite(pose.roll)) return result;

    const float yaw = std::abs(pose.yaw);
    const float pitch = std::abs(pose.pitch);
    const float roll = std::abs(pose.roll);
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
    if (pose.yaw > 25.0f) result.violations |= PoseViolationBit(HeadPoseViolation::YawLeft);
    else if (pose.yaw < -25.0f) result.violations |= PoseViolationBit(HeadPoseViolation::YawRight);
    if (pose.pitch > 15.0f) result.violations |= PoseViolationBit(HeadPoseViolation::PitchDown);
    else if (pose.pitch < -15.0f) result.violations |= PoseViolationBit(HeadPoseViolation::PitchUp);
    if (pose.roll > 18.0f) result.violations |= PoseViolationBit(HeadPoseViolation::RollRight);
    else if (pose.roll < -18.0f) result.violations |= PoseViolationBit(HeadPoseViolation::RollLeft);
    result.legality = yaw > 30.0f || pitch > 25.0f || roll > 25.0f
        ? HeadPoseLegality::Severe : HeadPoseLegality::AdjustRequired;
    return result;
}

} // namespace facelogin
