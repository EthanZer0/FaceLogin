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
    float          match_threshold        = 0.65f;        // Euclidean distance; 0.45(strict)…1.15(loose)
    float          anti_spoof_threshold   = 0.30f;       // DeepPixBiS pixel map threshold
    // Blink-detection mode. false (default) = CLASSIC stable algorithm (averaged
    // EAR + fixed threshold). true = GLASSES mode (adaptive per-eye baseline +
    // single-eye + pose gate) — for users whose glasses destabilize classic EAR.
    bool           blink_glasses_mode     = false;
    // Low-light enhancement. false (default) = no preprocessing. true = apply
    // brightness normalization to dark face chips before recognition AND
    // anti-spoof, so matches/scores don't degrade in dark scenes.
    bool           low_light_enhance      = false;
    // Release heavy model sessions after an auth completes (2d106det +
    // recognizer + anti-spoof), dropping idle RSS from ~77MB to ~44MB. The
    // next auth reloads them synchronously (~200-500ms). Off (default) keeps
    // models resident for the fastest auth; on trades a small per-auth reload
    // latency for a low idle footprint.
    bool           unload_models_after_auth = false;
    std::string    camera_device          = "";          // device symbolic link; empty = first camera
    // Camera rotation in degrees clockwise. Valid: 0, 90, 180, 270.
    // Use when the camera is physically mounted in a non-standard
    // orientation (e.g., vertical PC mount / sideways webcam).
    int            camera_rotation        = 0;
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
