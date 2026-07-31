#pragma once

#include <dlib/image_processing/full_object_detection.h>

namespace facelogin {

// Blink detection using Eye Aspect Ratio (EAR).
//
// EAR = (|p2-p6| + |p3-p5|) / (2 * |p1-p4|)
//
// The eye is "closed" when EAR drops below a threshold.
// A blink is detected when EAR drops below threshold for N consecutive
// frames, then rises back above threshold.
//
// dlib 68-point landmark indices for eyes:
//   Left eye:  36-41  (36=left corner, 39=right corner)
//   Right eye: 42-47  (42=left corner, 45=right corner)
//
// For each eye, the 6 points in order around the eye:
//   Left:  36(left), 37(top-left), 38(top), 39(right), 40(bottom), 41(bottom-left)
//   Right: 42(left), 43(top-left), 44(top), 45(right), 46(bottom), 47(bottom-left)

class LivenessDetector {
public:
    LivenessDetector() = default;

    // Configure detection parameters.
    // earThreshold: EAR below this value = eye closed (default 0.20)
    // consecutiveFramesRequired: frames where EAR must stay below threshold (default 3)
    void Configure(float earThreshold = 0.20f, int consecutiveFramesRequired = 3) {
        m_earThreshold = earThreshold;
        m_consecutiveFramesRequired = consecutiveFramesRequired;
        m_consecutiveBelowThreshold = 0;
        m_consecutiveOpen = 0;
        m_blinkDetected = false;
        m_blinkReported = false;
    }

    // Process one frame's landmarks. Returns true if a blink has been
    // detected in the recent frame window.
    // Call this every frame. Once it returns true, the liveness check
    // is considered passed for the current authentication session.
    bool ProcessFrame(const dlib::full_object_detection& landmarks);

    // Returns true if a blink has been detected since last Reset()
    bool HasBlinked() const { return m_blinkDetected; }

    // Reset the detector state for a new authentication session.
    void Reset();

private:
    // Compute EAR for a single eye given the 6 landmark indices.
    static float ComputeEyeEAR(const dlib::full_object_detection& landmarks,
                                const int indices[6]);
    static float ComputeLeftEAR(const dlib::full_object_detection& landmarks);
    static float ComputeRightEAR(const dlib::full_object_detection& landmarks);

    float m_earThreshold = 0.20f;
    int m_consecutiveFramesRequired = 3;
    int m_consecutiveBelowThreshold = 0;   // frames the eye has been "closed"
    int m_consecutiveOpen = 0;             // debounce counter for "open"
    bool m_blinkDetected = false;
    bool m_blinkReported = false;          // one-shot — only true on the frame blink completes
};

} // namespace facelogin
