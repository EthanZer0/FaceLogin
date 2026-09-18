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

struct HeadPoseStabilityResult {
    HeadPoseStats pose;
    HeadPoseEvaluation evaluation;
    bool pending = false;
};

class HeadPoseStabilizer {
public:
    void Reset() {
        m_hasSmoothedPose = false;
        m_stableEvaluation = {};
        m_candidateEvaluation = {};
        m_candidateFrames = 0;
    }

    HeadPoseStabilityResult Update(const HeadPoseStats& rawPose) {
        if (!rawPose.valid || rawPose.quality != HeadPoseQuality::Valid ||
            !std::isfinite(rawPose.yaw) || !std::isfinite(rawPose.pitch) ||
            !std::isfinite(rawPose.roll)) {
            m_candidateEvaluation = {};
            m_candidateFrames = 0;
            if (m_stableEvaluation.legality == HeadPoseLegality::Invalid) {
                m_stableEvaluation = {};
            }
            return {m_smoothedPose, m_stableEvaluation, false};
        }

        if (!m_hasSmoothedPose) {
            m_smoothedPose = rawPose;
            m_hasSmoothedPose = true;
        } else {
            m_smoothedPose.yaw = Smooth(m_smoothedPose.yaw, rawPose.yaw);
            m_smoothedPose.pitch = Smooth(m_smoothedPose.pitch, rawPose.pitch);
            m_smoothedPose.roll = Smooth(m_smoothedPose.roll, rawPose.roll);
            m_smoothedPose.inferenceMs = rawPose.inferenceMs;
            m_smoothedPose.faceWidth = rawPose.faceWidth;
            m_smoothedPose.faceHeight = rawPose.faceHeight;
            m_smoothedPose.faceAspect = rawPose.faceAspect;
            m_smoothedPose.cropWidth = rawPose.cropWidth;
            m_smoothedPose.cropHeight = rawPose.cropHeight;
            m_smoothedPose.cropAspect = rawPose.cropAspect;
            m_smoothedPose.valid = rawPose.valid;
            m_smoothedPose.quality = rawPose.quality;
            m_smoothedPose.range = rawPose.range;
        }

        const HeadPoseEvaluation rawEvaluation = EvaluateHeadPose(m_smoothedPose);
        const HeadPoseEvaluation desiredEvaluation =
            HeldEvaluation(rawEvaluation);
        bool pending = false;

        if (SameEvaluation(desiredEvaluation, m_stableEvaluation)) {
            m_candidateEvaluation = {};
            m_candidateFrames = 0;
        } else {
            pending = true;
            if (!SameEvaluation(desiredEvaluation, m_candidateEvaluation)) {
                m_candidateEvaluation = desiredEvaluation;
                m_candidateFrames = 1;
            } else {
                ++m_candidateFrames;
            }

            const int requiredFrames =
                desiredEvaluation.legality == HeadPoseLegality::Severe ? 2 : 3;
            if (m_candidateFrames >= requiredFrames) {
                m_stableEvaluation = desiredEvaluation;
                m_candidateEvaluation = {};
                m_candidateFrames = 0;
                pending = false;
            }
        }

        return {m_smoothedPose, m_stableEvaluation, pending};
    }

private:
    static constexpr float kSmoothingAlpha = 0.35f;

    static float Smooth(float previous, float current) {
        return previous + kSmoothingAlpha * (current - previous);
    }

    static bool SameEvaluation(const HeadPoseEvaluation& left,
                               const HeadPoseEvaluation& right) {
        return left.legality == right.legality &&
               left.violations == right.violations;
    }

    HeadPoseEvaluation HeldEvaluation(
        const HeadPoseEvaluation& rawEvaluation) const {
        if (m_stableEvaluation.legality == HeadPoseLegality::Front &&
            Within(m_smoothedPose, 17.0f, 12.0f, 14.0f)) {
            HeadPoseEvaluation held;
            held.legality = HeadPoseLegality::Front;
            held.accepted = true;
            held.front = true;
            return held;
        }

        if (m_stableEvaluation.legality == HeadPoseLegality::Acceptable &&
            Within(m_smoothedPose, 28.0f, 18.0f, 21.0f)) {
            HeadPoseEvaluation held;
            held.legality = HeadPoseLegality::Acceptable;
            held.accepted = true;
            return held;
        }

        return rawEvaluation;
    }

    static bool Within(const HeadPoseStats& pose,
                       float yaw,
                       float pitch,
                       float roll) {
        return std::abs(pose.yaw) <= yaw &&
               std::abs(pose.pitch) <= pitch &&
               std::abs(pose.roll) <= roll;
    }

    bool m_hasSmoothedPose = false;
    HeadPoseStats m_smoothedPose;
    HeadPoseEvaluation m_stableEvaluation;
    HeadPoseEvaluation m_candidateEvaluation;
    int m_candidateFrames = 0;
};

} // namespace facelogin
