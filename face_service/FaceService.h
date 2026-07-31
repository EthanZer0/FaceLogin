#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <atomic>

#include "face_detector.h"
#include "face_recognizer.h"
#include "liveness_detector.h"
#include "liveness_types.h"
#include "onnx_models.h"
#include "webcam_capture.h"
#include "webcam_capture_dshow.h"
#include "pipe_server.h"
#include "credential_store.h"
#include "../common/config_util.h"

namespace facelogin {

// Windows service implementing the face recognition pipeline.
// Runs as LOCAL SYSTEM (required for DPAPI machine-scope decryption
// and credential provider IPC).
//
// Lifecycle:
//   1. ServiceMain called by SCM
//   2. HandlerEx handles start/stop/pause
//   3. Run() is the main loop: accept pipe connections, process auth requests
//
// The face recognition pipeline:
//   Webcam -> HOG face detection -> 68-point landmarks -> 128-D embedding
//   -> Match against stored DB -> EAR blink liveness check -> Send credentials

class FaceService {
public:
    FaceService();
    ~FaceService();

    // Service entry points
    static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
    static DWORD WINAPI HandlerEx(DWORD control, DWORD eventType,
                                   LPVOID eventData, LPVOID context);

    // Run standalone (foreground, for testing) — no SCM registration
    static void RunStandalone();

    // Install/uninstall the service
    static bool Install(const std::wstring& exePath);
    static bool Uninstall();

private:
    void Run();          // Main service loop
    void Stop();
    bool Initialize();   // Load models, DB, camera
    bool ProcessAuthRequest();  // Handle one auth session

    // Configuration
    std::wstring GetModelsDir();
    float GetMatchThreshold();

    // Service state
    SERVICE_STATUS_HANDLE m_hStatus = nullptr;
    SERVICE_STATUS m_status = {};
    std::atomic<bool> m_running{false};
    static FaceService* s_pInstance;

    // Components
    std::unique_ptr<PipeServer> m_pipeServer;
    std::unique_ptr<FaceDetector> m_detector;           // dlib HOG (fallback)
    std::unique_ptr<OnnxDetector> m_onnxDetector;       // SCRFD (primary)
    std::unique_ptr<FaceRecognizer> m_recognizer;       // dlib fallback
    std::unique_ptr<OnnxRecognizer> m_onnxRecognizer; // InsightFace (primary)
    std::unique_ptr<OnnxAntiSpoof>  m_antiSpoof;       // MiniFASNetV2 (optional)
    std::unique_ptr<WebcamCapture>   m_webcamMF;   // Media Foundation (standalone)
    std::unique_ptr<WebcamCaptureDS> m_webcamDS;   // DirectShow (service mode)
    std::unique_ptr<CredentialStore> m_store;

    // Configuration
    AppConfig m_config;
    LivenessMethod m_livenessMethod = LivenessMethod::Blink;
    float m_antiSpoofThreshold = 0.50f;

    bool m_isServiceMode = false;  // set by ServiceMain

    // Settings
    std::wstring m_dataDir;
    std::wstring m_modelsDir;
    float m_matchThreshold = 0.55f;
    int m_authTimeoutSeconds = 15;
};

} // namespace facelogin
