#include "FaceService.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include "../common/secure_buffer.h"
#include "../common/registry_util.h"
#include "../common/boot_evidence.h"
#include "../common/session_util.h"
#include "../common/config_util.h"
#include "../common/image_utils.h"
#include "liveness_detector.h"
#include <shlobj.h>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cmath>
#include <wtsapi32.h>
#include <psapi.h>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "psapi.lib")

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

// A match result contains the credential payload needed only after a complete
// authentication succeeds.  Consensus and anti-spoof binding must instead use
// a stable account identity, so two different enrolled users can never lend
// their match and liveness evidence to one another.
static std::wstring MatchAccountKey(const CredentialStore::MatchResult& match) {
    if (!match.sid.empty()) return L"sid:" + match.sid;
    if (!match.upn.empty()) return L"upn:" + match.upn;
    return L"name:" + match.username;
}

static bool SameMatchedAccount(const CredentialStore::MatchResult& match,
                               const std::wstring& accountKey) {
    return !accountKey.empty() &&
           CompareStringOrdinal(MatchAccountKey(match).c_str(), -1,
                                accountKey.c_str(), -1, TRUE) == CSTR_EQUAL;
}

FaceService::FaceService() {
    s_pInstance = this;
}

FaceService::~FaceService() {
    CleanupSessionResources();
    if (m_wicFactory) m_wicFactory->Release();
    m_wicFactory = nullptr;
    s_pInstance = nullptr;
}

// ============================================================================
// Unknown-face capture (opt-in, config-gated)
// ============================================================================

bool FaceService::EnsureWicFactory() {
    if (m_wicFactory) return true;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&m_wicFactory));
    return m_wicFactory != nullptr;
}

void FaceService::SaveUnknownFace(const dlib::matrix<dlib::rgb_pixel>& frame,
                                  float bestDistance) {
    if (frame.size() == 0) return;
    FACELOGIN_INFO(L"SaveUnknownFace: enter (frame=%ldx%ld, dist=%.3f)",
                   frame.nc(), frame.nr(), bestDistance);

    // Storage: <dataDir>\data\unknown\ — dataDir follows the user-chosen
    // install directory (registry DataPath), not fixed to C:.
    std::wstring dir = m_dataDir + L"\\data\\unknown";
    CreateDirectoryW(dir.c_str(), nullptr);

    // Rolling cap: keep at most kMaxUnknownFaces files, delete the oldest.
    constexpr int kMaxUnknownFaces = 100;
    {
        std::vector<std::wstring> files;
        WIN32_FIND_DATAW fd = {};
        HANDLE hFind = FindFirstFileW((dir + L"\\*.jpg").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    files.push_back(fd.cFileName);
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
        std::sort(files.begin(), files.end());   // chronological by filename
        while (static_cast<int>(files.size()) >= kMaxUnknownFaces) {
            std::wstring old = dir + L"\\" + files.front();
            DeleteFileW(old.c_str());
            files.erase(files.begin());
        }
    }
    FACELOGIN_INFO(L"SaveUnknownFace: dir ready, WIC=%d", EnsureWicFactory() ? 1 : 0);

    // Filename: timestamp + sequence (never user input — no path injection).
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"%04d%02d%02d-%02d%02d%02d-%03d.jpg",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::wstring jpgPath = dir + L"\\" + name;

    // Encode the full-res frame as JPEG via WIC. Create the factory per call
    // (instead of caching it on the service): a cached WIC factory reused
    // across consecutive calls crashed with an access violation (0xc0000005)
    // inside the encoder on this SYSTEM-session machine — creating + releasing
    // per call sidesteps whatever state the encoder keeps between uses.
    {
        IWICImagingFactory* wic = nullptr;
        HRESULT wicHr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                         CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&wic));
        if (SUCCEEDED(wicHr) && wic) {
            int w = static_cast<int>(frame.nc());
            int h = static_cast<int>(frame.nr());
            FACELOGIN_INFO(L"SaveUnknownFace: encoding %dx%d via WIC", w, h);
            std::vector<BYTE> bgra(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; y++) {
                BYTE* row = bgra.data() + static_cast<size_t>(y) * w * 4;
                for (int x = 0; x < w; x++) {
                    const auto& p = frame(y, x);
                    row[x * 4 + 0] = p.blue;
                    row[x * 4 + 1] = p.green;
                    row[x * 4 + 2] = p.red;
                    row[x * 4 + 3] = 255;
                }
            }

            IWICBitmap* pBitmap = nullptr;
            HRESULT hr = wic->CreateBitmapFromMemory(
                w, h, GUID_WICPixelFormat32bppBGR, w * 4,
                static_cast<UINT>(bgra.size()), bgra.data(), &pBitmap);
            if (SUCCEEDED(hr)) {
                IWICBitmapEncoder* pEncoder = nullptr;
                if (SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatJpeg,
                                                 nullptr, &pEncoder))) {
                IStream* pStream = nullptr;
                if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream))) {
                    if (SUCCEEDED(pEncoder->Initialize(pStream, WICBitmapEncoderNoCache))) {
                        IWICBitmapFrameEncode* pFrameEncode = nullptr;
                        IPropertyBag2* pProps = nullptr;
                        if (SUCCEEDED(pEncoder->CreateNewFrame(&pFrameEncode, &pProps))) {
                            PROPBAG2 opt = {};
                            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                            VARIANT v;
                            VariantInit(&v);
                            v.vt = VT_R4;
                            v.fltVal = 0.70f;
                            pProps->Write(1, &opt, &v);
                            VariantClear(&v);
                            if (SUCCEEDED(pFrameEncode->Initialize(pProps)) &&
                                SUCCEEDED(pFrameEncode->SetSize(w, h)) &&
                                SUCCEEDED(pFrameEncode->WriteSource(pBitmap, nullptr))) {
                                pFrameEncode->Commit();
                                pEncoder->Commit();

                                STATSTG stat;
                                if (SUCCEEDED(pStream->Stat(&stat, STATFLAG_NONAME))) {
                                    ULONG jpgSize = static_cast<ULONG>(stat.cbSize.QuadPart);
                                    std::vector<BYTE> jpg(jpgSize);
                                    LARGE_INTEGER li = {};
                                    pStream->Seek(li, STREAM_SEEK_SET, nullptr);
                                    ULONG read = 0;
                                    if (SUCCEEDED(pStream->Read(jpg.data(), jpgSize, &read))) {
                                        std::ofstream f(jpgPath, std::ios::binary);
                                        if (f) {
                                            f.write(reinterpret_cast<const char*>(jpg.data()), jpgSize);
                                            f.close();
                                        }
                                    }
                                }
                            }
                            pFrameEncode->Release();
                            pProps->Release();
                        }
                    }
                    pStream->Release();
                }
                pEncoder->Release();
            }
            pBitmap->Release();
            }
            wic->Release();
        }
    }

    FACELOGIN_INFO(L"SaveUnknownFace: jpg written, appending event");
    // Append the event record (JSONL): timestamp, reason, best distance, file.
    {
        // Filename is ASCII (digits/dashes/dots) — convert for the UTF-8 file.
        char nameUtf8[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, nameUtf8, 64, nullptr, nullptr);
        std::ofstream ev(m_dataDir + L"\\data\\unknown\\events.jsonl", std::ios::app);
        if (ev) {
            ev << "{";
            ev << "\"ts\":\"" << st.wYear << "-" << st.wMonth << "-" << st.wDay
               << " " << st.wHour << ":" << st.wMinute << ":" << st.wSecond << "\",";
            ev << "\"reason\":\"NO_MATCH\",";
            ev << "\"bestDistance\":" << bestDistance << ",";
            ev << "\"file\":\"" << nameUtf8 << "\"";
            ev << "}\n";
        }
    }

    FACELOGIN_WARN(L"Unknown face diagnostic frame captured (distance=%.3f)",
                   bestDistance);
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
        if (eventType == PBT_APMRESUMESUSPEND ||
            eventType == PBT_APMRESUMEAUTOMATIC) {
            FACELOGIN_INFO(L"Power resume event — refreshing boot evidence");
            pService->QueueServiceEvent(
                ServiceEventType::KernelBootRefresh, 0xFFFFFFFF);
        }
        return NO_ERROR;
    }
    case SERVICE_CONTROL_SESSIONCHANGE: {
        // Session notifications describe the login state directly. Only a
        // logoff starts a new login-entry generation; lock/unlock do not.
        auto* evt = reinterpret_cast<WTSSESSION_NOTIFICATION*>(eventData);
        if (evt && evt->cbSize == sizeof(WTSSESSION_NOTIFICATION)) {
            const DWORD activeSession = WTSGetActiveConsoleSessionId();
            if (evt->dwSessionId == activeSession || activeSession == 0xFFFFFFFF) {
                if (eventType == WTS_SESSION_LOGOFF) {
                    pService->QueueServiceEvent(
                        ServiceEventType::SessionLogoff, evt->dwSessionId);
                } else if (eventType == WTS_SESSION_LOGON) {
                    // Service startup can precede creation of the interactive
                    // console session. Retry the current Kernel-Boot record
                    // now that this session is available; do not complete the
                    // generation here, because LogonUI still needs to claim it.
                    pService->QueueServiceEvent(
                        ServiceEventType::KernelBootRefresh, evt->dwSessionId);
                } else if (eventType == WTS_SESSION_DESKTOP_READY) {
                    pService->QueueServiceEvent(
                        ServiceEventType::LoginEntryCompleted, evt->dwSessionId);
                } else if (eventType == WTS_SESSION_LOCK) {
                    // Locking can be emitted while LogonUI is preparing a
                    // fresh cold-start entry. It is not proof that an entry
                    // has completed, so it must not consume its generation.
                    FACELOGIN_INFO(L"Console session locked: session=%lu (generation retained)",
                                   evt->dwSessionId);
                } else if (eventType == WTS_SESSION_UNLOCK) {
                    // WTS_SESSION_DESKTOP_READY is not consistently delivered
                    // after a successful interactive logon.  UNLOCK is the
                    // reliable evidence that this session has reached its
                    // desktop, so finish any active cold-start/logoff entry
                    // here as well. CompleteLoginEntryGeneration() is
                    // idempotent and therefore a no-op for ordinary Win+L
                    // unlocks, whose generation was already completed.
                    pService->QueueServiceEvent(
                        ServiceEventType::LoginEntryCompleted, evt->dwSessionId);
                    FACELOGIN_INFO(L"Console session unlocked: session=%lu "
                                   L"(completing active login entry)",
                                   evt->dwSessionId);
                }
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
    m_modelsDir = m_dataDir + L"\\models";

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
    FACELOGIN_INFO(L"Data and model directories initialized");

    m_store = std::make_unique<CredentialStore>();
    m_store->SetDataDir(m_dataDir);
    if (!m_store->LoadDatabase()) {
        FACELOGIN_ERROR(L"Failed to load credential database");
        return false;
    }
    m_adaptiveLearning.SetDataDir(m_dataDir);
    if (!m_adaptiveLearning.Load()) {
        FACELOGIN_WARN(L"Adaptive learning archive could not be loaded; continuing without it");
    }
    FACELOGIN_INFO(L"Loaded %zu registered user(s)", m_store->GetUserCount());

    ProcessKernelBootEvidence(L"service-start");

    m_detector = nullptr;  // loaded by the background thread — see StartBackgroundModelLoad()

    // SCRFD (m_onnxDetector) is now also loaded by the background thread
    // (see LoadHeavyModels step 0) and is unloaded alongside the other models
    // when unload_models_after_auth is on. The pipe listener can still be up
    // as fast as before: auth waits on EnsureModelsLoaded() (which blocks
    // until the background loader finishes, or loads synchronously if models
    // were unloaded after a prior auth).
    // NOTE: this keeps the idle service at zero model memory when the
    // "内存优化" setting is enabled (previously SCRFD stayed resident).

    // Heavy models (2d106det + recognizer + anti-spoof + SCRFD) load in a
    // background thread. See StartBackgroundModelLoad().
    StartBackgroundModelLoad();

    if (m_isServiceMode) {
        // Camera is initialized lazily per auth request to avoid
        // device contention with the console app. See Run().
        // MF is preferred so unlock images come from the SAME pipeline the
        // enrollment console uses (DirectShow colour/gain output differs →
        // systematic ~0.4 embedding offset). DS remains the Session 0
        // fallback when MF cannot initialize.
FACELOGIN_INFO(L"Camera pipeline: MF preferred, DirectShow fallback — initialized on demand%s",
                       m_config.camera_device.empty() ? L"" : L" (configured device)");
    } else {
        // Media Foundation camera is also initialized on demand — keeping
        // it open across auth sessions causes the source reader to stall
        // (especially when FaceLoginConsole is running concurrently).
        FACELOGIN_INFO(L"MF camera will be initialized on demand%s",
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
// Lazy model loading
//
// The pipe listener must be up as soon as possible so the credential provider
// connects when the lock screen appears. Loading all ONNX sessions
// synchronously delays cold boot, so the detector and remaining sessions load
// in a background thread before the pipe loop. If a request arrives first,
// EnsureModelsLoaded() waits until the sessions are ready or loading fails.
// ============================================================================

bool FaceService::LoadHeavyModels() {
    FACELOGIN_INFO(L"Loading heavy models in background...");

    // 0. SCRFD face detector (det_500m, ~22MB? — actually ~2.5MB). Moved into
    // the background loader so the idle service holds ZERO model memory when
    // unload_models_after_auth is on. Auth waits for all of these via
    // EnsureModelsLoaded().
    {
        auto detector = std::make_unique<OnnxDetector>();
        std::wstring path = m_modelsDir + L"\\det_500m.onnx";
        if (!detector->Initialize(path)) {
            FACELOGIN_ERROR(L"SCRFD detector failed to load — face detection unavailable");
            return false;
        }
        std::lock_guard<std::mutex> lock(m_modelMutex);
        m_onnxDetector = std::move(detector);
    }
        FACELOGIN_DEBUG(L"Model ready: detector");

    // Head pose is an optional observer. It is loaded with the heavy model
    // group so the service and Console use the same model asset and lifetime,
    // but failure never blocks face authentication.
    {
        auto headPose = std::make_unique<OnnxHeadPose>();
        std::wstring path = m_modelsDir + L"\\head_pose_mobilenetv2.onnx";
        if (headPose->Initialize(path)) {
            std::lock_guard<std::mutex> lock(m_modelMutex);
            m_headPose = std::move(headPose);
            FACELOGIN_DEBUG(L"Model ready: head pose");
        } else {
            FACELOGIN_WARN(L"MobileNetV2 head-pose model unavailable; pose logging disabled");
        }
    }

    // 1. 106-point landmark detector (2d106det.onnx).
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
    FACELOGIN_DEBUG(L"Model ready: landmarks");

    // 2. InsightFace recognizer (w600k_mbf.onnx).
    {
        auto recognizer = std::make_unique<OnnxRecognizer>();
        std::wstring path = m_modelsDir + L"\\w600k_mbf.onnx";
        if (!recognizer->Initialize(path)) {
            FACELOGIN_ERROR(L"ONNX recognizer failed to load — recognition unavailable");
            return false;
        }
        std::lock_guard<std::mutex> lock(m_modelMutex);
        m_onnxRecognizer = std::move(recognizer);
    }
    FACELOGIN_DEBUG(L"Model ready: recognizer");

    // 3. Anti-spoof model (facenox MiniFAS, 1.6.0 — replaces DeepPixBiS/OULU).
    {
        auto antiSpoof = std::make_unique<OnnxAntiSpoof>();
        std::wstring path = m_modelsDir + L"\\minifas_quantized.onnx";
        if (antiSpoof->Initialize(path)) {
            std::lock_guard<std::mutex> lock(m_modelMutex);
            m_antiSpoof = std::move(antiSpoof);
            FACELOGIN_DEBUG(L"Model ready: anti-spoof MiniFAS");
        } else {
            // Fall back to the legacy OULU model if present.
            std::wstring ouluPath = m_modelsDir + L"\\OULU_Protocol_2_model_0_0.onnx";
            auto oulu = std::make_unique<OnnxAntiSpoof>();
            if (oulu->Initialize(ouluPath)) {
                std::lock_guard<std::mutex> lock(m_modelMutex);
                m_antiSpoof = std::move(oulu);
                FACELOGIN_DEBUG(L"Model ready: anti-spoof fallback");
            } else {
                FACELOGIN_WARN(L"Anti-spoof model not available");
            }
        }
    }

    // NOTE: liveness-method validation (anti-spoof → blink fallback) is NOT
    // done here. It touches m_livenessMethod / m_antiSpoof, which the main
    // thread (CONFIG_RELOAD) also mutates — doing it on this background thread
    // would race. It happens on the main thread in ValidateLivenessMethod(),
    // called after the models are known ready.

    FACELOGIN_INFO(L"Models ready: detector, landmarks, recognizer%s",
                   m_antiSpoof ? L", anti-spoof" : L"");
    return true;
}

// Release all heavy-model memory (SCRFD + 2d106det + recognizer + anti-spoof
// sessions) after an auth completes, so the idle service's RSS drops back to
// ~baseline (zero model memory — 1.9.0: SCRFD is included now; previously it
// stayed resident for the first frame, keeping ~22MB peak-ish resident).
// The next auth's EnsureModelsLoaded() reloads them synchronously (~200-500ms
// of startup latency, a deliberate trade for a low idle footprint — 1.6.0).
// Only called from the main pipe thread after the auth pipeline has fully
// unwound, so no inference is in flight.
void FaceService::UnloadHeavyModels() {
    std::lock_guard<std::mutex> lock(m_modelMutex);
    if (m_onnxDetector)     { m_onnxDetector.reset(); }
    if (m_headPose)         { m_headPose.reset(); }
    if (m_detector)         { m_detector.reset(); }
    if (m_onnxRecognizer)   { m_onnxRecognizer.reset(); }
    if (m_antiSpoof)        { m_antiSpoof.reset(); }
    m_modelState.store(ModelLoadState::NotLoaded);
    FACELOGIN_INFO(L"Heavy models unloaded after auth");
}

// Empty the process working set so freed model pages leave the RESIDENT set
// (heap free returns the memory to the allocator but Windows keeps the pages
// resident until they're trimmed or evicted — observable as high RSS while
// "idle"). Pages are paged out to the standby list and re-faulted on next
// access; combined with UnloadHeavyModels() the service's idle footprint after
// auth drops to the true baseline. Called only on the auth-completion path.
void FaceService::TrimWorkingSet() {
    // Log RSS before/after so the benefit is measurable from service.log.
    PROCESS_MEMORY_COUNTERS pmc = {};
    pmc.cb = sizeof(PROCESS_MEMORY_COUNTERS);
    SIZE_T beforeWs = 0, afterWs = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        beforeWs = pmc.WorkingSetSize;
    }

    BOOL ok = SetProcessWorkingSetSizeEx(
        GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);

    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        afterWs = pmc.WorkingSetSize;
    }
    FACELOGIN_INFO(L"TrimWorkingSet: %s (WS %llu KB -> %llu KB)",
                   ok ? L"ok" : L"FAILED",
                   static_cast<unsigned long long>(beforeWs / 1024),
                   static_cast<unsigned long long>(afterWs / 1024));
}

void FaceService::StartBackgroundModelLoad() {
    m_modelState.store(ModelLoadState::Loading);
    m_modelLoadThread = std::thread([this]() {
        // Load under a scoped RAII so the flags are cleared on every exit path
        // (including exceptions).
        struct LoadGuard {
            FaceService* svc;
            bool ok;
            ~LoadGuard() {
                if (svc->m_modelState.load() != ModelLoadState::Stopping) {
                    svc->m_modelState.store(ok ? ModelLoadState::Ready
                                               : ModelLoadState::Failed);
                }
                svc->m_modelCv.notify_all();
            }
        };
        bool ok = false;
        try {
            ok = LoadHeavyModels();
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

// Called from the main auth path before the first inference. Returns true if
// the heavy models are ready, loading them SYNCHRONOUSLY if they were unloaded
// after the previous auth (1.6.0: UnloadHeavyModels frees model memory when the
// service is idle). Blocks while the startup background loader is still running.
// Returns false only if a REQUIRED model failed to load (auth cannot proceed)
// or the service is stopping.
bool FaceService::EnsureModelsLoaded() {
    if (m_modelState.load() == ModelLoadState::Ready) return true;
    if (m_modelState.load() == ModelLoadState::Stopping) return false;

    // Unloaded after a prior auth, or the startup background load is still in
    // flight. Wait for any in-flight load; if none is running, load now on this
    // thread (synchronous, ~200-500ms).
    {
        std::unique_lock<std::mutex> lock(m_modelMutex);
        m_modelCv.wait(lock, [this]() {
            return m_modelState.load() != ModelLoadState::Loading;
        });
        if (m_modelState.load() == ModelLoadState::Ready) return true;
        if (m_modelState.load() == ModelLoadState::Failed) return false;
        if (m_modelState.load() == ModelLoadState::Stopping) return false;
    }

    // No load in flight and not ready — load synchronously on this thread.
    bool ok = LoadHeavyModels();
    m_modelState.store(ok ? ModelLoadState::Ready : ModelLoadState::Failed);
    m_modelCv.notify_all();
    return ok;
}

// Release anyone blocked in EnsureModelsLoaded() during service shutdown so
// Stop() can join the loader thread without deadlocking.
void FaceService::AbortModelLoadWait() {
    m_modelState.store(ModelLoadState::Stopping);
    m_modelCv.notify_all();
    if (m_modelLoadThread.joinable()) {
        m_modelLoadThread.join();
    }
}

void FaceService::Run() {
    m_stopRequested.store(false);

    while (!m_stopRequested.load()) {
        ProcessPendingServiceEvents();
        if (!m_pipeServer->WaitForClient(60000)) {
            if (m_stopRequested.load()) break;
            continue;
        }

        std::wstring request;
        if (!m_pipeServer->ReadMessage(request, 30000)) {
            m_pipeServer->Disconnect();
            continue;
        }

        if (request == ipc::MSG_AUTH_REQUEST) {
            FACELOGIN_INFO(L"IPC: authentication request");
        } else if (request == ipc::MSG_RELOAD_DB) {
            FACELOGIN_INFO(L"IPC: database reload request");
        } else if (request == ipc::MSG_CONFIG_RELOAD) {
            FACELOGIN_INFO(L"IPC: configuration reload request");
        }

        if (request == ipc::MSG_RELOAD_DB) {
            m_store->ReloadDatabase();   // force re-read (LoadDatabase is cached)
            m_adaptiveLearning.Reload();
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
                m_pipeServer->Disconnect();
                continue;
            }

            // Now that the models are ready (loader finished), the pointer
            // mutations below are safe on the main thread.

            // Retry loading anti-spoof model if configured and not yet loaded
            if (m_livenessMethod == LivenessMethod::AntiSpoof && (!m_antiSpoof || !m_antiSpoof->IsInitialized())) {
                m_antiSpoof = std::make_unique<OnnxAntiSpoof>();
                std::wstring antiSpoofPath = m_modelsDir + L"\\minifas_quantized.onnx";
                if (m_antiSpoof->Initialize(antiSpoofPath)) {
                    FACELOGIN_INFO(L"CONFIG_RELOAD: anti-spoof model loaded successfully");
                } else {
                    FACELOGIN_WARN(L"CONFIG_RELOAD: anti-spoof still unavailable, falling back to blink");
                    m_livenessMethod = LivenessMethod::Blink;
                    m_antiSpoof.reset();
                }
            }
            m_pipeServer->Disconnect();
            FACELOGIN_INFO(L"Configuration reloaded: rec=%hs det=%hs live=%hs thr=%.2f rotation=%d",
                          m_config.recognition_model.c_str(), m_config.detector.c_str(),
                          m_livenessMethod == LivenessMethod::Blink ? "blink" :
                          m_livenessMethod == LivenessMethod::AntiSpoof ? "antispoof" : "none",
                          m_matchThreshold, m_config.camera_rotation);
        }
        else if (request == ipc::MSG_AUTH_REQUEST) {
            HandleAuthRequest();
            m_pipeServer->Disconnect();
        }
        else {
            FACELOGIN_WARN(L"Unknown pipe request (chars=%zu)", request.size());
            m_pipeServer->Disconnect();
        }
    }

    CleanupSessionResources();
}

void FaceService::ProcessKernelBootEvidence(const wchar_t* reason) {
    const KernelBootEvidence evidence = QueryLatestKernelBootEvidence();
    if (!evidence.valid) {
        FACELOGIN_WARN(L"KernelBoot: reason=%s query failed error=%lu",
                       reason, evidence.error);
        return;
    }

    const bool createsLoginEntry = evidence.kind == KernelBootKind::FullStartup ||
        evidence.kind == KernelBootKind::FastStartup;
    const DWORD sessionId = WTSGetActiveConsoleSessionId();
    const KernelBootRegistration registration = RegisterKernelBootEvidence(
        evidence.recordId, sessionId, createsLoginEntry);

    FACELOGIN_INFO(L"KernelBoot: reason=%s record=%llu bootType=%lu kind=%s "
                   L"sessionId=%lu createsLoginEntry=%d observed=%d seeded=%d "
                   L"generation=%llu created=%d",
                   reason, evidence.recordId, evidence.bootType,
                   KernelBootKindName(evidence.kind), sessionId,
                   static_cast<int>(createsLoginEntry),
                   static_cast<int>(registration.observed),
                   static_cast<int>(registration.seeded), registration.generation,
                   static_cast<int>(registration.createdLoginEntry));
}

void FaceService::QueueServiceEvent(ServiceEventType type, DWORD sessionId) {
    {
        std::lock_guard<std::mutex> lock(m_serviceEventMutex);
        m_pendingServiceEvents.push_back({type, sessionId});
    }
    if (m_pipeServer) m_pipeServer->WakeWait();
}

void FaceService::ProcessPendingServiceEvents() {
    std::vector<ServiceEvent> events;
    {
        std::lock_guard<std::mutex> lock(m_serviceEventMutex);
        events.swap(m_pendingServiceEvents);
    }

    for (const auto& event : events) {
        switch (event.type) {
        case ServiceEventType::KernelBootRefresh:
            ProcessKernelBootEvidence(L"service-event");
            if (event.sessionId != 0xFFFFFFFF) {
                FACELOGIN_INFO(L"Console session logon: session=%lu", event.sessionId);
            }
            break;
        case ServiceEventType::SessionLogoff: {
            const ULONGLONG generation = BeginLoginEntryGeneration(event.sessionId);
            FACELOGIN_INFO(L"LoginEntry generation published: reason=session-logoff "
                           L"sessionId=%lu generation=%llu",
                           event.sessionId, generation);
            break;
        }
        case ServiceEventType::LoginEntryCompleted:
            CompleteLoginEntryGeneration(event.sessionId);
            FACELOGIN_INFO(L"Login entry completed: session=%lu", event.sessionId);
            break;
        }
    }
}

void FaceService::Stop() {
    m_stopRequested.store(true);
    // The SCM callback only requests cancellation. Camera and model objects
    // are owned and released by the Run() thread after the
    // active request has unwound.
    m_modelState.store(ModelLoadState::Stopping);
    m_modelCv.notify_all();
    if (m_pipeServer) m_pipeServer->RequestStop();
}

void FaceService::CleanupSessionResources() {
    // Called by the Run/destructor owner thread only. It is intentionally
    // idempotent so every exit path has one cleanup boundary.
    ReleaseCamera();
    AbortModelLoadWait();
    if (m_pipeServer) m_pipeServer->Close();
}

// ============================================================================
// Camera lifecycle — MF preferred, DirectShow fallback
// ============================================================================

bool FaceService::EnsureCameraForAuth() {
    if (m_isServiceMode) {
        // The camera is released after every auth, so normally nothing is up
        // when we get here — these two checks only cover a mid-auth fallback
        // where the pipeline already switched.
        if (m_cameraPipeline == CameraPipeline::MF && m_webcamMF &&
            m_webcamMF->IsInitialized()) {
            return true;
        }
        if (m_cameraPipeline == CameraPipeline::DS && m_webcamDS &&
            m_webcamDS->IsInitialized()) {
            return true;
        }

        // Prefer Media Foundation: it is the SAME pipeline the enrollment
        // console uses. Matching requires enrollment and unlock images from
        // ONE pipeline — DirectShow's colour/gain output differs enough to
        // shift same-person embeddings by ~0.4 (observed 0.66-0.75 against a
        // 0.75 threshold), the systematic offset behind cross-environment
        // recognition failures. DS stays as the Session 0 fallback.
        m_webcamMF = std::make_unique<WebcamCapture>();
        if (m_webcamMF->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
            m_cameraPipeline = CameraPipeline::MF;
            FACELOGIN_INFO(L"MF camera initialized on demand for auth (preferred pipeline)");
            return true;
        }
        FACELOGIN_WARN(L"MF camera init failed in service mode — falling back to DirectShow");
        m_webcamMF.reset();
        m_cameraPipeline = CameraPipeline::None;

        m_webcamDS = std::make_unique<WebcamCaptureDS>();
        if (m_webcamDS->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
            m_cameraPipeline = CameraPipeline::DS;
            FACELOGIN_INFO(L"DS camera initialized on demand for auth (fallback pipeline)");
            return true;
        }
        FACELOGIN_ERROR(L"Both MF and DS camera init failed in service mode");
        m_webcamDS.reset();
        m_cameraPipeline = CameraPipeline::None;
        return false;
    }

    // Standalone: Media Foundation only. Every completed authentication
    // releases the camera, so each later attempt starts with a fresh reader.
    if (!m_webcamMF) {
        m_webcamMF = std::make_unique<WebcamCapture>();
        if (!m_webcamMF->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
            FACELOGIN_ERROR(L"MF camera init failed on demand");
            m_webcamMF.reset();
            m_cameraPipeline = CameraPipeline::None;
            return false;
        }
        FACELOGIN_INFO(L"MF camera initialized on demand for auth (standalone)");
    }
    m_cameraPipeline = CameraPipeline::MF;
    return true;
}

void FaceService::ReleaseCamera() {
    if (m_cameraPipeline == CameraPipeline::MF && m_webcamMF) {
        m_webcamMF->Shutdown();
        m_webcamMF.reset();
        FACELOGIN_INFO(L"MF camera released after auth");
    } else if (m_cameraPipeline == CameraPipeline::DS && m_webcamDS) {
        m_webcamDS->Shutdown();
        m_webcamDS.reset();
        FACELOGIN_INFO(L"DS camera released after auth");
    }
    m_cameraPipeline = CameraPipeline::None;
}

bool FaceService::SendAuthTerminal(const std::wstring& message) {
    if (m_terminalSentForCurrentRequest) {
        FACELOGIN_WARN(L"Ignoring duplicate authentication terminal response");
        return false;
    }
    m_terminalSentForCurrentRequest = true;

    if (!m_pipeServer || !m_pipeServer->IsConnected()) return false;
    const bool written = m_pipeServer->WriteMessage(message);
    return written;
}

bool FaceService::HandleAuthRequest() {
    m_terminalSentForCurrentRequest = false;
    bool authenticated = false;
    if (EnsureCameraForAuth()) {
        authenticated = ProcessAuthRequest();
    } else {
        SendAuthTerminal(ipc::BuildAuthErrorMessage(
            ipc::L10N_CAMERA_UNAVAILABLE));
    }

    // The service main thread owns camera teardown for every outcome,
    // including disconnect, timeout and model failure.
    ReleaseCamera();
    if (m_config.unload_models_after_auth) {
        UnloadHeavyModels();
        TrimWorkingSet();
    }
    return authenticated;
}

bool FaceService::GrabAuthFrame(dlib::matrix<dlib::rgb_pixel>& frame) {
    bool captured = false;
    if (m_cameraPipeline == CameraPipeline::MF && m_webcamMF) {
        captured = m_webcamMF->GrabFrame(frame);
    } else if (m_cameraPipeline == CameraPipeline::DS && m_webcamDS) {
        captured = m_webcamDS->GrabFrame(frame);
    }
    if (captured) RotateFrame(frame, m_config.camera_rotation);
    return captured;
}

bool FaceService::PrepareAuthFaceFrame(
    dlib::matrix<dlib::rgb_pixel>& frame,
    dlib::rectangle& faceRect,
    dlib::full_object_detection& landmarks,
    HeadPoseStats* pose) {
    if (pose) *pose = {};

    auto detect = [this, &faceRect, &landmarks](
                      const dlib::matrix<dlib::rgb_pixel>& candidate) {
        const auto detected = m_onnxDetector->DetectLargestFace(candidate);
        if (!detected) return false;
        faceRect = dlib::rectangle(
            static_cast<long>(detected->x1), static_cast<long>(detected->y1),
            static_cast<long>(detected->x2), static_cast<long>(detected->y2));
        landmarks = dlib::full_object_detection();
        return m_detector->DetectLandmarks(candidate, faceRect, landmarks);
    };

    if (!detect(frame)) return false;

    if (pose && m_headPose && m_headPose->IsInitialized()) {
        *pose = m_headPose->Estimate(frame, faceRect);
    }
    return true;
}

std::optional<CredentialStore::MatchResult> FaceService::MatchEmbedding(
    const std::vector<float>& embedding) {
    if (!m_store || embedding.empty()) return std::nullopt;

    // The existing normal-template path stays first and unchanged. Adaptive
    // representatives are only a recovery path for a normal nearest candidate
    // that missed the distance threshold but still passed account ambiguity.
    if (auto normal = m_store->FindBestMatch(embedding.data(), embedding.size(), m_matchThreshold)) {
        return normal;
    }

    const auto candidate = m_store->FindNearestCandidate(embedding.data(), embedding.size());
    if (!candidate || !candidate->ratioAccepted || candidate->matchedFaceId == 0) {
        return std::nullopt;
    }
    const auto adaptiveDistance = m_adaptiveLearning.FindBestDistance(
        candidate->sid, candidate->matchedFaceId, embedding.data(), embedding.size());
    if (!adaptiveDistance || *adaptiveDistance >= m_matchThreshold) return std::nullopt;
    return m_store->ResolveCandidate(*candidate, *adaptiveDistance);
}

const wchar_t* FaceService::PoseStatusKey(
    const HeadPoseStats& pose,
    const HeadPoseEvaluation& evaluation) {
    if (evaluation.legality == HeadPoseLegality::Invalid) {
        return ipc::L10N_POSE_INVALID;
    }
    if (evaluation.legality == HeadPoseLegality::Front) {
        return ipc::L10N_RECOGNIZING;
    }
    if (evaluation.legality == HeadPoseLegality::Acceptable) {
        return ipc::L10N_POSE_ACCEPTABLE;
    }

    const float yawScore = std::abs(pose.yaw) / 25.0f;
    const float pitchScore = std::abs(pose.pitch) / 18.0f;
    const float rollScore = std::abs(pose.roll) / 18.0f;
    if (yawScore >= pitchScore && yawScore >= rollScore) {
        return pose.yaw > 0.0f ? ipc::L10N_POSE_YAW_LEFT
                               : ipc::L10N_POSE_YAW_RIGHT;
    }
    if (pitchScore >= rollScore) {
        return pose.pitch > 0.0f ? ipc::L10N_POSE_PITCH_DOWN
                                 : ipc::L10N_POSE_PITCH_UP;
    }
    return pose.roll > 0.0f ? ipc::L10N_POSE_ROLL_RIGHT
                            : ipc::L10N_POSE_ROLL_LEFT;
}

FaceService::AuthFrameResult FaceService::AcquireLegalPoseFrame(
    dlib::matrix<dlib::rgb_pixel>& frame,
    dlib::rectangle& faceRect,
    dlib::full_object_detection& landmarks,
    HeadPoseStats& pose,
    HeadPoseStabilizer& poseStabilizer,
    const std::function<void(const wchar_t*)>& publishStatus) {
    if (!GrabAuthFrame(frame)) return AuthFrameResult::NoFrame;
    if (!PrepareAuthFaceFrame(frame, faceRect, landmarks, &pose)) {
        publishStatus(ipc::L10N_POSE_INVALID);
        return AuthFrameResult::Invalid;
    }

    const HeadPoseStabilityResult stability = poseStabilizer.Update(pose);
    pose = stability.pose;
    const HeadPoseEvaluation evaluation = stability.evaluation;
    if (stability.pending) return AuthFrameResult::Pending;
    if (!evaluation.accepted) {
        publishStatus(PoseStatusKey(pose, evaluation));
        return AuthFrameResult::Rejected;
    }
    return AuthFrameResult::Accepted;
}

bool FaceService::ProcessAuthRequest() {
    const char* camName = m_cameraPipeline == CameraPipeline::MF ? "MF" :
                          m_cameraPipeline == CameraPipeline::DS ? "DS" : "none";
    FACELOGIN_INFO(L"Starting face authentication... (camera=%hs)", camName);

    if (m_store->GetUserCount() == 0) {
        FACELOGIN_WARN(L"No registered users");
        SendAuthTerminal(ipc::BuildAuthErrorMessage(ipc::L10N_NO_REGISTERED_USERS));
        return false;
    }

    // E: If the heavy models are still loading in the background (unusually
    // fast lock screen right after boot), tell the lock screen what's
    // happening instead of silently blocking. The CP updates the tile's status
    // text live via STATUS: — the payload is the locale key
    // (ipc::L10N_LOADING_MODELS) and the CP translates it, so the user sees
    // "正在加载模型..." rather than a frozen "识别中". The message is NOT
    // flushed until after the wait below — if the models are already ready,
    // this whole block is a no-op and no extra STATUS message is sent.
    if (m_modelState.load() == ModelLoadState::Loading) {
        m_pipeServer->WriteMessage(std::wstring(ipc::MSG_STATUS_PREFIX) + ipc::L10N_LOADING_MODELS);
    }

    // Heavy models (2d106det + recognizer, optionally anti-spoof) load
    // in the background during startup. Normally they're ready by the time the
    // user triggers auth; if the lock screen appeared unusually fast, block
    // here until they finish. The auth timeout is running from when the CP
    // connected, so this only ever costs the tail of the boot time.
    if (!EnsureModelsLoaded()) {
        FACELOGIN_ERROR(L"Required models not loaded \u2014 cannot authenticate");
        SendAuthTerminal(ipc::BuildAuthErrorMessage(ipc::L10N_MODEL_LOAD_FAILED));
        return false;
    }

    // Models are now known loaded \u2014 validate the liveness method once (e.g.
    // anti-spoof configured but model unavailable \u2192 fall back to blink). This
    // ran on the main thread only (see LoadHeavyModels: the loader never
    // touches liveness method), so no race with CONFIG_RELOAD.
    ValidateLivenessMethod();

    // The first two frames only allow the capture backend to become readable.
    {
        constexpr int kWarmupMaxFrames = 2;
        dlib::matrix<dlib::rgb_pixel> warmFrame;
        int dropped = 0;

        for (; dropped < kWarmupMaxFrames; dropped++) {
            if (!GrabAuthFrame(warmFrame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (dropped >= kWarmupMaxFrames) {
            FACELOGIN_INFO(L"Camera readiness guard complete after %d frame attempts",
                           kWarmupMaxFrames);
        }
    }

    // STATUS: Notify credential provider that recognition has started. The
    // payload is the locale key (ipc::L10N_RECOGNIZING = "credential.recognizing");
    // the CP translates it to the lock-screen language.
    {
        std::wstring statusMsg = std::wstring(ipc::MSG_STATUS_PREFIX) + ipc::L10N_RECOGNIZING;
        m_pipeServer->WriteMessage(statusMsg);
    }

    // Intermediate status payloads are locale keys, never display text. Keep
    // the last key so a stable pose does not cause a STATUS write on every
    // camera frame.
    auto sendStatusKey = [this, lastStatusKey = std::wstring(ipc::L10N_RECOGNIZING)](
                              const wchar_t* key, bool force = false) mutable {
        if (!key || (!force && lastStatusKey == key)) return;
        if (m_pipeServer->WriteMessage(std::wstring(ipc::MSG_STATUS_PREFIX) + key)) {
            lastStatusKey = key;
        } else {
            FACELOGIN_WARN(L"Status update could not be delivered: key=%s", key);
        }
    };
    const auto publishPoseStatus = [&sendStatusKey](const wchar_t* key) {
        sendStatusKey(key);
    };

    HeadPoseStabilizer matchPoseStabilizer;
    dlib::matrix<dlib::rgb_pixel> frame;  // reused by the match loop below
    auto startTime = std::chrono::steady_clock::now();
    bool authSent = false;
    bool acceptedPoseSeen = false;
    bool poseRejectedSeen = false;
    int consecutiveMatches = 0;
    std::wstring consensusAccountKey;
    // Consecutive frames where a face WAS detected (and its embedding was
    // computed) but no enrolled face matched. After kNoMatchFailFrames such
    // frames the auth stops immediately with a "人脸匹配失败" notice instead
    // of staring at the user until the 15s timeout — a stranger (or a
    // registered user the camera can't recognize right now) gets instant
    // feedback and can retry with a key press or fall back to the password.
    // Face-less frames and embedding failures are NOT counted.
    int consecutiveNoMatch = 0;
    static constexpr int kNoMatchFailFrames = 3;
    // Consensus: how many consecutive matched frames release credentials.
    // Two consecutive frames of the SAME account establish identity before
    // liveness starts.  Anti-spoof then binds its result to that account on
    // the same frame, so no post-liveness re-match is needed.
    static constexpr int CONSENSUS_FRAMES = 2;

    while (!m_stopRequested.load()) {
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
            SendAuthTerminal(poseRejectedSeen && !acceptedPoseSeen
                                 ? ipc::MSG_AUTH_POSE_TIMEOUT
                                 : ipc::MSG_AUTH_TIMEOUT);
            return false;
        }

        if (!GrabAuthFrame(frame)) {
            if (m_stopRequested.load()) return false;
            // A stalled camera (e.g. after resume) self-shut-down in
            // GrabFrame. Rebuild it here so auth can continue instead of
            // spinning on a dead SourceReader until timeout.
            if (m_cameraPipeline == CameraPipeline::MF && m_webcamMF &&
                !m_webcamMF->IsInitialized()) {
                FACELOGIN_INFO(L"MF camera stalled — re-initializing");
                m_webcamMF->Shutdown();
                m_webcamMF.reset();
                m_webcamMF = std::make_unique<WebcamCapture>();
                if (!m_webcamMF->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
                    FACELOGIN_ERROR(L"MF camera re-init failed");
                    m_webcamMF.reset();
                    m_cameraPipeline = CameraPipeline::None;
                    // Service mode: a resume-stalled MF reader shouldn't kill
                    // the unlock — fall back to the DirectShow pipeline.
                    if (m_isServiceMode) EnsureCameraForAuth();
                }
            } else if (m_cameraPipeline == CameraPipeline::DS && m_webcamDS &&
                       !m_webcamDS->IsInitialized()) {
                FACELOGIN_INFO(L"DS camera stalled — re-initializing");
                m_webcamDS->Shutdown();
                m_webcamDS.reset();
                m_webcamDS = std::make_unique<WebcamCaptureDS>();
                if (!m_webcamDS->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
                    FACELOGIN_ERROR(L"DS camera re-init failed");
                    m_webcamDS.reset();
                    m_cameraPipeline = CameraPipeline::None;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // Detect and align directly from the camera frame.
        std::optional<CredentialStore::MatchResult> match;
        dlib::full_object_detection landmarks;
        dlib::rectangle faceRect;
        HeadPoseStats pose;
        if (!PrepareAuthFaceFrame(frame, faceRect, landmarks, &pose)) {
            sendStatusKey(ipc::L10N_POSE_INVALID);
            consecutiveMatches = 0;
            consensusAccountKey.clear();
            consecutiveNoMatch = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        const HeadPoseStabilityResult poseStability = matchPoseStabilizer.Update(pose);
        pose = poseStability.pose;
        const HeadPoseEvaluation poseEvaluation = poseStability.evaluation;
        if (poseStability.pending) {
            consecutiveMatches = 0;
            consensusAccountKey.clear();
            consecutiveNoMatch = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }
        if (!poseEvaluation.accepted) {
            sendStatusKey(PoseStatusKey(pose, poseEvaluation));
            poseRejectedSeen = true;
            consecutiveMatches = 0;
            consensusAccountKey.clear();
            consecutiveNoMatch = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        acceptedPoseSeen = true;
        sendStatusKey(PoseStatusKey(pose, poseEvaluation));

        auto onnxEmb = m_onnxRecognizer->ComputeEmbedding(
            frame, landmarks);
        if (!onnxEmb.empty()) {
            match = MatchEmbedding(onnxEmb);
        }

        if (match) {
            if (consecutiveMatches == 0 || SameMatchedAccount(*match, consensusAccountKey)) {
                ++consecutiveMatches;
            } else {
                // Do not combine consecutive matches from different enrolled
                // accounts into one authentication consensus.
                consecutiveMatches = 1;
            }
            consensusAccountKey = MatchAccountKey(*match);
            consecutiveNoMatch = 0;   // a match resets the no-match counter
            FACELOGIN_DEBUG(L"Auth frame matched: distance=%.4f "
                            L"pose=%d range=%d yaw=%.1f pitch=%.1f roll=%.1f pose_ms=%.1f "
                            L"face=%.0fx%.0f aspect=%.2f crop=%.0fx%.0f/%.2f [%d/%d]",
                           match->distance,
                          static_cast<int>(pose.quality), static_cast<int>(pose.range),
                          pose.yaw, pose.pitch,
                          pose.roll, pose.inferenceMs, pose.faceWidth,
                          pose.faceHeight, pose.faceAspect, pose.cropWidth,
                          pose.cropHeight, pose.cropAspect,
                          consecutiveMatches, CONSENSUS_FRAMES);

            if (consecutiveMatches < CONSENSUS_FRAMES) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
        } else {
            // Only count frames where a face was really present but failed to
            // match (embedding computed, no stored face close enough) — not
            // face-less frames or embedding failures.
            if (!onnxEmb.empty()) {
                if (++consecutiveNoMatch >= kNoMatchFailFrames) {
                    // Diagnostics: report the nearest distance in the single
                    // normalized input domain used by the recognizer.
                    float d0 = m_store->FindNearestDistance(onnxEmb.data(), onnxEmb.size());
                    FACELOGIN_WARN(L"Auth failed: no match after %d face frames (nearest=%.3f)",
                                   consecutiveNoMatch, d0);
                    // Opt-in unknown-face capture: save the failing frame +
                    // a JSONL event record (see SaveUnknownFace). Pass the
                    // original-image nearest distance — the variants were
                    // computed for diagnostics, the original is the distance
                    // the user actually failed at.
                    if (m_config.capture_unknown_faces) {
                        SaveUnknownFace(frame, d0);
                    }
                    // Distinct terminal message (AUTH_NO_MATCH) so the CP can
                    // show the no-match wording instead of the timeout one —
                    // a face WAS seen, it just didn't match. The STATUS: text
                    // carries the locale key (ipc::L10N_NO_MATCH); the CP
                    // translates it.
                    m_pipeServer->WriteMessage(std::wstring(ipc::MSG_STATUS_PREFIX) +
                        ipc::L10N_NO_MATCH);
                    SendAuthTerminal(ipc::MSG_AUTH_NO_MATCH);
                    return false;
                }
            }
            // Soft consensus: a miss DECAYS the counter by 1 instead of fully
            // resetting to 0. A single intermittent bad frame (motion, blink,
            // momentary profile turn, partial occlusion) then no longer forces
            // a full 3-frame restart — the user's slightly moving face stays
            // matched and auth completes in ~1-2s instead of timing out.
            //
            // Security is preserved: this only relaxes frame consensus. Blink
            // still performs its temporal liveness and fresh match check;
            // anti-spoof binds both checks to the same accepted frame.
            if (consecutiveMatches > 0) {
                consecutiveMatches--;
                if (consecutiveMatches == 0) consensusAccountKey.clear();
                FACELOGIN_DEBUG(L"Auth match consensus decayed to %d", consecutiveMatches);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        {
            // Passwordless account: no stored password. Submit a BLANK
            // credential — Windows allows blank-password accounts to
            // console-logon (lock-screen unlock included) by default policy,
            // so a genuinely passwordless account unlocks via face login.
            // If the policy forbids it or the account actually has a password,
            // LSA rejects at submission time and the user falls back to PIN.
            if (match->passwordless) {
                match->password.clear();  // defensive; store already returns empty
                FACELOGIN_INFO(L"Matched passwordless account — issuing blank-password unlock");
            }

            std::wstring domain = L".";
            wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1] = {};
            DWORD size = ARRAYSIZE(computerName);
            if (GetComputerNameW(computerName, &size)) {
                domain = computerName;
            }

            // === Liveness check ===
            {
                LivenessMethod method = m_livenessMethod;

                // Determine status text
                if (method == LivenessMethod::AntiSpoof) {
                    sendStatusKey(ipc::L10N_LIVENESS_CHECKING);
                } else if (method == LivenessMethod::Blink) {
                    sendStatusKey(ipc::L10N_BLINK_PROMPT);
                }

                bool livenessPassed = false;
                bool restartMatching = false;
                bool livenessAcceptedPoseSeen = false;
                bool livenessPoseRejected = false;
                bool livenessPosePromptActive = false;
                HeadPoseStabilizer livenessPoseStabilizer;

                if (method == LivenessMethod::None) {
                    livenessPassed = true;
                } else if (method == LivenessMethod::AntiSpoof) {
                    // Bind every anti-spoof inference to a frame that still
                    // matches the account confirmed above. The second consensus
                    // frame itself is the first sample, avoiding a redundant
                    // capture and the old post-liveness final-match pass.
                    const std::wstring candidateAccountKey = consensusAccountKey;
                    const int maxJointAttempts = AntiSpoofCheckCount(m_antiSpoofThreshold);
                    const float effectiveThreshold = AntiSpoofEffectiveThreshold(
                        m_antiSpoofThreshold, m_antiSpoof->IsFacenoxMode());
                    FACELOGIN_INFO(L"Anti-spoof: candidate confirmed; threshold=%.3f max_attempts=%d",
                                   m_antiSpoofThreshold, maxJointAttempts);
                    auto asStart = std::chrono::steady_clock::now();
                    int totalChecked = 0;

                    const auto evaluateJointFrame = [&](const dlib::matrix<dlib::rgb_pixel>& antiSpoofFrame,
                                                        const dlib::full_object_detection& antiSpoofLandmarks,
                                                        const CredentialStore::MatchResult& antiSpoofMatch) {
                        const float score = m_antiSpoof->Predict(antiSpoofFrame, antiSpoofLandmarks);
                        ++totalChecked;
                        const bool passed = score >= effectiveThreshold;
                        FACELOGIN_DEBUG(L"Anti-spoof joint attempt %d/%d: score=%.3f threshold=%.2f identity=confirmed passed=%d",
                                        totalChecked, maxJointAttempts, score, effectiveThreshold,
                                        passed ? 1 : 0);
                        if (passed) {
                            FACELOGIN_INFO(L"Anti-spoof joint verification passed: attempts=%d distance=%.4f",
                                           totalChecked, antiSpoofMatch.distance);
                        }
                        return passed;
                    };

                    // This frame already passed SCRFD, landmarks, pose and the
                    // second consecutive ArcFace match for the candidate.
                    livenessAcceptedPoseSeen = true;
                    livenessPassed = evaluateJointFrame(frame, landmarks, *match);

                    while (!m_stopRequested.load() && !livenessPassed &&
                           totalChecked < maxJointAttempts) {
                        if (m_pipeServer->IsClientDisconnected()) {
                            FACELOGIN_INFO(L"Client disconnected during anti-spoof — aborting");
                            return false;
                        }
                        auto antiSpoofElapsed = std::chrono::steady_clock::now() - asStart;
                        if (std::chrono::duration_cast<std::chrono::seconds>(antiSpoofElapsed).count() >= 5) break;

                        dlib::matrix<dlib::rgb_pixel> asFrame;
                        dlib::full_object_detection asLandmarks;
                        dlib::rectangle asRect;
                        HeadPoseStats asPose;
                        const AuthFrameResult frameResult = AcquireLegalPoseFrame(
                            asFrame, asRect, asLandmarks, asPose,
                            livenessPoseStabilizer, publishPoseStatus);
                        if (frameResult == AuthFrameResult::NoFrame) {
                            if (m_stopRequested.load()) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }
                        if (frameResult == AuthFrameResult::Pending) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }
                        if (frameResult != AuthFrameResult::Accepted) {
                            livenessPosePromptActive = true;
                            if (frameResult == AuthFrameResult::Rejected) {
                                livenessPoseRejected = true;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }

                        livenessAcceptedPoseSeen = true;
                        const bool restoreLivenessStatus = livenessPosePromptActive;
                        livenessPosePromptActive = false;
                        sendStatusKey(ipc::L10N_LIVENESS_CHECKING, restoreLivenessStatus);
                        if (restoreLivenessStatus) {
                            FACELOGIN_INFO(L"Pose recovered — restoring anti-spoof status");
                        }

                        auto asEmbedding = m_onnxRecognizer->ComputeEmbedding(asFrame, asLandmarks);
                        std::optional<CredentialStore::MatchResult> asMatch;
                        if (!asEmbedding.empty()) {
                            asMatch = MatchEmbedding(asEmbedding);
                        }
                        if (!asMatch) {
                            // A blurred or unmatchable frame is not valid
                            // evidence for either identity or liveness.
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }
                        if (!SameMatchedAccount(*asMatch, candidateAccountKey)) {
                            FACELOGIN_INFO(L"Anti-spoof candidate changed — returning to matching");
                            restartMatching = true;
                            break;
                        }

                        livenessPassed = evaluateJointFrame(asFrame, asLandmarks, *asMatch);
                        if (livenessPassed) {
                            // The credential must come from the exact frame
                            // whose identity and liveness jointly passed.
                            match = std::move(asMatch);
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    if (!livenessPassed && !restartMatching) {
                        FACELOGIN_WARN(L"Anti-spoof joint verification failed: attempts=%d/%d",
                                       totalChecked, maxJointAttempts);
                    }
                } else if (method == LivenessMethod::Blink) {
                    LivenessDetector liveness;
                    liveness.Configure(kDefaultEarThreshold, kDefaultBlinkFrames,
                                       m_config.blink_glasses_mode);
                    auto livenessStart = std::chrono::steady_clock::now();
                    bool blinked = false;
                    while (!m_stopRequested.load()) {
                        if (m_pipeServer->IsClientDisconnected()) {
                            FACELOGIN_INFO(L"Client disconnected during blink check — aborting");
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
                        dlib::full_object_detection livenessLandmarks;
                        dlib::rectangle lRect;
                        HeadPoseStats livenessPose;
                        const AuthFrameResult frameResult = AcquireLegalPoseFrame(
                            livenessFrame, lRect, livenessLandmarks,
                            livenessPose, livenessPoseStabilizer,
                            publishPoseStatus);
                        if (frameResult == AuthFrameResult::NoFrame) {
                            if (m_stopRequested.load()) break;
                            liveness.ResetBlinkProgress();
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }
                        if (frameResult == AuthFrameResult::Pending) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }
                        if (frameResult != AuthFrameResult::Accepted) {
                            livenessPosePromptActive = true;
                            if (frameResult == AuthFrameResult::Rejected) {
                                livenessPoseRejected = true;
                            }
                            liveness.ResetBlinkProgress();
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }

                        livenessAcceptedPoseSeen = true;
                        const bool restoreLivenessStatus = livenessPosePromptActive;
                        livenessPosePromptActive = false;
                        sendStatusKey(ipc::L10N_BLINK_PROMPT, restoreLivenessStatus);
                        if (restoreLivenessStatus) {
                            FACELOGIN_INFO(L"Pose recovered — restoring blink prompt");
                        }
                        if (liveness.ProcessFrame(livenessLandmarks)) {
                            blinked = true;
                            FACELOGIN_INFO(L"Blink detected");
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    }
                    livenessPassed = blinked;
                }

                if (restartMatching) {
                    consecutiveMatches = 0;
                    consensusAccountKey.clear();
                    consecutiveNoMatch = 0;
                    sendStatusKey(ipc::L10N_RECOGNIZING);
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    continue;
                }

                if (!livenessPassed) {
                    FACELOGIN_WARN(L"Liveness check failed");
                    SendAuthTerminal(ipc::BuildAuthErrorMessage(
                        livenessPoseRejected && !livenessAcceptedPoseSeen
                            ? ipc::L10N_POSE_TIMEOUT
                            : method == LivenessMethod::AntiSpoof
                                ? ipc::L10N_ANTI_SPOOF_FAILED
                                : ipc::L10N_BLINK_FAILED));
                    return false;
                }

                // Blink evidence is temporal, so it retains a fresh identity
                // verification. Anti-spoof already passed on an identity-bound
                // frame and therefore does not need a second match pass.
                //
                // Uses the SAME SCRFD detector as the recognition stage so the
                // two stages agree on face position. Retries over a short window:
                // the frame right after a blink is often mid-motion and its
                // box/embedding is noisy, so a single frame is unreliable. We
                // keep grabbing until a frame both detects a face AND matches
                // (or ~2s elapses).
                if (method == LivenessMethod::Blink) {
                    FACELOGIN_INFO(L"Blink liveness passed \u2014 verifying match");
                    sendStatusKey(ipc::L10N_FINAL_VERIFYING);
                    auto verifyStart = std::chrono::steady_clock::now();
                    bool verifyOk = false;
                    bool verifyAcceptedPoseSeen = false;
                    bool verifyPoseRejected = false;
                    bool verifyPosePromptActive = false;
                    HeadPoseStabilizer verifyPoseStabilizer;
                    while (!m_stopRequested.load() && !verifyOk) {
                        if (m_pipeServer->IsClientDisconnected()) {
                            FACELOGIN_INFO(L"Client disconnected during final verify — aborting");
                            return false;
                        }
                        auto vElapsed = std::chrono::steady_clock::now() - verifyStart;
                        if (std::chrono::duration_cast<std::chrono::seconds>(vElapsed).count() >= 2) break;

                        dlib::matrix<dlib::rgb_pixel> verifyFrame;
                        dlib::full_object_detection verifyLandmarks;
                        dlib::rectangle verifyRect;
                        HeadPoseStats verifyPose;
                        const AuthFrameResult frameResult = AcquireLegalPoseFrame(
                            verifyFrame, verifyRect, verifyLandmarks,
                            verifyPose, verifyPoseStabilizer, publishPoseStatus);
                        if (frameResult == AuthFrameResult::NoFrame) {
                            if (m_stopRequested.load()) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }
                        if (frameResult == AuthFrameResult::Pending) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }
                        if (frameResult != AuthFrameResult::Accepted) {
                            verifyPosePromptActive = true;
                            if (frameResult == AuthFrameResult::Rejected) {
                                verifyPoseRejected = true;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }

                        verifyAcceptedPoseSeen = true;
                        // Liveness has already passed in this phase. Keep the
                        // UI on final verification instead of regressing to the
                        // initial recognition status when the pose is legal.
                        const bool restoreFinalStatus = verifyPosePromptActive;
                        verifyPosePromptActive = false;
                        sendStatusKey(ipc::L10N_FINAL_VERIFYING, restoreFinalStatus);
                        if (restoreFinalStatus) {
                            FACELOGIN_INFO(L"Pose recovered — restoring final verification status");
                        }

                        std::optional<CredentialStore::MatchResult> verifyMatch;
                        auto verifyEmbedding = m_onnxRecognizer->ComputeEmbedding(
                            verifyFrame, verifyLandmarks);
                        if (!verifyEmbedding.empty()) {
                            verifyMatch = MatchEmbedding(verifyEmbedding);
                        }

                        if (verifyMatch) {
                            verifyOk = true;
                            FACELOGIN_INFO(L"Final match verified: distance=%.4f, "
                                           L"pose=%d range=%d yaw=%.1f pitch=%.1f roll=%.1f pose_ms=%.1f "
                                           L"face=%.0fx%.0f aspect=%.2f crop=%.0fx%.0f/%.2f",
                                           verifyMatch->distance,
                                           static_cast<int>(verifyPose.quality),
                                           static_cast<int>(verifyPose.range),
                                           verifyPose.yaw, verifyPose.pitch,
                                           verifyPose.roll, verifyPose.inferenceMs,
                                           verifyPose.faceWidth, verifyPose.faceHeight,
                                           verifyPose.faceAspect, verifyPose.cropWidth,
                                           verifyPose.cropHeight, verifyPose.cropAspect);
                            // Use the verified match for the credential (fresh, same identity).
                            match = std::move(verifyMatch);
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    }

                    if (!verifyOk) {
                        FACELOGIN_WARN(L"Final match verify failed \u2014 face swap detected");
                        SendAuthTerminal(ipc::BuildAuthErrorMessage(
                            verifyPoseRejected && !verifyAcceptedPoseSeen
                                ? ipc::L10N_POSE_TIMEOUT
                                : ipc::L10N_FINAL_MATCH_FAILED));
                        return false;
                    }
                }
            }

            // Build the sensitive success payload only after every recognition
            // and liveness gate has passed, keep it alive for the shortest
            // possible interval, and wipe it immediately after the pipe write.
            std::wstring msg = ipc::BuildAuthSuccessMessage(
                match->sid, match->upn,
                domain, match->username, match->password);
            const bool credentialsSent = SendAuthTerminal(msg);
            if (!msg.empty()) {
                SecureZeroMemory(msg.data(), msg.size() * sizeof(wchar_t));
                msg.clear();
            }

            match->WipePassword();

            if (!credentialsSent) {
                FACELOGIN_WARN(L"Credentials could not be delivered to the credential provider");
                return false;
            }

            authSent = true;
            FACELOGIN_INFO(L"AuthTerminal: outcome=success delivered=1 accountKind=%s",
                           match->upn.empty() ? L"local_or_domain" : L"online");

            // Stop the capture graph now. The graph keeps streaming during auth; pausing
            // it immediately after success frees the camera without waiting for
            // the full teardown.
            if (m_cameraPipeline == CameraPipeline::DS && m_webcamDS) {
                m_webcamDS->Pause();
            } else if (m_cameraPipeline == CameraPipeline::MF && m_webcamMF) {
                m_webcamMF->Shutdown();
            }

            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    return authSent;
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

    FACELOGIN_INFO(L"Service installed successfully");
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
