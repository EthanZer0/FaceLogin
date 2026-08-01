#pragma once

#include "../face_service/liveness_types.h"
#include <string>

namespace facelogin {

struct AppConfig {
    // dlib recognizer/detector were removed — the system is pure ONNX.
    // recognition_model and detector are retained for config.json backwards
    // compatibility but ignored at runtime (only "onnx"/"scrfd" are valid).
    std::string    recognition_model      = "onnx";      // retained for compat
    std::string    detector               = "scrfd";     // retained for compat
    LivenessMethod liveness_method        = LivenessMethod::Blink;
    float          match_threshold        = 0.30f;
    float          anti_spoof_threshold   = 0.30f;       // DeepPixBiS pixel map threshold
    std::string    camera_device          = "";          // device symbolic link; empty = first camera
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
