#pragma once

#include "../face_service/liveness_types.h"
#include <string>

namespace facelogin {

struct AppConfig {
    std::string    recognition_model      = "both";      // "dlib" / "onnx" / "both"
    std::string    detector               = "scrfd";     // "dlib_hog" / "scrfd"
    LivenessMethod liveness_method        = LivenessMethod::Blink;
    float          match_threshold        = 0.30f;
    float          anti_spoof_threshold   = 0.50f;       // DeepPixBiS pixel map threshold
};

AppConfig LoadConfig(const std::wstring& dataDir);
bool      SaveConfig(const std::wstring& dataDir, const AppConfig& cfg);
AppConfig DefaultConfig();

// Serialization
std::string ConfigToJson(const AppConfig& cfg);
AppConfig   ConfigFromJson(const std::string& json);

// LivenessMethod helpers
std::string    LivenessMethodToString(LivenessMethod m);
LivenessMethod LivenessMethodFromString(const std::string& s);

} // namespace facelogin
