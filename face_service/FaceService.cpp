#include "FaceService.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include "../common/secure_buffer.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include "liveness_detector.h"
#include <shlobj.h>
#include <chrono>
#include <thread>
#include <algorithm>
#include <wtsapi32.h>

#pragma comment(lib, "wtsapi32.lib")

namespace facelogin {

FaceService* FaceService::s_pInstance = nullptr;

static constexpr wchar_t SERVICE_NAME[] = L"FaceLoginService";

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
        | SERVICE_ACCEPT_SESSIONCHANGE;
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

    m_detector = std::make_unique<FaceDetector>();
    std::wstring shapePredictorPath = m_modelsDir + L"\\shape_predictor_68_face_landmarks.dat";
    if (!m_detector->Initialize(shapePredictorPath)) {
        FACELOGIN_ERROR(L"Failed to initialize face detector");
        return false;
    }

    // Try loading SCRFD ONNX detector (primary).
    // Falls back to dlib HOG if ONNX model is unavailable.
    m_onnxDetector = std::make_unique<OnnxDetector>();
    std::wstring onnxDetPath = m_modelsDir + L"\\det_500m.onnx";
    if (m_onnxDetector->Initialize(onnxDetPath)) {
        FACELOGIN_INFO(L"SCRFD detector loaded");
    } else {
        FACELOGIN_WARN(L"SCRFD detector failed, using dlib HOG");
        m_onnxDetector.reset();
    }

    m_recognizer = std::make_unique<FaceRecognizer>();
    std::wstring recModelPath = m_modelsDir + L"\\dlib_face_recognition_resnet_model_v1.dat";
    if (!m_recognizer->Initialize(recModelPath)) {
        FACELOGIN_ERROR(L"Failed to initialize face recognizer");
        return false;
    }

    // Try loading InsightFace ONNX model (primary recognizer).
    // Falls back to dlib if ONNX model is unavailable.
    m_onnxRecognizer = std::make_unique<OnnxRecognizer>();
    std::wstring onnxRecPath = m_modelsDir + L"\\w600k_mbf.onnx";
    if (m_onnxRecognizer->Initialize(onnxRecPath)) {
        FACELOGIN_INFO(L"ONNX recognizer loaded — using InsightFace buffalo_s");
    } else {
        FACELOGIN_WARN(L"ONNX recognizer failed, using dlib only");
        m_onnxRecognizer.reset();
    }

    // Try loading anti-spoof model (MiniFASNetV2).
    m_antiSpoof = std::make_unique<OnnxAntiSpoof>();
    std::wstring antiSpoofPath = m_modelsDir + L"\\OULU_Protocol_2_model_0_0.onnx";
    if (m_antiSpoof->Initialize(antiSpoofPath)) {
        FACELOGIN_INFO(L"Anti-spoof model loaded (MiniFASNetV2)");
    } else {
        FACELOGIN_WARN(L"Anti-spoof model not available");
        m_antiSpoof.reset();
    }

    if (m_isServiceMode) {
        // Camera is initialized lazily per auth request to avoid
        // device contention with the console app. See Run().
        FACELOGIN_INFO(L"DirectShow webcam will be initialized on demand");
    } else {
        m_webcamMF = std::make_unique<WebcamCapture>();
        if (!m_webcamMF->Initialize(1280, 720)) {
            FACELOGIN_ERROR(L"Failed to initialize MF webcam");
            return false;
        }
    }

    m_pipeServer = std::make_unique<PipeServer>();

    // Load configuration from config.json (falls back to registry)
    m_config = LoadConfig(m_dataDir);
    m_matchThreshold = m_config.match_threshold;
    m_livenessMethod = m_config.liveness_method;
    m_antiSpoofThreshold = m_config.anti_spoof_threshold;

    // Apply detector preference
    if (m_config.detector == "dlib_hog") {
        FACELOGIN_INFO(L"Config: forcing dlib HOG detector");
        m_onnxDetector.reset();
    }

    // Apply recognizer preference
    if (m_config.recognition_model == "dlib") {
        FACELOGIN_INFO(L"Config: using dlib recognizer only");
        m_onnxRecognizer.reset();
    } else if (m_config.recognition_model == "onnx" && (!m_onnxRecognizer || !m_onnxRecognizer->IsInitialized())) {
        FACELOGIN_WARN(L"Config: ONNX recognizer requested but unavailable");
    }
    // "both" = keep both loaded (ONNX preferred for matching)

    // Validate liveness method — fall back if model unavailable
    if (m_livenessMethod == LivenessMethod::AntiSpoof && (!m_antiSpoof || !m_antiSpoof->IsInitialized())) {
        FACELOGIN_WARN(L"Anti-spoof configured but model not loaded, falling back to blink");
        m_livenessMethod = LivenessMethod::Blink;
    }

    FACELOGIN_INFO(L"Liveness method: %hs", m_livenessMethod == LivenessMethod::Blink ? "blink" :
                  m_livenessMethod == LivenessMethod::AntiSpoof ? "antispoof" : "none");
    FACELOGIN_INFO(L"Match threshold: %.3f", m_matchThreshold);
    FACELOGIN_INFO(L"Initialization complete");

    return true;
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
            m_pipeServer->WriteMessage(ipc::MSG_CONFIG_RELOAD_OK);
            m_pipeServer->Disconnect();
            FACELOGIN_INFO(L"Configuration reloaded: live=%hs thr=%.2f",
                          m_livenessMethod == LivenessMethod::Blink ? "blink" :
                          m_livenessMethod == LivenessMethod::AntiSpoof ? "antispoof" : "none",
                          m_matchThreshold);
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
                    if (!m_webcamDS->Initialize(1280, 720)) {
                        FACELOGIN_ERROR(L"DS camera init failed on demand");
                        m_webcamDS.reset();
                        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(L"摄像头不可用"));
                        FlushFileBuffers(m_pipeServer->GetHandle());
                        wchar_t dummy[64]; DWORD br;
                        ReadFile(m_pipeServer->GetHandle(), dummy, sizeof(dummy), &br, nullptr);
                        m_pipeServer->Disconnect();
                        continue;
                    }
                    FACELOGIN_INFO(L"DS camera initialized on demand for auth");
                }
            }
            ProcessAuthRequest();
            if (m_isServiceMode && m_webcamDS) {
                m_webcamDS->Shutdown();
                m_webcamDS.reset();
                FACELOGIN_INFO(L"DS camera released after auth");
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
    if (m_pipeServer) {
        m_pipeServer->Close();
    }
}

bool FaceService::ProcessAuthRequest() {
    FACELOGIN_INFO(L"Starting face authentication...");

    auto grabFrame = [this](dlib::matrix<dlib::rgb_pixel>& f) -> bool {
        if (m_isServiceMode)
            return m_webcamDS->GrabFrame(f);
        if (m_webcamMF)
            return m_webcamMF->GrabFrame(f);
        return false;
    };

    if (m_store->GetUserCount() == 0) {
        FACELOGIN_WARN(L"No registered users");
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(L"\u6ca1\u6709\u6ce8\u518c\u7528\u6237"));
        FlushFileBuffers(m_pipeServer->GetHandle());
        {
            wchar_t dummy[64];
            DWORD bytesRead = 0;
            ReadFile(m_pipeServer->GetHandle(), dummy, sizeof(dummy),
                     &bytesRead, nullptr);
        }
        return false;
    }

    // Drop initial frames to let camera exposure adjust. 5 frames is enough
    // (exposure settles within 3-5 frames); 10 frames wasted ~0.3s per auth.
    dlib::matrix<dlib::rgb_pixel> frame;
    for (int i = 0; i < 5; i++) {
        grabFrame(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // STATUS: Notify credential provider that recognition has started.
    // L"\u8bc6\u522b\u4e2d..." = L"识别中..."
    {
        std::wstring statusMsg = std::wstring(ipc::MSG_STATUS_PREFIX) + L"\u8bc6\u522b\u4e2d...";
        m_pipeServer->WriteMessage(statusMsg);
    }

    auto startTime = std::chrono::steady_clock::now();
    bool authSent = false;
    int consecutiveMatches = 0;
    static constexpr int CONSENSUS_FRAMES = 3;

    while (m_running) {
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= m_authTimeoutSeconds) {
            FACELOGIN_INFO(L"Authentication timed out");
            m_pipeServer->WriteMessage(ipc::MSG_AUTH_TIMEOUT);
            FlushFileBuffers(m_pipeServer->GetHandle());
            if (m_running) {
                wchar_t dummy[64];
                DWORD bytesRead = 0;
                ReadFile(m_pipeServer->GetHandle(), dummy, sizeof(dummy),
                         &bytesRead, nullptr);
            }
            return false;
        }

        if (!grabFrame(frame)) {
            if (!m_running) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // Face detection + landmarks: prefer SCRFD ONNX, fall back to dlib HOG.
        // NOTE: the dlib embedding is deliberately NOT computed here. It used to
        // run on every frame (~150-200ms of ResNet inference) even though the
        // match uses the ONNX embedding. Now the dlib embedding is computed
        // lazily, only when the ONNX embedding is unavailable (rare fallback).
        std::optional<CredentialStore::MatchResult> match;
        dlib::full_object_detection landmarks;
        bool haveLandmarks = false;

        if (m_onnxDetector) {
            auto onnxDet = m_onnxDetector->DetectLargestFace(frame);
            if (onnxDet) {
                // SCRFD gives bbox — use dlib shape predictor for landmarks
                dlib::rectangle dlibRect(static_cast<long>(onnxDet->x1),
                                         static_cast<long>(onnxDet->y1),
                                         static_cast<long>(onnxDet->x2),
                                         static_cast<long>(onnxDet->y2));
                landmarks = m_detector->GetLandmarks(frame, dlibRect);
                haveLandmarks = true;
            }
        }

        if (!haveLandmarks) {
            auto faces = m_detector->Detect(frame);
            if (faces.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            auto largestIt = std::max_element(faces.begin(), faces.end(),
                [](const FaceWithLandmarks& a, const FaceWithLandmarks& b) {
                    return a.rect.area() < b.rect.area();
                });
            landmarks = largestIt->landmarks;
            haveLandmarks = true;
        }

        // Embedding + match: prefer ONNX (fast, ~20ms). Compute the dlib
        // embedding only when the ONNX recognizer is unavailable or fails on
        // this frame — this is the rare fallback path.
        if (m_onnxRecognizer) {
            auto onnxEmb = m_onnxRecognizer->ComputeEmbedding(frame, landmarks);
            if (!onnxEmb.empty()) {
                // Enrollment stores ONNX embeddings, so ONNX match alone is sufficient.
                match = m_store->FindBestMatch(onnxEmb.data(), m_matchThreshold);
            } else {
                // ONNX failed on this frame — fall back to dlib embedding
                dlib::matrix<float, 0, 1> dlibEmb =
                    m_recognizer->ComputeEmbedding(frame, landmarks);
                if (dlibEmb.size() > 0) {
                    match = m_store->FindBestMatch(&dlibEmb(0), m_matchThreshold);
                }
            }
        } else {
            // No ONNX recognizer loaded — dlib only
            dlib::matrix<float, 0, 1> dlibEmb =
                m_recognizer->ComputeEmbedding(frame, landmarks);
            if (dlibEmb.size() > 0) {
                match = m_store->FindBestMatch(&dlibEmb(0), m_matchThreshold);
            }
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
            if (consecutiveMatches > 0) {
                FACELOGIN_INFO(L"Match lost after %d consecutive hits", consecutiveMatches);
            }
            consecutiveMatches = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        {
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
                        auto elapsed = std::chrono::steady_clock::now() - asStart;
                        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 5) break;

                        dlib::matrix<dlib::rgb_pixel> asFrame;
                        if (!grabFrame(asFrame)) { if (!m_running) break; std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }

                        auto asFace = m_detector->DetectLargestFace(asFrame);
                        if (!asFace) { std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }

                        float score = m_antiSpoof->Predict(asFrame, asFace->landmarks);
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
                    liveness.Configure(kDefaultEarThreshold, kDefaultBlinkFrames);
                    auto livenessStart = std::chrono::steady_clock::now();
                    bool blinked = false;
                    while (m_running) {
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
                        auto livenessFace = m_detector->DetectLargestFace(livenessFrame);
                        if (!livenessFace) { std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }
                        if (liveness.ProcessFrame(livenessFace->landmarks)) {
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
                    {
                        wchar_t dummy[64]; DWORD bytesRead = 0;
                        ReadFile(m_pipeServer->GetHandle(), dummy, sizeof(dummy), &bytesRead, nullptr);
                    }
                    SecureZeroMemory(match->password.data(), match->password.size() * sizeof(wchar_t));
                    return false;
                }

                FACELOGIN_INFO(L"Liveness passed \u2014 verifying match");

                // Final match verify (for blink/antispoof \u2014 prevents face-swap)
                if (method != LivenessMethod::None) {
                    dlib::matrix<dlib::rgb_pixel> verifyFrame;
                    if (grabFrame(verifyFrame)) {
                        auto verifyFace = m_detector->DetectLargestFace(verifyFrame);
                        if (verifyFace) {
                            std::optional<CredentialStore::MatchResult> verifyMatch;
                            if (m_onnxRecognizer) {
                                auto onnxEmb = m_onnxRecognizer->ComputeEmbedding(verifyFrame, verifyFace->landmarks);
                                if (!onnxEmb.empty()) {
                                    verifyMatch = m_store->FindBestMatch(onnxEmb.data(), m_matchThreshold);
                                }
                            } else {
                                auto verifyEmbedding = m_recognizer->ComputeEmbedding(verifyFrame, verifyFace->landmarks);
                                if (verifyEmbedding.size() > 0) {
                                    verifyMatch = m_store->FindBestMatch(&verifyEmbedding(0), m_matchThreshold);
                                }
                            }
                            if (!verifyMatch) {
                                FACELOGIN_WARN(L"Final match verify failed \u2014 face swap detected");
                                m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(
                                    L"\u6d3b\u4f53\u9a8c\u8bc1\u671f\u95f4\u4eba\u8138\u4e0d\u5339\u914d\uff0c\u8bf7\u91cd\u8bd5"));
                                FlushFileBuffers(m_pipeServer->GetHandle());
                                {
                                    wchar_t dummy[64]; DWORD bytesRead = 0;
                                    ReadFile(m_pipeServer->GetHandle(), dummy, sizeof(dummy), &bytesRead, nullptr);
                                }
                                SecureZeroMemory(match->password.data(), match->password.size() * sizeof(wchar_t));
                                return false;
                            }
                        }
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

            {
                wchar_t dummy[64];
                DWORD bytesRead = 0;
                ReadFile(m_pipeServer->GetHandle(), dummy, sizeof(dummy),
                         &bytesRead, nullptr);
            }
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
