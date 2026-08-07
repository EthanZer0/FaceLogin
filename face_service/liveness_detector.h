#pragma once

#include <dlib/image_processing/full_object_detection.h>
#include <vector>

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
//
// === Glasses robustness (1.4.0) ===
//
// Fixed-threshold EAR fails for glasses wearers: frames/shadow distort the
// dlib landmarks, compressing the open/closed EAR range (a glasses user's
// open-eye EAR can sit near or below the old fixed 0.20 threshold, so the
// "close→open" cycle never registers). Two changes make it robust:
//
//   1. ADAPTIVE BASELINE THRESHOLD — the first kBaselineFrames valid frames
//      collect per-eye EAR samples; the baseline is their median (robust to a
//      blink or two during warmup) and each eye's closed-threshold becomes
//      baseline * kRelativeFactor. A glasses user whose open EAR is compressed
//      still closes to ~half of their own baseline when blinking, so a
//      relative threshold recovers detection. If warmup never fills (face
//      lost), it degrades to the fixed kDefaultEarThreshold.
//
//   2. PER-EYE DETECTION — the close→open state machine runs on EACH eye
//      independently instead of the average, so a blink caught by either eye
//      registers. Glasses often distort only one eye; the healthy eye carries
//      detection. Security is preserved: a blink still requires a full
//      close(>=kDefaultBlinkFrames)→open(>=2) cycle — a static photo never
//      completes it.
//
//   3. POSE-STABILITY GATE (anti tilt-spoof) — the adaptive threshold lowered
//      the closed-eye bar, which reintroduced a spoof: tilting the head up
//      quickly projects the eyes narrower → EAR drops → a fake "close→open"
//      cycle. A real blink moves only the EYELIDS (eye corners and nose are
//      static); a head tilt moves the NOSE relative to the eye line. So a
//      frame is only counted as "closed" if the nose-to-eye-line pose proxy
//      is close to the baseline pose captured during warmup. Large pose
//      changes (tilt/translation) are ignored as head motion, not blinks.

// Default blink-detection parameters. Shared by the auth service and the
// enrollment wizard so both use the same tuning.
//
// Why consecutiveFramesRequired = 2 (not 3): the detection loop runs at
// roughly 6 fps (grab + HOG detect + landmarks per frame), while a natural
// blink lasts only ~150ms. Requiring 3 consecutive below-threshold frames
// (~500ms of eye-closed time) meant fast blinks were easily missed — the
// close counter kept getting reset before reaching 3. With 2 frames, a
// real blink reliably registers while still tolerating landmark jitter
// (a single spurious below-threshold frame does not, by itself, count).
constexpr float kDefaultEarThreshold = 0.20f;
constexpr int   kDefaultBlinkFrames  = 2;

// Adaptive baseline: number of valid frames to sample before switching to the
// relative threshold. 12 frames ≈ 2s at the ~6fps detection loop — long enough
// for a stable median, short enough to not eat the 8s liveness timeout.
constexpr int   kBaselineFrames        = 12;
// Closed-eye threshold = baseline * this factor. A natural blink drops EAR to
// roughly half the open baseline, but for glasses wearers landmark distortion
// can keep the closed EAR as high as ~0.7× the open baseline — a stricter
// factor (0.55) forced users to blink hard to trigger. 0.70 accepts a real
// blink while still requiring a genuine drop (an open eye under jitter rarely
// falls below 0.85× baseline).
constexpr float kRelativeFactor        = 0.70f;
// Pose gate: a frame's nose-to-eye-line pose must stay within this normalized
// distance of the baseline pose to be counted as "closed". Real blinks come
// with slight head motion that moves the nose ~0.1-0.15; a deliberate head
// tilt moves it several times further. 0.15 tolerates normal motion while
// still rejecting a tilt.
constexpr float kMaxPoseDelta          = 0.15f;

class LivenessDetector {
public:
    LivenessDetector() = default;

    // Configure detection parameters.
    // earThreshold: EAR below this value = eye closed (default 0.20). Used as
    //   the fixed threshold in classic mode; in glasses mode it is the fallback
    //   while the adaptive baseline is warming up (and final if warmup fails).
    // consecutiveFramesRequired: frames where EAR must stay below threshold (default 2)
    // glassesMode: false (default) = CLASSIC algorithm — the stable 1.3.0
    //   detector (averaged EAR + fixed threshold + debounced close→open). true =
    //   GLASSES algorithm — adaptive per-eye baseline + single-eye detection +
    //   pose-stability gate (for glasses wearers; ON by choice in settings).
    void Configure(float earThreshold = kDefaultEarThreshold,
                   int consecutiveFramesRequired = kDefaultBlinkFrames,
                   bool glassesMode = false) {
        m_fixedEarThreshold = earThreshold;
        m_consecutiveFramesRequired = consecutiveFramesRequired;
        m_glassesMode = glassesMode;
        Reset();
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
    // Compute EAR for a single eye from its 3 upper + 3 lower lid points
    // and inner/outer corner indices (106-point layout).
    static float ComputeEyeEAR(const dlib::full_object_detection& landmarks,
                               const int upper[3], const int lower[3],
                               int innerIdx, int outerIdx);
    static float ComputeLeftEAR(const dlib::full_object_detection& landmarks);
    static float ComputeRightEAR(const dlib::full_object_detection& landmarks);

    // Median of a non-empty vector (robust to outlier frames).
    static float Median(std::vector<float>& v);

    // Head-pose proxy: (nose-to-eye-line vertical distance) / (eye distance).
    // Invariant under scale; rises sharply when the head tilts up/down.
    static float ComputePose(const dlib::full_object_detection& landmarks);

    // CLASSIC algorithm (default, 1.3.0 stable): averaged EAR, fixed
    // threshold, single debounced close→open state machine.
    bool ProcessFrameClassic(const dlib::full_object_detection& landmarks);

    // GLASSES algorithm (opt-in): adaptive per-eye baseline + single-eye
    // detection + pose-stability gate.
    bool ProcessFrameGlasses(const dlib::full_object_detection& landmarks);

    // Algorithm mode.
    bool m_glassesMode = false;

    // Adaptive thresholds (per eye). While baseline is warming up (or if
    // warmup failed), these hold the fixed fallback threshold.
    float m_leftThreshold  = kDefaultEarThreshold;
    float m_rightThreshold = kDefaultEarThreshold;

    float m_fixedEarThreshold = kDefaultEarThreshold;
    int m_consecutiveFramesRequired = 2;

    // Baseline warmup samples (per eye).
    std::vector<float> m_leftSamples;
    std::vector<float> m_rightSamples;
    std::vector<float> m_poseSamples;
    float m_baselinePose = -1.0f;  // <0 until warmup completes
    bool m_baselineReady = false;

    // Classic-mode state (single averaged EAR).
    int m_below = 0;   // consecutive frames "closed"
    int m_open  = 0;   // debounce counter "open"

    // Glasses-mode per-eye state.
    int m_leftBelow  = 0;   // consecutive frames left eye "closed"
    int m_leftOpen   = 0;   // debounce counter left eye "open"
    int m_rightBelow = 0;   // consecutive frames right eye "closed"
    int m_rightOpen  = 0;   // debounce counter right eye "open"

    bool m_blinkDetected = false;
    bool m_blinkReported = false;  // one-shot — only true on the frame blink completes
};

} // namespace facelogin
