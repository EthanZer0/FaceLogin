#pragma once

namespace facelogin {

enum class LivenessMethod {
    Blink,      // EAR-based blink detection
    AntiSpoof,  // ONNX silent anti-spoof (MiniFASNetV2)
    None        // No liveness check (insecure)
};

// Anti-spoof check is run N times, where N scales with strictness:
// threshold 0.25 (least strict) → 3 checks, 0.75 (most strict) → 5 checks,
// linearly interpolated in between. NEVER fewer than 3: facenox MiniFAS
// scores on real footage jitter frame-to-frame (measured ≈ 0.3..6 for the
// same user on a laptop webcam, vs the old single-frame assumption), so a
// single check is a coin flip — the pass rule below (any one frame passing)
// needs a small sample to ride out the jitter.
inline int AntiSpoofCheckCount(float antiSpoofThreshold) {
    constexpr float kMinThr = 0.25f, kMaxThr = 0.75f;
    constexpr int kMinChecks = 3, kMaxChecks = 5;
    float clamped = antiSpoofThreshold < kMinThr ? kMinThr :
                    antiSpoofThreshold > kMaxThr ? kMaxThr : antiSpoofThreshold;
    float t = (clamped - kMinThr) / (kMaxThr - kMinThr);   // [0,1]
    return kMinChecks + static_cast<int>(t * (kMaxChecks - kMinChecks) + 0.5f);
}

// Checks required to pass: ANY single frame passing is enough (max-score
// semantics). Real faces jitter around the threshold and at least one frame
// in the sample clears it; photo/screen replays score below the threshold on
// EVERY frame, so max-score does not weaken attack rejection.
inline int AntiSpoofPassRequired(int checkCount) {
    (void)checkCount;
    return 1;
}

// Per-frame anti-spoof score threshold for the CURRENT model.
// OULU/DeepPixBiS score = pixel-map mean in [0,1], so the config slider
// (0.25–0.75) is used as-is.
// facenox MiniFAS score = real_logit − spoof_logit, a different scale. The
// original calibration assumed real faces measure ≈ +4..+5, but real footage
// on laptop webcams measures ≈ 0.3..6 with heavy frame-to-frame jitter
// (measured: the same user scored −1.2, +0.5 and +3.9 on consecutive
// captures). Anchored so the default (0.30) maps to 0.50 — safely above the
// <0 replay range while clearing typical real-face scores on at least one of
// the 3 checks:  eff = 0.50 + (config − 0.30)·4
//   lenient 0.25 → 0.30   default 0.30 → 0.50   strict 0.75 → 2.30
inline float AntiSpoofEffectiveThreshold(float configThreshold, bool facenoxMode) {
    if (!facenoxMode) return configThreshold;
    float t = configThreshold < 0.25f ? 0.25f : configThreshold > 0.75f ? 0.75f : configThreshold;
    return 0.50f + (t - 0.30f) * 4.0f;
}

} // namespace facelogin
