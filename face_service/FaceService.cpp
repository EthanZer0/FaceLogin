#include "FaceService.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include "../common/secure_buffer.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include "../common/image_utils.h"
#include "liveness_detector.h"
#include <shlobj.h>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cmath>
#include <wtsapi32.h>

#pragma comment(lib, "wtsapi32.lib")

namespace facelogin {

FaceService* FaceService::s_pInstance = nullptr;

static constexpr wchar_t SERVICE_NAME[] = L"FaceLoginService";

// UTF-8 → wide string, for passing config.camera_device to the camera backends.
static std::wstring Utf8ToWstr(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

FaceService::FaceService() {
    s_pInstance = this;
}

FaceService::~FaceService() {
    s_pInstance = nullptr;
}

void WINAPI FaceService::ServiceMain(DWORD argc, LPWSTR* argv) {
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    FaceService service;
    service.m_isServiceMode = true;

    service.m_hStatus = RegisterServiceCtrlHandlerExW(
        SERVICE_NAME, HandlerEx, &service);

    if (!service.m_hStatus) {
        FACELOGIN_ERROR(L"RegisterServiceCtrlHandler failed: %lu", GetLastError());
        return;
    }

    service.m_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    service.m_status.dwCurrentState = SERVICE_START_PENDING;
    service.m_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
        | SERVICE_ACCEPT_SESSIONCHANGE | SERVICE_ACCEPT_POWEREVENT;
    service.m_status.dwWin32ExitCode = NO_ERROR;
    service.m_status.dwServiceSpecificExitCode = 0;
    service.m_status.dwCheckPoint = 0;
    service.m_status.dwWaitHint = 10000;
    SetServiceStatus(service.m_hStatus, &service.m_status);

    if (!service.Initialize()) {
        service.m_status.dwCurrentState = SERVICE_STOPPED;
        service.m_status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        service.m_status.dwServiceSpecificExitCode = 1;
        SetServiceStatus(service.m_hStatus, &service.m_status);
        return;
    }

    service.m_status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(service.m_hStatus, &service.m_status);

    FACELOGIN_INFO(L"FaceLoginService started");

    service.Run();

    service.m_status.dwCurrentState = SERVICE_STOPPED;
    service.m_status.dwWin32ExitCode = NO_ERROR;
    SetServiceStatus(service.m_hStatus, &service.m_status);
    FACELOGIN_INFO(L"FaceLoginService stopped");
}

void FaceService::RunStandalone() {
    FaceService service;

    {
        std::wstring logDir = ReadRegString(REGVAL_DATA_PATH, L"");
        if (logDir.empty()) {
            wchar_t programData[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
                logDir = std::wstring(programData) + L"\\FaceLogin";
            } else {
                logDir = L"C:\\ProgramData\\FaceLogin";
            }
        }
        CreateDirectoryW(logDir.c_str(), nullptr);
        std::wstring logPath = logDir + L"\\log\\service.log";
        Logger::Instance().SetLogFile(logPath);
    }
    Logger::Instance().SetMinLevel(LogLevel::Debug);
    Logger::Instance().SetEnableDebugOutput(true);
    FACELOGIN_INFO(L"=== FaceLoginService standalone mode ===");

    if (!service.Initialize()) {
        FACELOGIN_ERROR(L"Initialization failed");
        return;
    }

    service.Run();
}

DWORD WINAPI FaceService::HandlerEx(DWORD control, DWORD eventType,
                                     LPVOID eventData, LPVOID context) {

    auto* pService = static_cast<FaceService*>(context);

    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        pService->Stop();
        return NO_ERROR;
    case SERVICE_CONTROL_POWEREVENT: {
        // PBT_APMRESUMESUSPEND = resumed from sleep/hibernate. The camera may
        // still be in low-power recovery, so force a fresh camera init on the
        // next auth instead of reusing a stale SourceReader.
        if (eventType == PBT_APMRESUMESUSPEND) {
            FACELOGIN_INFO(L"Power resume event — forcing camera re-init on next auth");
            pService->m_resumedFlag.store(true);
        }
        return NO_ERROR;
    }
    case SERVICE_CONTROL_SESSIONCHANGE: {
        // Only respond to LOGON (user signed in) and LOGOFF (user signed out).
        // Ignore other events like WTS_SESSION_LOCK (7), WTS_SESSION_UNLOCK (8),
        // WTS_CONSOLE_CONNECT (1), etc. — those don't change logged-in state.
        auto* evt = reinterpret_cast<WTSSESSION_NOTIFICATION*>(eventData);
        if (evt && evt->cbSize == sizeof(WTSSESSION_NOTIFICATION)
            && evt->dwSessionId == WTSGetActiveConsoleSessionId()) {
            if (eventType == WTS_SESSION_LOGON) {
                FACELOGIN_INFO(L"Session LOGON: session=%lu → UserLoggedIn=1",
                              evt->dwSessionId);
                WriteRegDword(REGVAL_USER_LOGGED_IN, 1);
            } else if (eventType == WTS_SESSION_LOGOFF) {
                FACELOGIN_INFO(L"Session LOGOFF: session=%lu → UserLoggedIn=0",
                              evt->dwSessionId);
                WriteRegDword(REGVAL_USER_LOGGED_IN, 0);
                // Clear the service start uptime so the next logon is
                // detected as a cold boot (fresh ServiceStartUptime written
                // on next service restart, or if the service stays running,
                // the CP will see ServiceStartUptime=0 and treat it as cold
                // boot via the UserLoggedIn=0 fallback).
                WriteRegQword(REGVAL_SERVICE_START_UPTIME, 0);
            } else {
                FACELOGIN_INFO(L"Session change ignored: eventType=%lu", eventType);
            }
        }
        return NO_ERROR;
    }
    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(pService->m_hStatus, &pService->m_status);
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

bool FaceService::Initialize() {
    {
        std::wstring regData = ReadRegString(REGVAL_DATA_PATH, L"");
        if (!regData.empty()) {
            m_dataDir = regData;
        } else {
            wchar_t programData[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
                m_dataDir = std::wstring(programData) + L"\\FaceLogin";
            } else {
                m_dataDir = L"C:\\ProgramData\\FaceLogin";
            }
        }
    }
    CreateDirectoryW(m_dataDir.c_str(), nullptr);
    m_modelsDir = GetModelsDir();

    // Load configuration from config.json (falls back to registry). Must happen
    // before camera init — the configured camera_device is used below.
    m_config = LoadConfig(m_dataDir);
    m_matchThreshold = m_config.match_threshold;
    m_livenessMethod = m_config.liveness_method;
    m_antiSpoofThreshold = m_config.anti_spoof_threshold;

    std::wstring logPath = m_dataDir + L"\\log\\service.log";
    Logger::Instance().SetLogFile(logPath);
    Logger::Instance().SetMinLevel(LogLevel::Info);
    FACELOGIN_INFO(L"=== FaceLoginService initializing ===");
    FACELOGIN_INFO(L"Data dir: %s", m_dataDir.c_str());
    FACELOGIN_INFO(L"Models dir: %s", m_modelsDir.c_str());

    m_store = std::make_unique<CredentialStore>();
    m_store->SetDataDir(m_dataDir);
    if (!m_store->LoadDatabase()) {
        FACELOGIN_ERROR(L"Failed to load credential database");
        return false;
    }
    FACELOGIN_INFO(L"Loaded %zu registered user(s)", m_store->GetUserCount());

    // Write the service's system uptime at startup for the CP's cold-boot
    // detection.  The CP compares its own uptime to this value:
    //   close to this value (within ~120s) → cold boot (CP loaded near service)
    //   far above, or below (cross-boot stale) → cold boot
    //   far above (same boot, hours later) → unlock
    //
    // ALWAYS overwrite — registry persists across reboots, and GetTickCount64
    // resets to 0 on each boot, so a stale value from a prior boot would
    // corrupt detection if we skipped the write.
    {
        ULONGLONG uptime = GetTickCount64();
        WriteRegQword(REGVAL_SERVICE_START_UPTIME, uptime);
        FACELOGIN_INFO(L"Initialize: ServiceStartUptime = %llu", uptime);
    }

    m_detector = nullptr;  // loaded by the background thread — see StartBackgroundModelLoad()

    // Load SCRFD ONNX detector (the only detector, and the lightest model —
    // 2.5MB). This is loaded SYNCHRONOUSLY in Initialize() because the pipe
    // listener must be up as soon as possible: SCRFD is needed for the very
    // first frame of auth, and it loads in a few hundred ms even on a cold
    // mechanical disk. Everything heavier (2d106det landmark detector,
    // InsightFace recognizer, anti-spoof) is deferred to a background thread —
    // see StartBackgroundModelLoad(). The lock screen therefore connects to
    // the pipe the moment it appears instead of waiting out the model loads.
    m_onnxDetector = std::make_unique<OnnxDetector>();
    std::wstring onnxDetPath = m_modelsDir + L"\\det_500m.onnx";
    if (m_onnxDetector->Initialize(onnxDetPath)) {
        FACELOGIN_INFO(L"SCRFD detector loaded");
    } else {
        FACELOGIN_ERROR(L"SCRFD detector failed to load — face detection unavailable");
        return false;
    }

    // Heavy models (2d106det + recognizer + anti-spoof) load in a
    // background thread. See StartBackgroundModelLoad().
    StartBackgroundModelLoad();

    if (m_isServiceMode) {
        // Camera is initialized lazily per auth request to avoid
        // device contention with the console app. See Run().
        FACELOGIN_INFO(L"DirectShow webcam will be initialized on demand%s",
                       m_config.camera_device.empty() ? L"" : L" (configured device)");
    } else {
        // Media Foundation camera is also initialized on demand — keeping
        // it open across auth sessions causes the source reader to stall
        // (especially when FaceLoginConsole is running concurrently).
        FACELOGIN_INFO(L"MF webcam will be initialized on demand%s",
                       m_config.camera_device.empty() ? L"" : L" (configured device)");
    }

    m_pipeServer = std::make_unique<PipeServer>();

    // dlib recognizer/detector were removed — the system is now pure ONNX.
    // recognition_model/detector config values are ignored (only onnx/scrfd
    // are supported; anything else logs a warning for backwards compat).

    // NOTE: liveness-method validation (anti-spoof fallback to blink) happens
    // in LoadHeavyModels() after the anti-spoof model actually loads — at this
    // point it is still being loaded in the background.

    FACELOGIN_INFO(L"Liveness method: %hs", m_livenessMethod == LivenessMethod::Blink ? "blink" :
                  m_livenessMethod == LivenessMethod::AntiSpoof ? "antispoof" : "none");
    FACELOGIN_INFO(L"Match threshold: %.3f", m_matchThreshold);
    FACELOGIN_INFO(L"Initialization complete");

    return true;
}

// ============================================================================
// Lazy model loading (1.5.0)
//
// The pipe listener must be up as soon as possible so the credential provider
// connects the moment the lock screen appears. Loading the 99.7MB dlib shape
// predictor + 3 ONNX sessions synchronously in Initialize() pushed that by
// seconds on a cold boot. Instead:
//   - SCRFD (2.5MB) loads synchronously in Initialize() — light, needed for
//     the first frame, and its absence would block even a liveness-less auth.
//   - Everything heavier loads in this background thread, kicked off right
//     before Run() enters the pipe loop. The lock screen shows ~10-20s after
//     SCM starts the service, which is normally enough for the loads to finish
//     in the background — the user never waits.
//   - If a request arrives before they finish, ProcessAuthRequest() calls
//     EnsureModelsLoaded(), which blocks until ready (or fails/stop).
// ============================================================================

bool FaceService::LoadHeavyModels(bool lowLightEnhance) {
    FACELOGIN_INFO(L"Loading heavy models in background...");

    // 1. 106-point landmark detector (2d106det.onnx, ~5MB — replaces the
    // 99.7MB dlib shape predictor).
    {
        auto detector = std::make_unique<OnnxLandmarkDetector>();
        std::wstring path = m_modelsDir + L"\\2d106det.onnx";
        if (!detector->Initialize(path)) {
            FACELOGIN_ERROR(L"2d106det failed to load — landmarks unavailable");
            return false;
        }
        std::lock_guard<std::mutex> lock(m_modelMutex);
        m_detector = std::move(detector);
    }
    FACELOGIN_INFO(L"2d106det landmark detector loaded");

    // 2. InsightFace recognizer (w600k_mbf.onnx).
    {
        auto recognizer = std::make_unique<OnnxRecognizer>();
        std::wstring path = m_modelsDir + L"\\w600k_mbf.onnx";
        if (!recognizer->Initialize(path)) {
            FACELOGIN_ERROR(L"ONNX recognizer failed to load — recognition unavailable");
            return false;
        }
        recognizer->SetLowLightEnhance(lowLightEnhance);
        std::lock_guard<std::mutex> lock(m_modelMutex);
        m_onnxRecognizer = std::move(recognizer);
    }
    FACELOGIN_INFO(L"ONNX recognizer loaded (InsightFace buffalo_s)");

    // 3. Anti-spoof model (MiniFASNetV2) — optional.
    {
        auto antiSpoof = std::make_unique<OnnxAntiSpoof>();
        std::wstring path = m_modelsDir + L"\\OULU_Protocol_2_model_0_0.onnx";
        if (antiSpoof->Initialize(path)) {
            antiSpoof->SetLowLightEnhance(lowLightEnhance);
            std::lock_guard<std::mutex> lock(m_modelMutex);
            m_antiSpoof = std::move(antiSpoof);
            FACELOGIN_INFO(L"Anti-spoof model loaded (MiniFASNetV2)");
        } else {
            FACELOGIN_WARN(L"Anti-spoof model not available");
        }
    }

    // NOTE: liveness-method validation (anti-spoof → blink fallback) is NOT
    // done here. It touches m_livenessMethod / m_antiSpoof, which the main
    // thread (CONFIG_RELOAD) also mutates — doing it on this background thread
    // would race. It happens on the main thread in ValidateLivenessMethod(),
    // called after the models are known ready.

    FACELOGIN_INFO(L"Heavy models loaded");
    return true;
}

void FaceService::StartBackgroundModelLoad() {
    // Capture the config values the loader needs NOW. The main thread can
    // rewrite m_config via CONFIG_RELOAD while the loader is running; reading
    // the struct here avoids a data race and the loader's low-light toggle is
    // overridden by CONFIG_RELOAD afterward anyway.
    const bool lowLightEnhance = m_config.low_light_enhance;

    m_modelsLoading.store(true);
    m_modelLoadThread = std::thread([this, lowLightEnhance]() {
        // Load under a scoped RAII so the flags are cleared on every exit path
        // (including exceptions).
        struct LoadGuard {
            FaceService* svc;
            bool ok;
            ~LoadGuard() {
                svc->m_modelsLoading.store(false);
                svc->m_modelsReady.store(ok);
                svc->m_modelsFailed.store(!ok);
                svc->m_modelCv.notify_all();
            }
        };
        bool ok = false;
        try {
            ok = LoadHeavyModels(lowLightEnhance);
        } catch (const std::exception& e) {
            FACELOGIN_ERROR(L"Model loader threw: %hs", e.what());
        }
        LoadGuard guard{ this, ok };
    });
}

// Validate the configured liveness method against the loaded models. Falls
// back to blink if anti-spoof was configured but its model is unavailable.
// Safe to call once the heavy models are known loaded (the loader thread has
// finished mutating the model pointers). Takes m_modelMutex to read
// m_antiSpoof consistently with any CONFIG_RELOAD.
void FaceService::ValidateLivenessMethod() {
    std::lock_guard<std::mutex> lock(m_modelMutex);
    if (m_livenessMethod == LivenessMethod::AntiSpoof &&
        (!m_antiSpoof || !m_antiSpoof->IsInitialized())) {
        FACELOGIN_WARN(L"Anti-spoof configured but model not loaded, falling back to blink");
        m_livenessMethod = LivenessMethod::Blink;
    }
}

// Called from the main auth path before the first inference. Blocks until the
// heavy models are ready, or fails, or the service is stopping. Returns false
// only if a REQUIRED model failed to load (auth cannot proceed) or the service
// is stopping.
bool FaceService::EnsureModelsLoaded() {
    if (m_modelsReady.load()) return true;
    if (m_modelsFailed.load() && !m_modelsLoading.load()) return false;

    std::unique_lock<std::mutex> lock(m_modelMutex);
    m_modelCv.wait(lock, [this]() {
        return m_modelsReady.load() || m_modelsFailed.load() || m_modelsAbort.load();
    });
    return m_modelsReady.load();
}

// Release anyone blocked in EnsureModelsLoaded() during service shutdown so
// Stop() can join the loader thread without deadlocking.
void FaceService::AbortModelLoadWait() {
    m_modelsAbort.store(true);
    m_modelCv.notify_all();
    if (m_modelLoadThread.joinable()) {
        m_modelLoadThread.join();
    }
}

void FaceService::Run() {
    m_running = true;

    while (m_running) {
        if (!m_pipeServer->WaitForClient(60000)) {
            if (!m_running) break;
            continue;
        }

        std::wstring request;
        if (!m_pipeServer->ReadMessage(request, 30000)) {
            m_pipeServer->Disconnect();
            continue;
        }

        FACELOGIN_INFO(L"Received request: %s", request.c_str());

        if (request == ipc::MSG_RELOAD_DB) {
            m_store->LoadDatabase();
            m_pipeServer->WriteMessage(ipc::MSG_RELOAD_OK);
            m_pipeServer->Disconnect();
            FACELOGIN_INFO(L"Database reloaded");
        }
        else if (request == ipc::MSG_CONFIG_RELOAD) {
            m_config = LoadConfig(m_dataDir);
            m_matchThreshold = m_config.match_threshold;
            m_livenessMethod = m_config.liveness_method;
            m_antiSpoofThreshold = m_config.anti_spoof_threshold;

            // dlib recognizer/detector were removed — recognition_model and
            // detector config values are ignored (pure ONNX now).

            // The heavy models load in the background (1.5.0). If they aren't
            // done yet, wait for them so the config edits below don't race the
            // loader thread's model-pointer writes. Models are required for
            // auth anyway — if they failed to load there is no recognizer to
            // configure, so fail the reload with a descriptive error.
            if (!EnsureModelsLoaded()) {
                FACELOGIN_ERROR(L"CONFIG_RELOAD: required models failed to load");
                m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(L"服务模型加载失败"));
                FlushFileBuffers(m_pipeServer->GetHandle());
                m_pipeServer->DrainOutput(5000);
                m_pipeServer->Disconnect();
                continue;
            }

            // Now that the models are ready (loader finished), the pointer
            // mutations below are safe on the main thread.

            // Retry loading anti-spoof model if configured and not yet loaded
            if (m_livenessMethod == LivenessMethod::AntiSpoof && (!m_antiSpoof || !m_antiSpoof->IsInitialized())) {
                m_antiSpoof = std::make_unique<OnnxAntiSpoof>();
                std::wstring antiSpoofPath = m_modelsDir + L"\\OULU_Protocol_2_model_0_0.onnx";
                if (m_antiSpoof->Initialize(antiSpoofPath)) {
                    FACELOGIN_INFO(L"CONFIG_RELOAD: anti-spoof model loaded successfully");
                } else {
                    FACELOGIN_WARN(L"CONFIG_RELOAD: anti-spoof still unavailable, falling back to blink");
                    m_livenessMethod = LivenessMethod::Blink;
                    m_antiSpoof.reset();
                }
            }
            // Propagate the low-light enhancement toggle to the models (hot reload).
            m_onnxRecognizer->SetLowLightEnhance(m_config.low_light_enhance);
            if (m_antiSpoof) m_antiSpoof->SetLowLightEnhance(m_config.low_light_enhance);
            m_pipeServer->WriteMessage(ipc::MSG_CONFIG_RELOAD_OK);
            m_pipeServer->Disconnect();
            FACELOGIN_INFO(L"Configuration reloaded: rec=%hs det=%hs live=%hs thr=%.2f rotation=%d",
                          m_config.recognition_model.c_str(), m_config.detector.c_str(),
                          m_livenessMethod == LivenessMethod::Blink ? "blink" :
                          m_livenessMethod == LivenessMethod::AntiSpoof ? "antispoof" : "none",
                          m_matchThreshold, m_config.camera_rotation);
        }
        else if (request == ipc::MSG_GET_LOGS) {
            auto lines = Logger::Instance().GetRecentLogs(500);
            std::wstring resp(ipc::MSG_GET_LOGS_OK_PREFIX);
            for (size_t i = 0; i < lines.size(); i++) {
                if (i > 0) resp += L"\x1E"; // ASCII record separator
                // Escape backslashes and the separator char itself
                std::wstring safe = lines[i];
                // Remove trailing \r\n from each line
                while (!safe.empty() && (safe.back() == L'\r' || safe.back() == L'\n'))
                    safe.pop_back();
                resp += safe;
            }
            m_pipeServer->WriteMessage(resp);
            m_pipeServer->Disconnect();
            FACELOGIN_DEBUG(L"Sent %zu log lines to client", lines.size());
        }
        else if (request == ipc::MSG_AUTH_REQUEST) {
            // Lazy-init camera: create + start on demand, then fully shutdown
            // after auth to free the device for other processes.
            if (m_isServiceMode) {
                if (!m_webcamDS) {
                    m_webcamDS = std::make_unique<WebcamCaptureDS>();
                    if (!m_webcamDS->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
                        FACELOGIN_ERROR(L"DS camera init failed on demand");
                        m_webcamDS.reset();
                        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(L"摄像头不可用"));
                        FlushFileBuffers(m_pipeServer->GetHandle());
                        m_pipeServer->DrainOutput(5000);
                        m_pipeServer->Disconnect();
                        continue;
                    }
                    FACELOGIN_INFO(L"DS camera initialized on demand for auth");
                }
            } else {
                // After a system resume the camera may still be in low-power
                // recovery. Drop the stale instance so Initialize() rebuilds a
                // fresh SourceReader instead of reusing the one that stalled.
                if (m_resumedFlag.exchange(false) && m_webcamMF) {
                    FACELOGIN_INFO(L"Resume detected — rebuilding MF camera");
                    m_webcamMF->Shutdown();
                    m_webcamMF.reset();
                }
                if (!m_webcamMF) {
                    m_webcamMF = std::make_unique<WebcamCapture>();
                    if (!m_webcamMF->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
                        FACELOGIN_ERROR(L"MF camera init failed on demand");
                        m_webcamMF.reset();
                        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(L"摄像头不可用"));
                        FlushFileBuffers(m_pipeServer->GetHandle());
                        m_pipeServer->DrainOutput(5000);
                        m_pipeServer->Disconnect();
                        continue;
                    }
                    FACELOGIN_INFO(L"MF camera initialized on demand for auth");
                }
            }
            ProcessAuthRequest();
            if (m_isServiceMode && m_webcamDS) {
                m_webcamDS->Shutdown();
                m_webcamDS.reset();
                FACELOGIN_INFO(L"DS camera released after auth");
            }
            if (!m_isServiceMode && m_webcamMF) {
                m_webcamMF->Shutdown();
                m_webcamMF.reset();
                FACELOGIN_INFO(L"MF camera released after auth");
            }
            m_pipeServer->Disconnect();
        }
        else if (request == ipc::MSG_PING) {
            m_pipeServer->WriteMessage(ipc::MSG_PONG);
            m_pipeServer->Disconnect();
        }
        else {
            FACELOGIN_WARN(L"Unknown request: %s", request.c_str());
            m_pipeServer->Disconnect();
        }
    }
}

// Session events arrive via HandlerEx → SERVICE_CONTROL_SESSIONCHANGE.
// eventType carries WTS_SESSION_LOGON / WTS_SESSION_LOGOFF.
// eventData is a WTSSESSION_NOTIFICATION with the session ID.
// We only care about the console session (session 1).

void FaceService::Stop() {
    m_running = false;
    if (m_isServiceMode && m_webcamDS) {
        m_webcamDS->Shutdown();
        m_webcamDS.reset();
    }
    if (!m_isServiceMode && m_webcamMF) {
        m_webcamMF->Shutdown();
    }
    // Signal the model loader to stop and join it. If an auth request was
    // blocked in EnsureModelsLoaded(), the abort flag releases it so it can
    // exit cleanly (m_running is false → ProcessAuthRequest returns false).
    AbortModelLoadWait();
    if (m_pipeServer) {
        m_pipeServer->Close();
    }
}

bool FaceService::ProcessAuthRequest() {
    FACELOGIN_INFO(L"Starting face authentication...");

    // Single grab helper used by ALL auth stages (match, anti-spoof, blink,
    // final verify): it applies the configured camera rotation so every stage
    // operates on identically-oriented frames. Previously rotation was only
    // applied in the match loop, leaving the liveness/verify stages to process
    // unrotated frames — with 90/270 rotation the face was sideways there and
    // detection/landmarks/EAR failed, blocking unlock.
    auto grabFrame = [this](dlib::matrix<dlib::rgb_pixel>& f) -> bool {
        bool ok = m_isServiceMode ? m_webcamDS->GrabFrame(f)
                                  : (m_webcamMF ? m_webcamMF->GrabFrame(f) : false);
        if (ok) RotateFrame(f, m_config.camera_rotation);
        return ok;
    };

    if (m_store->GetUserCount() == 0) {
        FACELOGIN_WARN(L"No registered users");
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(L"\u6ca1\u6709\u6ce8\u518c\u7528\u6237"));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }

    // E: If the heavy models are still loading in the background (unusually
    // fast lock screen right after boot), tell the lock screen what's
    // happening instead of silently blocking. The CP updates the tile's status
    // text live via STATUS:, so the user sees "\u6b63\u5728\u52a0\u8f7d\u6a21\u578b..." rather than a
    // frozen "\u8bc6\u522b\u4e2d". The message is NOT flushed until after the wait below \u2014
    // if the models are already ready, this whole block is a no-op and no
    // extra STATUS message is sent.
    if (!m_modelsReady.load() && m_modelsLoading.load()) {
        m_pipeServer->WriteMessage(std::wstring(ipc::MSG_STATUS_PREFIX) + L"\u6b63\u5728\u52a0\u8f7d\u6a21\u578b...");
    }

    // Heavy models (2d106det + recognizer, optionally anti-spoof) load
    // in the background during startup. Normally they're ready by the time the
    // user triggers auth; if the lock screen appeared unusually fast, block
    // here until they finish. The auth timeout is running from when the CP
    // connected, so this only ever costs the tail of the boot time.
    if (!EnsureModelsLoaded()) {
        FACELOGIN_ERROR(L"Required models not loaded \u2014 cannot authenticate");
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(L"\u670d\u52a1\u6a21\u578b\u52a0\u8f7d\u5931\u8d25"));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }

    // Models are now known loaded \u2014 validate the liveness method once (e.g.
    // anti-spoof configured but model unavailable \u2192 fall back to blink). This
    // ran on the main thread only (see LoadHeavyModels: the loader never
    // touches liveness method), so no race with CONFIG_RELOAD.
    ValidateLivenessMethod();

    // C: Warm up the camera exposure ADAPTIVELY instead of a fixed 5-frame
    // delay. Dropping frames until the mean luma settles lets us exit the
    // moment the exposure is stable (usually after 2-3 frames) instead of
    // always waiting 5\u00d750ms \u2248 250ms. The saved time goes straight to the
    // "press key \u2192 camera ready" latency.
    //
    // Safety: the very first frames after camera start can be dark or
    // saturated. We require at least 2 frames and keep a short rolling window
    // so a single outlier (a hand passing the lens, a light flicker) doesn't
    // prematurely declare the exposure stable. If the camera never settles
    // (e.g. extreme backlight), the MAX cap (10 frames) bounds the wait and we
    // proceed anyway \u2014 a slightly under-exposed frame still detects a face.
    {
        constexpr int kWarmupMinFrames = 2;    // never exit before this many
        constexpr int kWarmupMaxFrames = 10;   // hard cap \u2014 proceed regardless
        constexpr int kWarmupWindow     = 3;   // rolling window size
        constexpr float kLumaTol        = 12.0f;  // mean-luma delta that counts as "stable"
        constexpr int kStableFrames     = 3;   // consecutive stable frames to exit

        dlib::matrix<dlib::rgb_pixel> warmFrame;
        int dropped = 0;
        int stableRun = 0;
        std::vector<float> window;      // rolling mean-luma window
        window.reserve(kWarmupWindow);

        for (; dropped < kWarmupMaxFrames; dropped++) {
            if (!grabFrame(warmFrame)) {
                // No frame yet \u2014 the camera is still starting. Wait and retry.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            if (dropped >= kWarmupMinFrames) {
                // Compute mean luma over a subsampled grid (every 4th pixel in
                // each direction \u2014 180\u00d7101 samples for 1280\u00d7720) to keep the
                // warmup cheap; full-frame mean luma would cost ~2ms per frame
                // and we already know the exposure within \u00b1few percent.
                double sum = 0.0;
                long count = 0;
                const long rows = warmFrame.nr();
                const long cols = warmFrame.nc();
                for (long r = 0; r < rows; r += 4) {
                    for (long c = 0; c < cols; c += 4) {
                        const auto& px = warmFrame(r, c);
                        sum += 0.299 * px.red + 0.587 * px.green + 0.114 * px.blue;
                        count++;
                    }
                }
                float luma = static_cast<float>(sum / count);

                // Rolling window: stable if all entries stay within tolerance.
                window.push_back(luma);
                if (static_cast<long>(window.size()) > kWarmupWindow) {
                    window.erase(window.begin());
                }
                if (static_cast<long>(window.size()) == kWarmupWindow) {
                    float minV = *std::min_element(window.begin(), window.end());
                    float maxV = *std::max_element(window.begin(), window.end());
                    if (maxV - minV <= kLumaTol) {
                        stableRun++;
                    } else {
                        stableRun = 0;
                    }
                    if (stableRun >= kStableFrames) {
                        FACELOGIN_INFO(L"Warmup: exposure stable after %d frames (luma=%.0f)", dropped + 1, luma);
                        break;
                    }
                }
            }

            // Slow the polling to ~30ms once we've dropped the minimum, so the
            // warmup doesn't spin the CPU at full grab rate while it waits.
            std::this_thread::sleep_for(std::chrono::milliseconds(
                dropped < kWarmupMinFrames ? 30 : 50));
        }
        if (dropped >= kWarmupMaxFrames) {
            FACELOGIN_INFO(L"Warmup: max %d frames dropped, proceeding (exposure not settled)",
                           kWarmupMaxFrames);
        }
    }

    // STATUS: Notify credential provider that recognition has started.
    // L"\u8bc6\u522b\u4e2d..." = L"识别中..."
    {
        std::wstring statusMsg = std::wstring(ipc::MSG_STATUS_PREFIX) + L"\u8bc6\u522b\u4e2d...";
        m_pipeServer->WriteMessage(statusMsg);
    }

    dlib::matrix<dlib::rgb_pixel> frame;  // reused by the match loop below
    auto startTime = std::chrono::steady_clock::now();
    bool authSent = false;
    int consecutiveMatches = 0;
    static constexpr int CONSENSUS_FRAMES = 3;

    while (m_running) {
        // Abort early if the client (LogonUI) has gone away — e.g. the user
        // switched to password/fingerprint unlock. Otherwise we'd keep the
        // camera on until the timeout.
        if (m_pipeServer->IsClientDisconnected()) {
            FACELOGIN_INFO(L"Client disconnected during auth — aborting, releasing camera");
            return false;
        }

        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= m_authTimeoutSeconds) {
            FACELOGIN_INFO(L"Authentication timed out");
            m_pipeServer->WriteMessage(ipc::MSG_AUTH_TIMEOUT);
            FlushFileBuffers(m_pipeServer->GetHandle());
            m_pipeServer->DrainOutput(5000);
            return false;
        }

        if (!grabFrame(frame)) {
            if (!m_running) return false;
            // A stalled camera (e.g. after resume) self-shut-down in
            // GrabFrame. Rebuild it here so auth can continue instead of
            // spinning on a dead SourceReader until timeout.
            if (m_webcamMF && !m_webcamMF->IsInitialized()) {
                FACELOGIN_INFO(L"MF camera stalled — re-initializing");
                m_webcamMF->Shutdown();
                m_webcamMF.reset();
                m_webcamMF = std::make_unique<WebcamCapture>();
                if (!m_webcamMF->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
                    FACELOGIN_ERROR(L"MF camera re-init failed");
                    m_webcamMF.reset();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // Face detection + landmarks: SCRFD detects, 2d106det extracts the
        // 106 points for alignment + blink.
        std::optional<CredentialStore::MatchResult> match;
        dlib::full_object_detection landmarks;
        bool haveLandmarks = false;

        auto onnxDet = m_onnxDetector->DetectLargestFace(frame);
        if (onnxDet) {
            // SCRFD gives bbox — 2d106det extracts 106 landmarks.
            dlib::rectangle dlibRect(static_cast<long>(onnxDet->x1),
                                     static_cast<long>(onnxDet->y1),
                                     static_cast<long>(onnxDet->x2),
                                     static_cast<long>(onnxDet->y2));
            haveLandmarks = m_detector->DetectLandmarks(frame, dlibRect, landmarks);
        }

        if (!haveLandmarks) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // Embedding + match: ONNX (the only recognizer).
        auto onnxEmb = m_onnxRecognizer->ComputeEmbedding(frame, landmarks);
        if (!onnxEmb.empty()) {
            // Pass the true dimensionality (512-D) so FindBestMatch compares
            // against same-dimension stored embeddings only.
            match = m_store->FindBestMatch(onnxEmb.data(), onnxEmb.size(), m_matchThreshold);
        }

        if (match) {
            consecutiveMatches++;
            FACELOGIN_INFO(L"Face matched: %s (distance=%.4f) [%d/%d]",
                          match->username.c_str(), match->distance,
                          consecutiveMatches, CONSENSUS_FRAMES);

            if (consecutiveMatches < CONSENSUS_FRAMES) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
        } else {
            // Soft consensus: a miss DECAYS the counter by 1 instead of fully
            // resetting to 0. A single intermittent bad frame (motion, blink,
            // momentary profile turn, partial occlusion) then no longer forces
            // a full 3-frame restart — the user's slightly moving face stays
            // matched and auth completes in ~1-2s instead of timing out.
            //
            // Security is preserved: this only relaxes the frame-consensus;
            // the blink liveness check AND the post-liveness final match verify
            // still run before credentials are released.
            if (consecutiveMatches > 0) {
                consecutiveMatches--;
                FACELOGIN_INFO(L"Match lost — counter decayed to %d", consecutiveMatches);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        {
            // Passwordless account: face login cannot unlock it (no password to
            // submit to LSA). Degrade gracefully with a notice instead of
            // attempting liveness and submitting nothing. Do NOT mark the user
            // as logged in.
            if (match->passwordless) {
                FACELOGIN_WARN(L"Matched passwordless account '%s' — face login cannot unlock; notifying CP",
                               match->username.c_str());
                m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(ipc::MSG_PASSWORDLESS_NOTICE));
                FlushFileBuffers(m_pipeServer->GetHandle());
                m_pipeServer->DrainOutput(5000);
                return false;
            }

            std::wstring domain = L".";
            wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1] = {};
            DWORD size = ARRAYSIZE(computerName);
            if (GetComputerNameW(computerName, &size)) {
                domain = computerName;
            }

            std::wstring msg = ipc::BuildAuthSuccessMessage(
                match->sid, match->upn,
                domain, match->username, match->password);

            // === Liveness check ===
            {
                LivenessMethod method = m_livenessMethod;

                // Determine status text
                if (method == LivenessMethod::AntiSpoof) {
                    m_pipeServer->WriteMessage(std::wstring(ipc::MSG_STATUS_PREFIX) + L"\u6b63\u5728\u8fdb\u884c\u6d3b\u4f53\u68c0\u6d4b...");
                } else if (method == LivenessMethod::Blink) {
                    m_pipeServer->WriteMessage(std::wstring(ipc::MSG_STATUS_PREFIX) + L"\u8bf7\u7728\u773c\u4ee5\u786e\u8ba4\u6d3b\u4f53...");
                }
                if (method != LivenessMethod::None) {
                    FlushFileBuffers(m_pipeServer->GetHandle());
                }

                bool livenessPassed = false;

                if (method == LivenessMethod::None) {
                    livenessPassed = true;
                } else if (method == LivenessMethod::AntiSpoof) {
                    // Anti-spoof consensus check with threshold-driven frame count:
                    // low threshold (lenient) → fewer checks, high threshold (strict) → more.
                    int totalChecks = AntiSpoofCheckCount(m_antiSpoofThreshold);
                    int passRequired = AntiSpoofPassRequired(totalChecks);
                    FACELOGIN_INFO(L"Anti-spoof: threshold=%.3f → %d checks, %d required",
                                   m_antiSpoofThreshold, totalChecks, passRequired);
                    auto asStart = std::chrono::steady_clock::now();
                    int passCount = 0, totalChecked = 0;
                    while (m_running && totalChecked < totalChecks) {
                        if (m_pipeServer->IsClientDisconnected()) {
                            FACELOGIN_INFO(L"Client disconnected during anti-spoof — aborting");
                            SecureZeroMemory(match->password.data(), match->password.size() * sizeof(wchar_t));
                            return false;
                        }
                        auto elapsed = std::chrono::steady_clock::now() - asStart;
                        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 5) break;

                        dlib::matrix<dlib::rgb_pixel> asFrame;
                        if (!grabFrame(asFrame)) { if (!m_running) break; std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }

                        // Detect with SCRFD, extract 106-point landmarks.
                        dlib::full_object_detection asLandmarks;
                        auto asDet = m_onnxDetector->DetectLargestFace(asFrame);
                        asLandmarks = dlib::full_object_detection();  // reset for this frame
                        if (asDet) {
                            dlib::rectangle asRect(static_cast<long>(asDet->x1),
                                                   static_cast<long>(asDet->y1),
                                                   static_cast<long>(asDet->x2),
                                                   static_cast<long>(asDet->y2));
                            m_detector->DetectLandmarks(asFrame, asRect, asLandmarks);
                        }
                        if (asLandmarks.num_parts() == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }

                        float score = m_antiSpoof->Predict(asFrame, asLandmarks);
                        totalChecked++;
                        if (score >= m_antiSpoofThreshold) passCount++; // config-driven threshold
                        FACELOGIN_INFO(L"Anti-spoof frame %d: score=%.3f (pass=%d)", totalChecked, score, passCount);

                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    livenessPassed = (totalChecked > 0 && passCount >= passRequired);
                    if (!livenessPassed) {
                        FACELOGIN_WARN(L"Anti-spoof check failed: %d/%d passed (need %d)",
                                       passCount, totalChecked, passRequired);
                    }
                } else if (method == LivenessMethod::Blink) {
                    LivenessDetector liveness;
                    liveness.Configure(kDefaultEarThreshold, kDefaultBlinkFrames,
                                       m_config.blink_glasses_mode);
                    auto livenessStart = std::chrono::steady_clock::now();
                    bool blinked = false;
                    while (m_running) {
                        if (m_pipeServer->IsClientDisconnected()) {
                            FACELOGIN_INFO(L"Client disconnected during blink check — aborting");
                            SecureZeroMemory(match->password.data(), match->password.size() * sizeof(wchar_t));
                            return false;
                        }
                        auto livenessElapsed = std::chrono::steady_clock::now() - livenessStart;
                        // 8s timeout (was 5s): a user may react to the "blink"
                        // prompt with a slight delay, and the detection loop only
                        // runs at ~6fps. 5s was too tight for a comfortable blink.
                        if (std::chrono::duration_cast<std::chrono::seconds>(livenessElapsed).count() >= 8) {
                            FACELOGIN_WARN(L"Liveness check timed out \u2014 no blink detected");
                            break;
                        }
                        dlib::matrix<dlib::rgb_pixel> livenessFrame;
                        if (!grabFrame(livenessFrame)) { if (!m_running) break; std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }
                        // Detect with SCRFD, extract 106-point landmarks for EAR.
                        dlib::full_object_detection livenessLandmarks;
                        auto livenessDet = m_onnxDetector->DetectLargestFace(livenessFrame);
                        if (livenessDet) {
                            dlib::rectangle lRect(static_cast<long>(livenessDet->x1),
                                                  static_cast<long>(livenessDet->y1),
                                                  static_cast<long>(livenessDet->x2),
                                                  static_cast<long>(livenessDet->y2));
                            m_detector->DetectLandmarks(livenessFrame, lRect, livenessLandmarks);
                        }
                        if (livenessLandmarks.num_parts() == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }
                        if (liveness.ProcessFrame(livenessLandmarks)) {
                            blinked = true;
                            FACELOGIN_INFO(L"Blink detected");
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    }
                    livenessPassed = blinked;
                }

                if (!livenessPassed) {
                    FACELOGIN_WARN(L"Liveness check failed");
                    m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(
                        method == LivenessMethod::AntiSpoof ?
                        L"\u68c0\u6d4b\u5230\u653b\u51fb\uff0c\u8bf7\u4f7f\u7528\u771f\u5b9e\u4eba\u8138" :
                        L"\u672a\u68c0\u6d4b\u5230\u7728\u773c\uff0c\u8bf7\u52a8\u4f5c\u660e\u786e\u5730\u95ed\u773c\u518d\u7741\u5f00\u91cd\u8bd5"));
                    FlushFileBuffers(m_pipeServer->GetHandle());
                    m_pipeServer->DrainOutput(5000);
                    SecureZeroMemory(match->password.data(), match->password.size() * sizeof(wchar_t));
                    return false;
                }

                FACELOGIN_INFO(L"Liveness passed \u2014 verifying match");

                // Final match verify (for blink/antispoof \u2014 prevents face-swap).
                //
                // Uses the SAME SCRFD detector as the recognition stage so the
                // two stages agree on face position. Retries over a short window:
                // the frame right after a blink is often mid-motion and its
                // box/embedding is noisy, so a single frame is unreliable. We
                // keep grabbing until a frame both detects a face AND matches
                // (or ~2s elapses).
                if (method != LivenessMethod::None) {
                    auto verifyStart = std::chrono::steady_clock::now();
                    bool verifyOk = false;
                    while (m_running && !verifyOk) {
                        if (m_pipeServer->IsClientDisconnected()) {
                            FACELOGIN_INFO(L"Client disconnected during final verify — aborting");
                            SecureZeroMemory(match->password.data(), match->password.size() * sizeof(wchar_t));
                            return false;
                        }
                        auto vElapsed = std::chrono::steady_clock::now() - verifyStart;
                        if (std::chrono::duration_cast<std::chrono::seconds>(vElapsed).count() >= 2) break;

                        dlib::matrix<dlib::rgb_pixel> verifyFrame;
                        if (!grabFrame(verifyFrame)) {
                            if (!m_running) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }

                        // Detect with SCRFD (primary), same as the recognition loop.
                        dlib::full_object_detection verifyLandmarks;
                        auto det = m_onnxDetector->DetectLargestFace(verifyFrame);
                        if (det) {
                            dlib::rectangle r(static_cast<long>(det->x1),
                                              static_cast<long>(det->y1),
                                              static_cast<long>(det->x2),
                                              static_cast<long>(det->y2));
                            m_detector->DetectLandmarks(verifyFrame, r, verifyLandmarks);
                        }
                        if (verifyLandmarks.num_parts() == 0) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }

                        std::optional<CredentialStore::MatchResult> verifyMatch;
                        auto onnxEmb = m_onnxRecognizer->ComputeEmbedding(verifyFrame, verifyLandmarks);
                        if (!onnxEmb.empty()) {
                            verifyMatch = m_store->FindBestMatch(onnxEmb.data(), onnxEmb.size(), m_matchThreshold);
                        }

                        if (verifyMatch) {
                            verifyOk = true;
                            // Use the verified match for the credential (fresh, same identity).
                            match = verifyMatch;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    }

                    if (!verifyOk) {
                        FACELOGIN_WARN(L"Final match verify failed \u2014 face swap detected");
                        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(
                            L"\u6d3b\u4f53\u9a8c\u8bc1\u671f\u95f4\u4eba\u8138\u4e0d\u5339\u914d\uff0c\u8bf7\u91cd\u8bd5"));
                        FlushFileBuffers(m_pipeServer->GetHandle());
                        m_pipeServer->DrainOutput(5000);
                        SecureZeroMemory(match->password.data(), match->password.size() * sizeof(wchar_t));
                        return false;
                    }
                }
            }

            m_pipeServer->WriteMessage(msg);
            FlushFileBuffers(m_pipeServer->GetHandle());

            SecureZeroMemory(match->password.data(),
                           match->password.size() * sizeof(wchar_t));

            authSent = true;
            FACELOGIN_INFO(L"Credentials sent for %s\\%s",
                          domain.c_str(), match->username.c_str());

            // Mark user as logged in IMMEDIATELY after sending credentials.
            // This prevents a race condition: the user can lock (Win+L)
            // before Windows fires WTS_SESSION_LOGON (which can take
            // seconds), causing the CP to misdetect the unlock as a cold
            // boot because UserLoggedIn is still 0.
            WriteRegDword(REGVAL_USER_LOGGED_IN, 1);
            FACELOGIN_INFO(L"UserLoggedIn=1 written after auth success");

            // Stop the capture graph NOW (camera LED off) before the bounded
            // pipe drain below. The graph keeps streaming during auth; pausing
            // it immediately after success frees the camera without waiting for
            // the full teardown.
            if (m_isServiceMode && m_webcamDS) {
                m_webcamDS->Pause();
            } else if (!m_isServiceMode && m_webcamMF) {
                m_webcamMF->Shutdown();
            }

            // Bounded drain: wait briefly for the client to consume the
            // AUTH_SUCCESS message, then return. The old code did an
            // unbounded ReadFile(dummy) here — if the client closed the pipe
            // or never read, the service blocked forever and SCM killed it.
            m_pipeServer->DrainOutput(5000);
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    return authSent;
}

std::wstring FaceService::GetModelsDir() {
    {
        std::wstring regData = ReadRegString(REGVAL_DATA_PATH, L"");
        if (!regData.empty()) {
            return regData + L"\\models";
        }
    }
    wchar_t programData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
        return std::wstring(programData) + L"\\FaceLogin\\models";
    }
    return L"C:\\ProgramData\\FaceLogin\\models";
}

float FaceService::GetMatchThreshold() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\FaceLogin", 0,
                      KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 0, size = sizeof(val);
        if (RegQueryValueExW(hKey, L"MatchThreshold", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return val / 100.0f;
        }
        RegCloseKey(hKey);
    }
    return 0.30f;
}

bool FaceService::Install(const std::wstring& exePath) {
    SC_HANDLE hSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager) {
        FACELOGIN_ERROR(L"OpenSCManager failed: %lu", GetLastError());
        return false;
    }

    SC_HANDLE hService = CreateServiceW(
        hSCManager, SERVICE_NAME, L"FaceLogin Authentication Service",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        exePath.c_str(), nullptr, nullptr, nullptr,
        nullptr, nullptr);

    if (!hService) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_EXISTS) {
            FACELOGIN_ERROR(L"CreateService failed: %lu", err);
            CloseServiceHandle(hSCManager);
            return false;
        }
        hService = OpenServiceW(hSCManager, SERVICE_NAME, SERVICE_ALL_ACCESS);
        if (!hService) {
            CloseServiceHandle(hSCManager);
            return false;
        }
    }

    SERVICE_DESCRIPTIONW desc = {};
    desc.lpDescription = const_cast<LPWSTR>(
        L"FaceLogin \u2014 custom face recognition authentication for Windows login");
    ChangeServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, &desc);

    SERVICE_FAILURE_ACTIONSW fa = {};
    SC_ACTION actions[3] = {};
    actions[0].Type = SC_ACTION_RESTART;
    actions[0].Delay = 60000;
    actions[1].Type = SC_ACTION_RESTART;
    actions[1].Delay = 60000;
    actions[2].Type = SC_ACTION_RESTART;
    actions[2].Delay = 60000;
    fa.dwResetPeriod = 86400;
    fa.lpRebootMsg = nullptr;
    fa.lpCommand = nullptr;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(hService, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);

    FACELOGIN_INFO(L"Service installed: %s", exePath.c_str());
    return true;
}

bool FaceService::Uninstall() {
    SC_HANDLE hSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager) return false;

    SC_HANDLE hService = OpenServiceW(hSCManager, SERVICE_NAME, SERVICE_STOP | DELETE);
    if (!hService) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            CloseServiceHandle(hSCManager);
            return true;
        }
        CloseServiceHandle(hSCManager);
        return false;
    }

    SERVICE_STATUS status;
    ControlService(hService, SERVICE_CONTROL_STOP, &status);

    for (int i = 0; i < 30; i++) {
        QueryServiceStatus(hService, &status);
        if (status.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(1000);
    }

    DeleteService(hService);
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);

    FACELOGIN_INFO(L"Service uninstalled");
    return true;
}

} // namespace facelogin
