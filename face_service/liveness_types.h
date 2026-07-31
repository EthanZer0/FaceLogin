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

} // namespace facelogin
