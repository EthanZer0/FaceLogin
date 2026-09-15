#pragma once

#include <windows.h>
#include <wincodec.h>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>

#include "landmark_detector.h"
#include "liveness_detector.h"
#include "liveness_types.h"
#include "onnx_models.h"
#include "webcam_capture.h"
#include "webcam_capture_dshow.h"
#include "../common/photometric_pipeline.h"
#include "pipe_server.h"
#include "credential_store.h"
#include "../common/config_util.h"

namespace facelogin {

// Which camera backend serves the current auth session. Unifying on the
// Media Foundation pipeline matters: enrollment (console) and unlock (service)
// must capture through the SAME pipeline, or the DS/MF colour & gain
// difference shifts same-person embeddings by ~0.4 (observed 0.66-0.75 vs the
// 0.75 threshold), which is the systematic offset behind cross-environment
// recognition failures. Service mode therefore prefers MF and falls back to
// DirectShow only when MF cannot initialize in Session 0.
enum class CameraPipeline { None, MF, DS };
enum class ModelLoadState { NotLoaded, Loading, Ready, Failed, Stopping };

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
//   Webcam -> SCRFD/106-point landmarks -> unified photometric frame
//   -> 512-D ONNX embedding -> DB match -> liveness -> credentials

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
    enum class ServiceEventType {
        KernelBootRefresh,
        SessionLogoff,
        DesktopReady
    };

    struct ServiceEvent {
        ServiceEventType type;
        DWORD sessionId;
    };

    void Run();          // Main service loop
    void Stop();
    void CleanupSessionResources();
    bool Initialize();   // Load DB, config, lightweight SCRFD; queue heavy models
    bool HandleAuthRequest();
    bool ProcessAuthRequest();  // Handle one auth session
    bool SendAuthTerminal(const std::wstring& message);
    void ProcessKernelBootEvidence(const wchar_t* reason);
    void QueueServiceEvent(ServiceEventType type, DWORD sessionId);
    void ProcessPendingServiceEvents();

    // Camera lifecycle for one auth session: pick the backend (service mode:
    // MF preferred, DS fallback; standalone: MF) and release it afterwards.
    bool EnsureCameraForAuth();
    void ReleaseCamera();
    // Attach the unified photometric session to the active camera (called
    // after every camera (re)init).
    void AttachPhotometricSession();
    bool GrabAuthFrame(dlib::matrix<dlib::rgb_pixel>& frame);
    bool PrepareAuthFaceFrame(dlib::matrix<dlib::rgb_pixel>& frame,
                              dlib::rectangle& faceRect,
                              dlib::full_object_detection& landmarks,
                              HeadPoseStats* pose = nullptr);
    static const wchar_t* PoseStatusKey(
        const HeadPoseStats& pose,
        const HeadPoseEvaluation& evaluation);
    enum class AuthFrameResult { NoFrame, Invalid, Rejected, Accepted };
    AuthFrameResult AcquireLegalPoseFrame(
        dlib::matrix<dlib::rgb_pixel>& frame,
        dlib::rectangle& faceRect,
        dlib::full_object_detection& landmarks,
        HeadPoseStats& pose,
        const std::function<void(const wchar_t*)>& publishStatus);

    // Lazy model loading
    void StartBackgroundModelLoad();   // spawn the async loader thread
    bool EnsureModelsLoaded();         // block until heavy models are ready
    bool LoadHeavyModels();             // landmark + recognizer + anti-spoof
    void UnloadHeavyModels();           // release model memory after auth
    void TrimWorkingSet();              // empty process working set after unload
    void ValidateLivenessMethod();     // anti-spoof → blink fallback (main thread only)
    void AbortModelLoadWait();         // release anyone blocked in EnsureModelsLoaded

    // Configuration
    std::wstring GetModelsDir();
    float GetMatchThreshold();

    // Unknown-face capture (config-gated, opt-in): when a face is detected but
    // matches no enrolled user, save a full-res JPEG of that frame plus a
    // JSONL event under <dataDir>\data\unknown\ (rolling 100 max).
    void SaveUnknownFace(const dlib::matrix<dlib::rgb_pixel>& frame,
                         float bestDistance = -1.0f);
    bool EnsureWicFactory();   // lazy CoCreateInstance for JPEG encoding

    // Service state
    SERVICE_STATUS_HANDLE m_hStatus = nullptr;
    SERVICE_STATUS m_status = {};
    std::atomic<bool> m_stopRequested{false};
    std::mutex m_serviceEventMutex;
    std::vector<ServiceEvent> m_pendingServiceEvents;
    bool m_terminalSentForCurrentRequest = false;
    static FaceService* s_pInstance;

    // Components
    std::unique_ptr<PipeServer> m_pipeServer;
    std::unique_ptr<OnnxLandmarkDetector> m_detector;  // 106-point landmarks (2d106det)
    std::unique_ptr<OnnxDetector> m_onnxDetector;       // SCRFD (face detection)
    std::unique_ptr<OnnxHeadPose> m_headPose;           // MobileNetV2 6D pose (observer)
    std::unique_ptr<OnnxRecognizer> m_onnxRecognizer;   // InsightFace (recognition)
    std::unique_ptr<OnnxAntiSpoof>  m_antiSpoof;        // MiniFASNetV2 (optional)
    std::unique_ptr<WebcamCapture>   m_webcamMF;   // Media Foundation (standalone / service MF-first)
    std::unique_ptr<WebcamCaptureDS> m_webcamDS;   // DirectShow (service fallback)
    CameraPipeline m_cameraPipeline = CameraPipeline::None;  // active backend
    // Shared photometric session. Its COM adapter owns its interface refs, so
    // it can be ended after frame processing has stopped without dangling
    // borrowed pointers.
    PhotometricSession m_photometric;
    std::unique_ptr<CredentialStore> m_store;

    // Configuration
    AppConfig m_config;
    LivenessMethod m_livenessMethod = LivenessMethod::Blink;
    float m_antiSpoofThreshold = 0.30f;

    bool m_isServiceMode = false;  // set by ServiceMain

    // Settings
    std::wstring m_dataDir;
    std::wstring m_modelsDir;
    float m_matchThreshold = 0.30f;
    int m_authTimeoutSeconds = 15;
    // WIC imaging factory for unknown-face JPEG encoding (lazy, SYSTEM session).
    IWICImagingFactory* m_wicFactory = nullptr;

    // Heavy ONNX sessions load in the background so the pipe listener becomes
    // available immediately. One state value expresses the complete lifecycle.
    std::atomic<ModelLoadState> m_modelState{ModelLoadState::NotLoaded};
    std::thread m_modelLoadThread;
    std::mutex m_modelMutex;                     // guards the model pointers
    std::condition_variable m_modelCv;           // signaled when ready/failed/abort
};

} // namespace facelogin
