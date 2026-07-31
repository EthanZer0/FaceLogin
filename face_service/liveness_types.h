#pragma once

namespace facelogin {

enum class LivenessMethod {
    Blink,      // EAR-based blink detection
    AntiSpoof,  // ONNX silent anti-spoof (MiniFASNetV2)
    None        // No liveness check (insecure)
};

} // namespace facelogin
