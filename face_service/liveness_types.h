#pragma once

namespace facelogin {

enum class LivenessMethod {
    Blink,      // EAR-based blink detection
    AntiSpoof,  // ONNX silent anti-spoof (MiniFASNetV2)
    None        // No liveness check (insecure)
};

// Anti-spoof check is run N times, where N scales with strictness:
// threshold 0.25 (least strict) → 1 check, 0.75 (most strict) → 5 checks,
// linearly interpolated in between. Pass requires >= half of the checks.
inline int AntiSpoofCheckCount(float antiSpoofThreshold) {
    constexpr float kMinThr = 0.25f, kMaxThr = 0.75f;
    constexpr int kMinChecks = 1, kMaxChecks = 5;
    float clamped = antiSpoofThreshold < kMinThr ? kMinThr :
                    antiSpoofThreshold > kMaxThr ? kMaxThr : antiSpoofThreshold;
    float t = (clamped - kMinThr) / (kMaxThr - kMinThr);   // [0,1]
    return kMinChecks + static_cast<int>(t * (kMaxChecks - kMinChecks) + 0.5f);
}

// Checks required to pass out of AntiSpoofCheckCount(): at least half.
inline int AntiSpoofPassRequired(int checkCount) {
    return (checkCount + 1) / 2;  // ceil(N/2)
}

// Per-frame anti-spoof score threshold for the CURRENT model.
// OULU/DeepPixBiS score = pixel-map mean in [0,1], so the config slider
// (0.25–0.75) is used as-is.
// facenox MiniFAS score = real_logit − spoof_logit, a different scale: real
// faces measure ≈ +1.3..+11 (commonly +4..+5), screen replays < 0. A bare
// 0.25–0.75 config would be meaningless here, so map the slider onto the
// facenox score range, ANCHORED so the default (0.30) keeps the historical
// hardcoded threshold of 1.0:  eff = 1.0 + (config − 0.30)·4
//   lenient 0.25 → 0.80   default 0.30 → 1.00   strict 0.75 → 2.80
// Strict 2.80 still passes real faces (≥ +1.3) while tightening the near-zero
// spoof boundary; the slider becomes genuinely effective in facenox mode.
inline float AntiSpoofEffectiveThreshold(float configThreshold, bool facenoxMode) {
    if (!facenoxMode) return configThreshold;
    float t = configThreshold < 0.25f ? 0.25f : configThreshold > 0.75f ? 0.75f : configThreshold;
    return 1.0f + (t - 0.30f) * 4.0f;
}

} // namespace facelogin
