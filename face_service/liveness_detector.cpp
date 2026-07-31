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

bool LivenessDetector::ProcessFrame(const dlib::full_object_detection& landmarks) {
    if (m_blinkDetected) {
        // One-shot: return true only once so the debug UI sees the exact
        // frame where the blink was confirmed.  HasBlinked() stays true.
        if (m_blinkReported) return false;
        m_blinkReported = true;
        return true;
    }

    float leftEAR = ComputeLeftEAR(landmarks);
    float rightEAR = ComputeRightEAR(landmarks);
    float avgEAR = (leftEAR + rightEAR) / 2.0f;

    // Debounced blink detection: we look for a full "close → open" cycle.
    //
    // Phase 1 — Eye closing: accumulate frames where EAR < threshold.
    //           Also allow a single above-threshold frame (noise) before
    //           resetting the close counter — this lets blinks survive
    //           momentary landmark jitter.
    // Phase 2 — Eye opening: once enough "closed" frames are accumulated,
    //           require 2 consecutive "open" frames (debounce) to declare
    //           the blink complete.

    if (avgEAR < m_earThreshold) {
        // Eye is closed or closing.
        if (m_consecutiveBelowThreshold < m_consecutiveFramesRequired) {
            m_consecutiveBelowThreshold++;
        }
        m_consecutiveOpen = 0;
    } else {
        // Eye is open.
        if (m_consecutiveBelowThreshold >= m_consecutiveFramesRequired) {
            m_consecutiveOpen++;
            if (m_consecutiveOpen >= 2) {
                m_blinkDetected = true;
            }
        } else {
            // Not enough closed frames yet.  Allow 1-2 jitter frames before
            // resetting the close counter.
            if (m_consecutiveBelowThreshold > 0) {
                m_consecutiveOpen++;
                if (m_consecutiveOpen >= 2) {
                    // Two open frames in a row before threshold met = no blink.
                    m_consecutiveBelowThreshold = 0;
                    m_consecutiveOpen = 0;
                }
            }
        }
    }

    return false;  // one-shot returned above on the reporting frame
}

void LivenessDetector::Reset() {
    m_consecutiveBelowThreshold = 0;
    m_consecutiveOpen = 0;
    m_blinkDetected = false;
    m_blinkReported = false;
}

} // namespace facelogin
