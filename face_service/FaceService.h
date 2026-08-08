#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "landmark_detector.h"
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
    bool Initialize();   // Load DB, config, lightweight SCRFD; queue heavy models
    bool ProcessAuthRequest();  // Handle one auth session

    // Lazy model loading (1.5.0)
    void StartBackgroundModelLoad();   // spawn the async loader thread
    bool EnsureModelsLoaded();         // block until heavy models are ready
    bool LoadHeavyModels(bool lowLightEnhance);  // shape pred + recognizer + anti-spoof
    void UnloadHeavyModels();          // release model memory after auth (1.6.0)
    void ValidateLivenessMethod();     // anti-spoof → blink fallback (main thread only)
    void AbortModelLoadWait();         // release anyone blocked in EnsureModelsLoaded

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
    std::unique_ptr<OnnxLandmarkDetector> m_detector;  // 106-point landmarks (2d106det)
    std::unique_ptr<OnnxDetector> m_onnxDetector;       // SCRFD (face detection)
    std::unique_ptr<OnnxRecognizer> m_onnxRecognizer;   // InsightFace (recognition)
    std::unique_ptr<OnnxAntiSpoof>  m_antiSpoof;        // MiniFASNetV2 (optional)
    std::unique_ptr<WebcamCapture>   m_webcamMF;   // Media Foundation (standalone)
    std::unique_ptr<WebcamCaptureDS> m_webcamDS;   // DirectShow (service mode)
    std::unique_ptr<CredentialStore> m_store;

    // Configuration
    AppConfig m_config;
    LivenessMethod m_livenessMethod = LivenessMethod::Blink;
    float m_antiSpoofThreshold = 0.30f;

    bool m_isServiceMode = false;  // set by ServiceMain

    // Set when the system resumes from sleep/hibernate (PBT_APMRESUMESUSPEND).
    // The MF platform survives resume but the USB camera may still be in
    // low-power recovery — force a fresh camera init on the next auth so we
    // don't reuse a stale SourceReader that returns no frames.
    std::atomic<bool> m_resumedFlag{false};

    // Settings
    std::wstring m_dataDir;
    std::wstring m_modelsDir;
    float m_matchThreshold = 0.30f;
    int m_authTimeoutSeconds = 15;

    // --- Lazy model loading (1.5.0) ---
    // The 99.7MB dlib shape predictor + 3 ONNX sessions take seconds to load.
    // Loading them synchronously in Initialize() delayed the named-pipe
    // listener by that much, so the credential provider (which appears the
    // moment the lock screen shows) had to wait. Now the service starts with
    // only the lightweight SCRFD detector loaded and immediately begins
    // listening; the heavy models load in a background thread. If a request
    // arrives before they finish, EnsureModelsLoaded() blocks until ready.
    std::atomic<bool> m_modelsReady{false};      // heavy models loaded OK
    std::atomic<bool> m_modelsFailed{false};     // heavy models failed to load
    std::atomic<bool> m_modelsLoading{false};    // loader in flight (or done)
    std::atomic<bool> m_modelsAbort{false};      // service stopping — release waiters
    std::thread m_modelLoadThread;
    std::mutex m_modelMutex;                     // guards the model pointers
    std::condition_variable m_modelCv;           // signaled when ready/failed/abort
};

} // namespace facelogin
