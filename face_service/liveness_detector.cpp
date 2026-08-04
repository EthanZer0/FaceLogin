#include "liveness_detector.h"
#include <algorithm>
#include <cmath>

namespace facelogin {

static const int LEFT_EYE_IDX[6]  = {36, 37, 38, 39, 40, 41};
static const int RIGHT_EYE_IDX[6] = {42, 43, 44, 45, 46, 47};

float LivenessDetector::ComputeEyeEAR(const dlib::full_object_detection& landmarks,
                                       const int indices[6]) {
    auto& p0 = landmarks.part(indices[0]);
    auto& p1 = landmarks.part(indices[1]);
    auto& p2 = landmarks.part(indices[2]);
    auto& p3 = landmarks.part(indices[3]);
    auto& p4 = landmarks.part(indices[4]);
    auto& p5 = landmarks.part(indices[5]);

    float horizDist = std::sqrt(
        std::pow(p3.x() - p0.x(), 2.0f) +
        std::pow(p3.y() - p0.y(), 2.0f));

    float vertDist1 = std::sqrt(
        std::pow(p1.x() - p5.x(), 2.0f) +
        std::pow(p1.y() - p5.y(), 2.0f));

    float vertDist2 = std::sqrt(
        std::pow(p2.x() - p4.x(), 2.0f) +
        std::pow(p2.y() - p4.y(), 2.0f));

    if (horizDist < 1e-6f) return 0.0f;

    return (vertDist1 + vertDist2) / (2.0f * horizDist);
}

float LivenessDetector::ComputeLeftEAR(const dlib::full_object_detection& landmarks) {
    return ComputeEyeEAR(landmarks, LEFT_EYE_IDX);
}

float LivenessDetector::ComputeRightEAR(const dlib::full_object_detection& landmarks) {
    return ComputeEyeEAR(landmarks, RIGHT_EYE_IDX);
}

float LivenessDetector::Median(std::vector<float>& v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n == 0) return 0.0f;
    if (n % 2 == 1) return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) * 0.5f;
}

// Head-pose proxy: vertical distance from the nose tip (point 33) to the
// eye line (left outer 36 → right outer 45), normalized by the eye distance.
//   pose = |cross((p45-p36), (p33-p36))| / |p45-p36|^2
// Scale-invariant. A blink moves the nose ~zero; tilting the head moves it
// substantially, so it discriminates real blinks from head-motion EAR dips.
float LivenessDetector::ComputePose(const dlib::full_object_detection& landmarks) {
    if (landmarks.num_parts() < 46) return 0.0f;

    auto& p36 = landmarks.part(36);  // left outer eye corner
    auto& p45 = landmarks.part(45);  // right outer eye corner
    auto& p33 = landmarks.part(33);  // nose tip

    float ex = p45.x() - p36.x();
    float ey = p45.y() - p36.y();
    float eyeDist2 = ex * ex + ey * ey;
    if (eyeDist2 < 1e-6f) return 0.0f;

    float nx = p33.x() - p36.x();
    float ny = p33.y() - p36.y();
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
