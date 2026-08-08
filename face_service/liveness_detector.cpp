#include "liveness_detector.h"
#include <algorithm>
#include <cmath>

namespace facelogin {

// 106-point eye indices (insightface 2d106det; "left/right" = subject's
// first-person, i.e. the RIGHT eye below is the eye on the image's LEFT side).
//
//   Right eye (first-person): outer corner 39, inner corner 35,
//     upper lid 41-40-42, lower lid 36-33-37
//   Left eye  (first-person): outer corner 93, inner corner 89,
//     upper lid 96-94-95, lower lid 91-87-90
//
// EAR uses (sum of upper-lid distances to lower-lid) / (2 * eye width). With
// three upper + three lower lid points we average the three vertical gaps.
static const int RIGHT_EYE_UPPER[3] = {41, 40, 42};
static const int RIGHT_EYE_LOWER[3] = {36, 33, 37};
static const int RIGHT_EYE_INNER = 35;   // toward the nose
static const int RIGHT_EYE_OUTER = 39;   // away from the nose

static const int LEFT_EYE_UPPER[3]  = {96, 94, 95};
static const int LEFT_EYE_LOWER[3]  = {91, 87, 90};
static const int LEFT_EYE_INNER  = 89;  // toward the nose
static const int LEFT_EYE_OUTER  = 93;  // away from the nose

// Compute EAR for one eye from its 3 upper + 3 lower lid points.
float LivenessDetector::ComputeEyeEAR(const dlib::full_object_detection& landmarks,
                                      const int upper[3], const int lower[3],
                                      int innerIdx, int outerIdx) {
    // Eye width = distance between inner and outer corners.
    auto& pi = landmarks.part(innerIdx);
    auto& po = landmarks.part(outerIdx);
    float eyeW = std::sqrt(std::pow(po.x() - pi.x(), 2.0f) +
                           std::pow(po.y() - pi.y(), 2.0f));
    if (eyeW < 1e-6f) return 0.0f;

    // Average the three upper→lower vertical distances.
    float vertSum = 0.0f;
    for (int i = 0; i < 3; i++) {
        auto& u = landmarks.part(upper[i]);
        auto& l = landmarks.part(lower[i]);
        vertSum += std::sqrt(std::pow(u.x() - l.x(), 2.0f) +
                             std::pow(u.y() - l.y(), 2.0f));
    }
    return (vertSum / 3.0f) / (2.0f * eyeW);
}

float LivenessDetector::ComputeLeftEAR(const dlib::full_object_detection& landmarks) {
    // First-person LEFT eye → the eye on the image's RIGHT side.
    return ComputeEyeEAR(landmarks, LEFT_EYE_UPPER, LEFT_EYE_LOWER,
                         LEFT_EYE_INNER, LEFT_EYE_OUTER);
}

float LivenessDetector::ComputeRightEAR(const dlib::full_object_detection& landmarks) {
    // First-person RIGHT eye → the eye on the image's LEFT side.
    return ComputeEyeEAR(landmarks, RIGHT_EYE_UPPER, RIGHT_EYE_LOWER,
                         RIGHT_EYE_INNER, RIGHT_EYE_OUTER);
}

float LivenessDetector::Median(std::vector<float>& v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n == 0) return 0.0f;
    if (n % 2 == 1) return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) * 0.5f;
}

// Head-pose proxy: vertical distance from the nose bridge (point 80, the
// nose-tip end of the 106-point nose bridge 72-73-74-86-80) to the eye line
// (outer corners 39 and 93, first-person right/left = image left/right),
// normalized by the eye distance.
//   pose = |cross((p93-p39), (p80-p39))| / |p93-p39|^2
// Scale-invariant. A blink moves the nose ~zero; tilting the head moves it
// substantially, so it discriminates real blinks from head-motion EAR dips.
float LivenessDetector::ComputePose(const dlib::full_object_detection& landmarks) {
    if (landmarks.num_parts() < 106) return 0.0f;

    auto& pL = landmarks.part(39);  // right-eye outer corner (image left)
    auto& pR = landmarks.part(93);  // left-eye outer corner (image right)
    auto& pN = landmarks.part(80);  // nose bridge end (nose tip)

    float ex = pR.x() - pL.x();
    float ey = pR.y() - pL.y();
    float eyeDist2 = ex * ex + ey * ey;
    if (eyeDist2 < 1e-6f) return 0.0f;

    float nx = pN.x() - pL.x();
    float ny = pN.y() - pL.y();
    // Cross product magnitude / eyeDist2 = perpendicular distance from the
    // nose to the eye line, in units of eye distance.
    float cross = ex * ny - ey * nx;
    return cross / eyeDist2;  // signed: + = nose below eye line (normal)
}

void LivenessDetector::Reset() {
    m_leftSamples.clear();
    m_rightSamples.clear();
    m_poseSamples.clear();
    m_baselinePose = -1.0f;
    m_baselineReady = false;
    m_leftThreshold  = m_fixedEarThreshold;
    m_rightThreshold = m_fixedEarThreshold;
    m_below = m_open = 0;
    m_leftBelow = m_leftOpen = 0;
    m_rightBelow = m_rightOpen = 0;
    m_blinkDetected = false;
    m_blinkReported = false;
}

bool LivenessDetector::ProcessFrame(const dlib::full_object_detection& landmarks) {
    if (m_blinkDetected) {
        // One-shot: return true only once so the debug UI sees the exact
        // frame where the blink was confirmed.  HasBlinked() stays true.
        if (m_blinkReported) return false;
        m_blinkReported = true;
        return true;
    }
    return m_glassesMode ? ProcessFrameGlasses(landmarks)
                         : ProcessFrameClassic(landmarks);
}

// ============================================================================
// CLASSIC algorithm (default): averaged EAR + fixed threshold + debounced
// close→open state machine. This is the stable 1.3.0 production detector —
// no adaptive baseline, no per-eye, no pose gate.
// ============================================================================
bool LivenessDetector::ProcessFrameClassic(const dlib::full_object_detection& landmarks) {
    float leftEAR = ComputeLeftEAR(landmarks);
    float rightEAR = ComputeRightEAR(landmarks);
    float avgEAR = (leftEAR + rightEAR) / 2.0f;

    if (avgEAR < m_fixedEarThreshold) {
        // Eye is closed or closing.
        if (m_below < m_consecutiveFramesRequired) m_below++;
        m_open = 0;
    } else {
        // Eye is open.
        if (m_below >= m_consecutiveFramesRequired) {
            m_open++;
            if (m_open >= 2) m_blinkDetected = true;
        } else {
            // Not enough closed frames yet. Allow 1-2 jitter frames before
            // resetting the close counter.
            if (m_below > 0) {
                m_open++;
                if (m_open >= 2) {
                    m_below = 0;
                    m_open = 0;
                }
            }
        }
    }

    if (m_blinkDetected) {
        m_blinkReported = true;
        return true;
    }
    return false;
}

// ============================================================================
// GLASSES algorithm (opt-in): adaptive per-eye baseline + single-eye detection
// + pose-stability gate (anti head-tilt spoof).
// ============================================================================
bool LivenessDetector::ProcessFrameGlasses(const dlib::full_object_detection& landmarks) {
    float leftEAR = ComputeLeftEAR(landmarks);
    float rightEAR = ComputeRightEAR(landmarks);
    float pose = ComputePose(landmarks);

    // --- Baseline warmup (adaptive threshold + pose) ---
    if (!m_baselineReady) {
        m_leftSamples.push_back(leftEAR);
        m_rightSamples.push_back(rightEAR);
        m_poseSamples.push_back(pose);
        if (static_cast<int>(m_leftSamples.size()) >= kBaselineFrames) {
            m_leftThreshold  = Median(m_leftSamples)  * kRelativeFactor;
            m_rightThreshold = Median(m_rightSamples) * kRelativeFactor;
            m_baselinePose   = Median(m_poseSamples);
            m_baselineReady  = true;
        }
        return false;  // still sampling — do not attempt blink detection yet
    }

    // --- Pose gate ---
    bool poseStable = (std::fabs(pose - m_baselinePose) <= kMaxPoseDelta);

    bool leftClosed  = poseStable && (leftEAR  < m_leftThreshold);
    bool rightClosed = poseStable && (rightEAR < m_rightThreshold);

    // Left eye state machine.
    if (leftClosed) {
        if (m_leftBelow < m_consecutiveFramesRequired) m_leftBelow++;
        m_leftOpen = 0;
    } else {
        if (m_leftBelow >= m_consecutiveFramesRequired) {
            m_leftOpen++;
            if (m_leftOpen >= 2) m_blinkDetected = true;
        } else {
            if (m_leftBelow > 0) {
                m_leftOpen++;
                if (m_leftOpen >= 2) {
                    m_leftBelow = 0;
                    m_leftOpen = 0;
                }
            }
        }
    }

    if (m_blinkDetected) {
        m_blinkReported = true;
        return true;
    }

    // Right eye state machine (only if left hasn't fired yet).
    if (rightClosed) {
        if (m_rightBelow < m_consecutiveFramesRequired) m_rightBelow++;
        m_rightOpen = 0;
    } else {
        if (m_rightBelow >= m_consecutiveFramesRequired) {
            m_rightOpen++;
            if (m_rightOpen >= 2) m_blinkDetected = true;
        } else {
            if (m_rightBelow > 0) {
                m_rightOpen++;
                if (m_rightOpen >= 2) {
                    m_rightBelow = 0;
                    m_rightOpen = 0;
                }
            }
        }
    }

    if (m_blinkDetected) {
        m_blinkReported = true;
        return true;
    }

    return false;
}

} // namespace facelogin
