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
    LivenessMethod liveness_method        = LivenessMethod::None;
    // Euclidean distance; 0.45(strict)…1.15(loose). Default 0.75 (was 0.65):
    // the 512-D model's same-person range measures 0.14–0.80, so 0.65 sat too
    // close to the high end of real-user matches under illumination changes
    // (dorm vs classroom drifted a user from ~0.3 to ~0.69 — right at 0.65,
    // causing intermittent unlock failures). 0.75 keeps strangers (>0.94)
    // comfortably rejected while covering realistic environment drift.
    float          match_threshold        = 0.75f;
    float          anti_spoof_threshold   = 0.30f;       // DeepPixBiS pixel map threshold
    // Blink-detection mode. false (default) = CLASSIC stable algorithm (averaged
    // EAR + fixed threshold). true = GLASSES mode (adaptive per-eye baseline +
    // single-eye + pose gate) — for users whose glasses destabilize classic EAR.
    bool           blink_glasses_mode     = false;
    // Low-light enhancement. false (default) = no preprocessing. true = apply
    // brightness normalization to dark face chips before recognition AND
    // anti-spoof, so matches/scores don't degrade in dark scenes.
    bool           low_light_enhance      = false;
    // Face-region exposure auto-control. false (default) = camera as-is.
    // true = a feedback loop keeps the face's brightness in
    // [face_exposure_target ± face_exposure_band] by steering the camera's
    // manual exposure/gain (when supported) and topping up with a frame-level
    // digital gain. Enrollment (console) and unlock (service) converge to the
    // same target, so anchors and probes share one brightness domain across
    // rooms — an over/under-exposed face no longer shifts the embedding.
    // NOTE: enabling this changes the input domain of enrolled embeddings —
    // re-enroll faces afterwards (same rule as the V5 alignment change).
    bool           face_exposure_control  = false;
    float          face_exposure_target   = 110.0f;   // face mean luma target
    float          face_exposure_band     = 15.0f;    // ± tolerance band
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
    // Save a photo of the frame when a face is detected but matches no
    // enrolled user (stranger / unrecognized visitor), plus a JSONL event
    // record. Stored under <dataDir>\data\unknown\ (JPEG, rolling 100 max).
    // OFF by default — captures non-user biometric data, opt-in only.
    bool           capture_unknown_faces  = false;
    // Cold-boot (power-on login screen) behavior. false (default) = start
    // face recognition immediately on boot (historical behavior). true =
    // require a key press first, like the lock-screen unlock flow — for users
    // who don't want the camera turning on automatically at boot.
    // Mirrored to HKLM\SOFTWARE\FaceLogin\ColdBootKeyTrigger on SetConfig so
    // the credential provider (LogonUI) can read it (it cannot reach the
    // config.json file reliably).
    bool           cold_boot_key_trigger  = false;
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
