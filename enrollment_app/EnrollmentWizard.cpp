#include "EnrollmentWizard.h"
#include "../common/logger.h"
#include "../common/dpapi_util.h"
#include "../common/ipc_protocol.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include "../common/image_utils.h"
#include "../common/account_identity.h"
#include <comdef.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wincred.h>
#include <sddl.h>
#include <lmaccess.h>
#include <lmapibuf.h>
#include <lmerr.h>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <locale>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace facelogin {

// Helper: convert UTF-8 string to wide string (for config camera_device)
static std::wstring Utf8ToWstr(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring ws(len, L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), len) == 0) return L"";
    ws.pop_back();
    return ws;
}

static bool DecodeJpegWithWic(const std::wstring& path,
                               dlib::matrix<dlib::rgb_pixel>& image) {
    image.set_size(0, 0);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr,
                                                GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                &decoder);
    }
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame, GUID_WICPixelFormat24bppRGB,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);
    if (SUCCEEDED(hr) && (width == 0 || height == 0 || width > 4096 || height > 4096)) {
        hr = E_INVALIDARG;
    }
    std::vector<BYTE> pixels;
    if (SUCCEEDED(hr)) {
        pixels.resize(static_cast<size_t>(width) * height * 3);
        hr = converter->CopyPixels(nullptr, width * 3,
                                   static_cast<UINT>(pixels.size()), pixels.data());
    }
    if (SUCCEEDED(hr)) {
        image.set_size(static_cast<long>(height), static_cast<long>(width));
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const BYTE* pixel = pixels.data() + (static_cast<size_t>(y) * width + x) * 3;
                image(static_cast<long>(y), static_cast<long>(x)) =
                    dlib::rgb_pixel(pixel[0], pixel[1], pixel[2]);
            }
        }
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    return SUCCEEDED(hr);
}

static uint64_t CurrentFileTimeTicks() {
    FILETIME fileTime = {};
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER value = {};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return value.QuadPart;
}

static std::wstring AdaptiveSampleName(uint32_t faceId) {
    const uint64_t ticks = CurrentFileTimeTicks();
    wchar_t name[96] = {};
    swprintf_s(name, L"sample-%llu-%u.jpg",
               static_cast<unsigned long long>(ticks), faceId);
    return name;
}

// ---------------------------------------------------------------------------
// Session identity helper
//
// GetUserNameExW(NameUserPrincipal) is the ONE authoritative source of the
// current session's UPN: it returns the email for Microsoft accounts and
// fails with ERROR_NONE_MAPPED (1332) for local accounts. Machine-wide
// registry caches (IdentityStore\LogonCache\Name2Sid etc.) can hold MSA
// identities that do NOT belong to the current user — a previous user
// profile, or an MSA that was later converted to local. Adopting one of
// those emails mislabels the account as MSA and poisons the stored record,
// which produced bug1's perpetual false "账号身份已变更" prompt. All
// MSA/local decisions must route through the shared GetLinkedAccountUpn()
// (account_identity.cpp): it tries the direct UPN first, then falls back to
// the MicrosoftAccount shadow SID (S-1-11-96-*) in the token's group list —
// a local account LINKED to an MSA reports no UPN but still carries the
// shadow SID, and a UPN-only check would mislabel it as local (the 1.6.1
// fix that Console must also use).
static std::wstring GetSessionUpn() {
    std::wstring upn;
    GetLinkedAccountUpn(upn);
    return upn;
}

EnrollmentWizard::EnrollmentWizard() {
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
    CreateDirectoryW(m_dataDir.c_str(), nullptr);

    std::wstring logPath = m_dataDir + L"\\log\\enrollment.log";
    Logger::Instance().SetLogFile(logPath);
    Logger::Instance().SetMinLevel(LogLevel::Info);
    Logger::Instance().SetEnableDebugOutput(true);

    FACELOGIN_INFO(L"=== Enrollment Wizard started ===");

    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&m_wicFactory));

    // --- Gather user identity ---
    {
        wchar_t username[256] = {};
        DWORD size = ARRAYSIZE(username);
        if (!GetUserNameW(username, &size)) {
            DWORD err = GetLastError();
            FACELOGIN_ERROR(L"GetUserNameW FAILED: err=%lu", err);
            username[0] = L'\0';
        }
        m_username = username;
    }

    // Get UPN (UserPrincipalName) — the ONLY trusted source of the current
    // session's identity. GetUserNameExW(NameUserPrincipal) returns the email
    // UPN for Microsoft accounts and fails with ERROR_NONE_MAPPED for local
    // accounts, so m_upn stays empty for local users.
    //
    // NOTE: machine-wide registry fallbacks (IdentityCRL\StoredIdentities,
    // IdentityStore\LogonCache\Name2Sid) used to live here and adopted ANY
    // cached MSA email without verifying it belongs to the current user. On
    // machines with a leftover MSA identity (a previous user profile, or an
    // MSA later converted to local) that mislabeled local accounts as MSA and
    // poisoned the stored record — producing a perpetual false
    // "账号身份已变更" prompt (docs/todo.md bug1). Never re-add an
    // unattributed cache lookup.
    m_upn = GetSessionUpn();

    // Determine account type: MSA accounts have a UPN containing '@'
    m_accountType = "local";
    if (!m_upn.empty() && m_upn.find(L'@') != std::wstring::npos) {
        m_accountType = "msa";
    }

    // Get SID via LookupAccountNameW
    {
        DWORD sidSize = 0, domainSize = 0;
        SID_NAME_USE sidType;
        std::wstring lookupName = m_upn.empty() ? m_username : m_upn;

        LookupAccountNameW(nullptr, lookupName.c_str(),
                           nullptr, &sidSize, nullptr, &domainSize, &sidType);
        if (sidSize > 0) {
            std::vector<BYTE> sidBuf(sidSize);
            std::vector<wchar_t> domainBuf(domainSize > 0 ? domainSize : 1);
            if (LookupAccountNameW(nullptr, lookupName.c_str(),
                                   sidBuf.data(), &sidSize,
                                   domainBuf.data(), &domainSize, &sidType)) {
                LPWSTR sidStr = nullptr;
                if (ConvertSidToStringSidW(reinterpret_cast<PSID>(sidBuf.data()), &sidStr)) {
                    m_sid = sidStr;
                    LocalFree(sidStr);
                }
            } else {
                DWORD err = GetLastError();
                FACELOGIN_WARN(L"LookupAccountNameW(UPN) FAILED: err=%lu", err);
            }
        }

        if (m_sid.empty()) {
            DWORD sidSize2 = 0, domainSize2 = 0;
            SID_NAME_USE sidType2;
            LookupAccountNameW(nullptr, m_username.c_str(),
                               nullptr, &sidSize2, nullptr, &domainSize2, &sidType2);
            if (sidSize2 > 0) {
                std::vector<BYTE> sidBuf2(sidSize2);
                std::vector<wchar_t> domainBuf2(domainSize2 > 0 ? domainSize2 : 1);
                if (LookupAccountNameW(nullptr, m_username.c_str(),
                                       sidBuf2.data(), &sidSize2,
                                       domainBuf2.data(), &domainSize2, &sidType2)) {
                    LPWSTR sidStr = nullptr;
                    if (ConvertSidToStringSidW(reinterpret_cast<PSID>(sidBuf2.data()), &sidStr)) {
                        m_sid = sidStr;
                        LocalFree(sidStr);
                    }
                } else {
                    DWORD err = GetLastError();
                    FACELOGIN_WARN(L"LookupAccountNameW(SAM) FAILED: err=%lu", err);
                }
            }
        }
    }

    m_webcam     = std::make_unique<WebcamCapture>();
    m_detector   = std::make_unique<OnnxLandmarkDetector>();
    m_store.SetDataDir(m_dataDir);
    m_adaptiveLearning.SetDataDir(m_dataDir);
    if (!m_adaptiveLearning.Load()) {
        FACELOGIN_WARN(L"Adaptive learning archive unavailable; it will be retried on demand");
    }

    m_config = LoadConfig(m_dataDir);
    m_livenessMethod = m_config.liveness_method;
    m_antiSpoofThreshold = m_config.anti_spoof_threshold;
}

EnrollmentWizard::~EnrollmentWizard() {
    StopPreview();
    m_capturing = false;
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_wicFactory) m_wicFactory->Release();
}

// ============================================================================
// Preview Control
// ============================================================================

bool EnrollmentWizard::StartPreview() {
    if (m_previewRunning) return true;

    // Only the camera is opened on the UI thread — it's fast and we need its
    // success to gate the background work. Model loading (2d106det + ONNX
    // sessions) is deferred to the frame thread so a cold start never
    // blocks the UI and the user can switch tabs while "starting camera".
    //
    // Retry briefly: right after unlock the credential-provider service may
    // still be releasing the camera it used for auth, so a first init can fail
    // with the device busy. A couple of short retries absorb that window.
    constexpr int kInitRetries = 5;
    bool webcamOk = false;
    for (int attempt = 0; attempt < kInitRetries && !webcamOk; attempt++) {
        webcamOk = m_webcam->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device));
        if (!webcamOk && attempt + 1 < kInitRetries) {
            FACELOGIN_WARN(L"Webcam init attempt %d failed — camera may still be releasing, retrying", attempt + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    if (!webcamOk) {
        FACELOGIN_ERROR(L"Failed to initialize webcam%s",
                        m_config.camera_device.empty() ? L"" : L" (configured device)");
        return false;
    }
    FACELOGIN_INFO(L"Webcam initialized");

    m_previewRunning = true;
    m_frameRunning = true;
    m_frameReinitCount = 0;

    // Single background thread: load models (if needed) → GrabFrame → JPEG
    // encode → detect → update caches. The UI thread stays completely free;
    // JS polls the caches via GetLatest*(). On a cold start the models are
    // loaded first here, off the UI thread. The generation is captured by
    // value: StopPreview() bumps it, so any zombie thread (detached after a
    // driver wedge, then recovered) sees the mismatch and exits instead of
    // running alongside the next StartPreview's fresh thread.
    int myGen = ++m_frameGeneration;
    m_frameThread = std::thread([this, myGen]() {
        bool poseDisplayInitialized = false;
        HeadPoseStats poseDisplay;
        if (!EnsureModelsLoaded()) {
            FACELOGIN_ERROR(L"Model loading failed — no frames will be produced");
            return;
        }
        while (m_frameRunning && m_frameGeneration == myGen) {
            // ---- Pull mode (during the SAMPLING phase only) ---------------
            // The capture thread requests ONE fresh frame per sample; we
            // deliver exactly that and do zero inference/encoding (the
            // preview is frozen — renderFrame is gated during capture — and
            // the capture thread detects the delivered frame itself). This
            // replaces the 30fps stream + per-frame NV12→RGB conversion +
            // full-frame copies that previously ran to feed 10 samples.
            //
            // Liveness deliberately runs on the PUSH stream (m_phase2Active
            // false): continuous grabbing keeps auto-exposure stable, which
            // facenox MiniFAS depends on — on-demand grabbing made frames
            // fluctuate and anti-spoof scores collapse.
            if (m_capturing && m_phase2Active) {
                {
                    std::unique_lock<std::mutex> lock(m_frameCacheMutex);
                    if (!m_sampleCv.wait_for(lock, std::chrono::milliseconds(100),
                                             [this, myGen] {
                                                 return !m_frameRunning ||
                                                        !m_phase2Active ||
                                                        m_frameGeneration != myGen ||
                                                        m_sampleDelivered < m_sampleSeq;
                                             })) {
                        continue;   // nothing requested yet
                    }
                    if (!m_frameRunning || !m_phase2Active || m_frameGeneration != myGen) break;
                }
                dlib::matrix<dlib::rgb_pixel> frame;
                bool ok = m_webcam->GrabFrame(frame);
                if (ok) {
                    RotateFrame(frame, m_config.camera_rotation);
                }
                {
                    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
                    if (ok) {
                        m_latestFrame = std::move(frame);
                    } else {
                        // Answer "no frame" — the requester falls back to retry.
                        m_latestFrame = dlib::matrix<dlib::rgb_pixel>();
                    }
                    m_sampleDelivered = m_sampleSeq;
                }
                m_sampleCv.notify_all();
                if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }

            // ---- Push mode (preview) ---------------------------------------
            dlib::matrix<dlib::rgb_pixel> frame;
            if (!m_webcam->GrabFrame(frame)) {
                // GrabFrame self-shuts-down after repeated failures (e.g. the
                // camera was taken over by the credential provider and the
                // SourceReader went stale). Rebuild the camera here so preview
                // recovers instead of spinning on a dead SourceReader until the
                // next StartPreview. Bounded retries: if the device truly is
                // gone, stop hammering it and wait for the caller to retry.
                if (!m_webcam->IsInitialized() && m_frameRunning &&
                    m_frameGeneration == myGen) {
                    if (++m_frameReinitCount >= 3) {
                        FACELOGIN_ERROR(L"Preview camera re-init exceeded limit — giving up until next start");
                        break;
                    }
                    FACELOGIN_WARN(L"Preview camera stalled — re-initializing (%d/3)", m_frameReinitCount);
                    m_webcam->Shutdown();
                    if (m_webcam->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
                        FACELOGIN_INFO(L"Preview camera re-initialized");
                        m_frameReinitCount = 0;   // a successful re-init resets the budget
                    } else {
                        FACELOGIN_ERROR(L"Preview camera re-init failed — giving up until next start");
                        break;   // exit frame loop; next StartPreview retries
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }

            RotateFrame(frame, m_config.camera_rotation);

            // Preview detection runs before the frame is encoded for the UI.
            std::string faceJson = "[]";
            if (m_onnxDetector) {
                std::vector<facelogin::FaceWithLandmarks> faces;
                FaceWithLandmarks fwl;
                HeadPoseStats pose;
                const bool prepared = PrepareFaceFrame(
                    frame, fwl.rect, fwl.landmarks, &pose);
                if (prepared) {
                    // Smooth only the preview overlay. Recognition, liveness
                    // and service logs keep the raw model result unchanged.
                    static constexpr float kPoseDisplayAlpha = 0.35f;
                    if (pose.range == HeadPoseRange::Invalid) {
                        poseDisplay = pose;
                        poseDisplayInitialized = false;
                    } else if (!poseDisplayInitialized) {
                        poseDisplay = pose;
                        poseDisplayInitialized = true;
                    } else {
                        poseDisplay.yaw += kPoseDisplayAlpha *
                                           (pose.yaw - poseDisplay.yaw);
                        poseDisplay.pitch += kPoseDisplayAlpha *
                                             (pose.pitch - poseDisplay.pitch);
                        poseDisplay.roll += kPoseDisplayAlpha *
                                            (pose.roll - poseDisplay.roll);
                        poseDisplay.inferenceMs = pose.inferenceMs;
                        poseDisplay.faceWidth = pose.faceWidth;
                        poseDisplay.faceHeight = pose.faceHeight;
                        poseDisplay.faceAspect = pose.faceAspect;
                        poseDisplay.cropWidth = pose.cropWidth;
                        poseDisplay.cropHeight = pose.cropHeight;
                        poseDisplay.cropAspect = pose.cropAspect;
                        poseDisplay.valid = pose.valid;
                        poseDisplay.quality = pose.quality;
                        poseDisplay.range = pose.range;
                    }
                    faces.push_back(std::move(fwl));
                    faceJson = FacesToJson(faces, &poseDisplay);
                }
            }
            std::string b64 = EncodeJPEGBase64(frame);

            {
                std::lock_guard<std::mutex> lock(m_frameCacheMutex);
                m_latestFrameB64  = std::move(b64);
                m_latestFacesJson = std::move(faceJson);
                m_latestFrame     = frame;
            }
        }
    });

    return true;
}

bool EnrollmentWizard::PrepareFaceFrame(
    dlib::matrix<dlib::rgb_pixel>& frame,
    dlib::rectangle& rect,
    dlib::full_object_detection& landmarks,
    HeadPoseStats* outPose) {
    if (outPose) *outPose = {};
    if (frame.size() == 0 || !m_onnxDetector || !m_detector) return false;
    rect = dlib::rectangle();
    landmarks = dlib::full_object_detection();

    auto detect = [this, &rect, &landmarks](
                      const dlib::matrix<dlib::rgb_pixel>& candidate) -> bool {
        auto det = m_onnxDetector->DetectLargestFace(candidate);
        if (!det) return false;
        rect = dlib::rectangle(static_cast<long>(det->x1),
                               static_cast<long>(det->y1),
                               static_cast<long>(det->x2),
                               static_cast<long>(det->y2));
        landmarks = dlib::full_object_detection();
        return !rect.is_empty() && m_detector->DetectLandmarks(candidate, rect, landmarks);
    };

    if (!detect(frame)) return false;
    if (outPose && m_headPose && m_headPose->IsInitialized()) {
        *outPose = m_headPose->Estimate(frame, rect);
    }
    return true;
}

// Load the 2d106det + ONNX models if not already loaded. Called from
// the background frame thread so a cold start never blocks the UI thread.
// Models survive StopPreview() (only the camera is torn down), so a restart
// (e.g. cancel back to the camera screen from the append dialog) reuses them.
bool EnrollmentWizard::EnsureModelsLoaded() {
    const bool requiredModelsWereReady =
        m_detector && m_detector->IsInitialized() &&
        m_onnxDetector && m_onnxDetector->IsInitialized() &&
        m_onnxRecognizer && m_onnxRecognizer->IsInitialized();
    std::wstring modelsDir = m_dataDir + L"\\models";
    std::wstring shapePath = modelsDir + L"\\2d106det.onnx";

    if (!m_detector->IsInitialized() && !m_detector->Initialize(shapePath)) {
        FACELOGIN_ERROR(L"Failed to load 2d106det landmark detector");
        return false;
    }

    // Load SCRFD ONNX detector (the only detector).
    std::wstring detPath = modelsDir + L"\\det_500m.onnx";
    if (!m_onnxDetector) {
        m_onnxDetector = std::make_unique<OnnxDetector>();
        if (!m_onnxDetector->Initialize(detPath)) {
            FACELOGIN_ERROR(L"SCRFD detector failed to load — enrollment unavailable");
            m_onnxDetector.reset();
            return false;
        }
    }

    // Pose is optional and observer-only. A missing/incompatible model must
    // never make face enrollment or recognition unavailable.
    std::wstring posePath = modelsDir + L"\\head_pose_mobilenetv2.onnx";
    if (!m_headPose) {
        m_headPose = std::make_unique<OnnxHeadPose>();
        if (!m_headPose->Initialize(posePath)) {
            FACELOGIN_WARN(L"MobileNetV2 head-pose model not available; pose HUD disabled");
            m_headPose.reset();
        }
    }

    // Load InsightFace ONNX recognizer (the only recognizer).
    std::wstring onnxPath = modelsDir + L"\\w600k_mbf.onnx";
    if (!m_onnxRecognizer) {
        m_onnxRecognizer = std::make_unique<OnnxRecognizer>();
        if (!m_onnxRecognizer->Initialize(onnxPath)) {
            FACELOGIN_ERROR(L"ONNX recognizer failed to load — enrollment unavailable");
            m_onnxRecognizer.reset();
            return false;
        }
    }

    // Try loading anti-spoof model (facenox MiniFAS, 1.6.0).
    std::wstring antiSpoofPath = modelsDir + L"\\minifas_quantized.onnx";
    if (!m_antiSpoof) {
        m_antiSpoof = std::make_unique<OnnxAntiSpoof>();
        if (!m_antiSpoof->Initialize(antiSpoofPath)) {
            FACELOGIN_WARN(L"Anti-spoof model not available");
            m_antiSpoof.reset();
        }
    }

    // Apply runtime settings to freshly-loaded models.
    // dlib recognizer/detector were removed — pure ONNX. recognition_model
    // and detector config values are ignored.

    // Validate liveness method
    if (m_livenessMethod == LivenessMethod::AntiSpoof && (!m_antiSpoof || !m_antiSpoof->IsInitialized())) {
        FACELOGIN_WARN(L"Anti-spoof configured but unavailable, falling back to blink");
        m_livenessMethod = LivenessMethod::Blink;
    }

    if (!requiredModelsWereReady) {
        FACELOGIN_INFO(L"Liveness method: %hs | Models ready",
                       m_livenessMethod == LivenessMethod::Blink ? "blink" :
                       m_livenessMethod == LivenessMethod::AntiSpoof ? "antispoof" : "none");
    }

    return true;
}

// Bounded thread join for StopPreview. Wait up to timeoutMs for the thread to
// exit, then detach it if it's still running (wedged in a driver call). A
// detach means the thread may outlive the wizard in the pathological
// driver-hang case — far better than freezing the UI thread forever
// (卡90%无响应). Only the frame thread can hit this: the capture thread always
// terminates within its bounded loops.
static void JoinBounded(std::thread& t, const wchar_t* name,
                        DWORD timeoutMs = 1500) {
    if (!t.joinable()) return;
    HANDLE h = t.native_handle();
    DWORD wait = WaitForSingleObject(h, timeoutMs);
    if (wait == WAIT_OBJECT_0) {
        t.join();   // thread already finished — join returns immediately
        return;
    }
    if (wait == WAIT_FAILED) {
        FACELOGIN_WARN(L"StopPreview: WaitForSingleObject on %s thread failed (%lu) — detaching",
                       name, GetLastError());
        t.detach();
        return;
    }
    // WAIT_TIMEOUT: the thread is stuck (likely inside a driver call that
    // Shutdown() above could not wake). Detach it so the UI thread moves on;
    // the stuck thread ends whenever the driver recovers or the process exits.
    FACELOGIN_WARN(L"StopPreview: %s thread did not exit within %lu ms — detaching (driver may be wedged)",
                   name, timeoutMs);
    t.detach();
}

void EnrollmentWizard::StopPreview() {
    m_previewRunning = false;
    m_frameRunning = false;
    m_capturing = false;
    // Bump the generation: a detached zombie frame thread that later wakes up
    // from a wedged driver call sees the mismatch and exits instead of
    // touching the (possibly re-initialized) camera next to the fresh thread.
    ++m_frameGeneration;

    // Now shut down the camera so a synchronous ReadSample that is blocked
    // (camera taken over by the credential provider at lock) returns an error.
    // Shutdown() itself is bounded (see WebcamCapture::Shutdown) so a wedged
    // driver can no longer freeze the UI thread.
    if (m_webcam)
        m_webcam->Shutdown();

    // Join background threads with a budget. The capture thread exits fast;
    // the frame thread may be blocked in ReadSample until the shutdown above
    // wakes it. If either is still running after the budget, detach instead of
    // blocking the UI thread forever.
    JoinBounded(m_captureThread, L"capture");
    JoinBounded(m_frameThread, L"frame");

    // Normal-path completion marker: lets log triage confirm a teardown went
    // through cleanly (any timeout above would emit its own WARN instead).
    FACELOGIN_INFO(L"Preview stopped (camera released, threads joined)");
}

// ============================================================================
// Per-frame Processing (called from timer callback)
// ============================================================================

std::string EnrollmentWizard::GetLatestFrameBase64() {
    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
    return m_latestFrameB64;
}

std::string EnrollmentWizard::GetLatestFacesJson() {
    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
    return m_latestFacesJson;
}

// Atomically return BOTH the current frame (JPEG base64) and its face overlay
// JSON. The frame thread updates them together under the same lock, so reading
// them in one call guarantees they belong to the SAME frame. The frontend uses
// this to draw the background and overlay from matching frames — otherwise the
// overlay could come from a newer frame than the displayed image, causing the
// face box/landmarks to drift from the visible face.
std::string EnrollmentWizard::GetLatestFrameAndFaces() {
    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
    std::string result = m_latestFrameB64;
    result += "\x1E";  // record separator
    result += m_latestFacesJson;
    return result;
}

// ============================================================================
// JPEG Encoding via WIC
// ============================================================================

static std::string EncodeBase64(const BYTE* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<unsigned int>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<unsigned int>(data[i + 2]);
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back((i + 1 < len) ? tbl[(n >> 6) & 63] : '=');
        out.push_back((i + 2 < len) ? tbl[n & 63] : '=');
    }
    return out;
}

std::string EnrollmentWizard::EncodeJPEGBase64(const dlib::matrix<dlib::rgb_pixel>& frame) {
    if (!m_wicFactory) return {};

    int srcW = static_cast<int>(frame.nc());
    int srcH = static_cast<int>(frame.nr());

    // Reuse the staging buffers across frames (only reallocated on resolution
    // change) instead of allocating ~3.7MB + the JPEG output every frame.
    if (srcW != m_encodeWidth || srcH != m_encodeHeight) {
        m_encodeWidth = srcW;
        m_encodeHeight = srcH;
        m_encodeBgra.assign(static_cast<size_t>(srcW) * srcH * 4, 0);
        m_encodeJpeg.clear();
        m_encodeJpeg.shrink_to_fit();
    }
    std::vector<BYTE>& bgra = m_encodeBgra;

    for (int y = 0; y < srcH; y++) {
        BYTE* row = bgra.data() + static_cast<size_t>(y) * srcW * 4;
        for (int x = 0; x < srcW; x++) {
            const auto& p = frame(y, x);
            row[x * 4 + 0] = p.blue;
            row[x * 4 + 1] = p.green;
            row[x * 4 + 2] = p.red;
            row[x * 4 + 3] = 255;
        }
    }

    IWICBitmap* pBitmap = nullptr;
    HRESULT hr = m_wicFactory->CreateBitmapFromMemory(
        srcW, srcH, GUID_WICPixelFormat32bppBGR,
        srcW * 4, static_cast<UINT>(bgra.size()), bgra.data(), &pBitmap);
    if (FAILED(hr)) return {};

    IWICBitmapEncoder* pEncoder = nullptr;
    hr = m_wicFactory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &pEncoder);
    if (FAILED(hr)) { pBitmap->Release(); return {}; }

    IStream* pStream = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &pStream);
    hr = pEncoder->Initialize(pStream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) { pEncoder->Release(); pStream->Release(); pBitmap->Release(); return {}; }

    IWICBitmapFrameEncode* pFrameEncode = nullptr;
    IPropertyBag2* pProps = nullptr;
    hr = pEncoder->CreateNewFrame(&pFrameEncode, &pProps);
    if (SUCCEEDED(hr)) {
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.70f;
        pProps->Write(1, &opt, &v);
        VariantClear(&v);
        pFrameEncode->Initialize(pProps);
        pFrameEncode->SetSize(srcW, srcH);
        pFrameEncode->WriteSource(pBitmap, nullptr);
        pFrameEncode->Commit();
        pEncoder->Commit();
    }

    STATSTG stat;
    pStream->Stat(&stat, STATFLAG_NONAME);
    ULONG jpgSize = static_cast<ULONG>(stat.cbSize.QuadPart);
    std::vector<BYTE>& jpgData = m_encodeJpeg;
    if (jpgData.size() < jpgSize) jpgData.resize(jpgSize);
    LARGE_INTEGER li = {};
    pStream->Seek(li, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    pStream->Read(jpgData.data(), jpgSize, &read);

    std::string result = "data:image/jpeg;base64,";
    result += EncodeBase64(jpgData.data(), jpgSize);

    if (pFrameEncode) pFrameEncode->Release();
    if (pProps) pProps->Release();
    pEncoder->Release();
    pStream->Release();
    pBitmap->Release();

    return result;
}

// ============================================================================
// Face Detection -> JSON
// ============================================================================

std::string EnrollmentWizard::FacesToJson(
    const std::vector<facelogin::FaceWithLandmarks>& faces,
    const HeadPoseStats* pose) {
    std::ostringstream js;
    js.imbue(std::locale::classic());
    js << "[";
    for (size_t fi = 0; fi < faces.size(); fi++) {
        if (fi > 0) js << ",";
        const auto& f = faces[fi];
        js << "{"
           << "\"x\":" << f.rect.left()
           << ",\"y\":" << f.rect.top()
           << ",\"w\":" << static_cast<int>(f.rect.width())
           << ",\"h\":" << static_cast<int>(f.rect.height())
           << ",\"landmarks\":[";
        for (unsigned long i = 0; i < f.landmarks.num_parts(); i++) {
            if (i > 0) js << ",";
            js << static_cast<int>(f.landmarks.part(i).x()) << ","
               << static_cast<int>(f.landmarks.part(i).y());
        }
        js << "],\"pose\":";
        if (pose) {
            js << "{\"valid\":" << (pose->valid ? "true" : "false")
               << std::fixed << std::setprecision(1)
               << ",\"yaw\":" << pose->yaw
               << ",\"pitch\":" << pose->pitch
               << ",\"roll\":" << pose->roll
               << std::setprecision(2)
               << ",\"inferenceMs\":" << pose->inferenceMs
               << ",\"faceWidth\":" << pose->faceWidth
               << ",\"faceHeight\":" << pose->faceHeight
               << ",\"faceAspect\":" << pose->faceAspect
               << ",\"cropWidth\":" << pose->cropWidth
               << ",\"cropHeight\":" << pose->cropHeight
               << ",\"cropAspect\":" << pose->cropAspect
               << ",\"range\":\""
               << (pose->range == HeadPoseRange::Normal ? "normal" :
                   pose->range == HeadPoseRange::Wide ? "wide" : "invalid")
               << "\""
               << "}";
        } else {
            js << "null";
        }
        js << "}";
    }
    js << "]";
    return js.str();
}

// ============================================================================
// Pull-model frame delivery (capture/liveness → frame thread)
// ============================================================================

bool EnrollmentWizard::RequestFreshFrame(dlib::matrix<dlib::rgb_pixel>& outFrame,
                                         DWORD budgetMs) {
    std::unique_lock<std::mutex> lock(m_frameCacheMutex);
    ++m_sampleSeq;
    m_sampleCv.notify_all();
    // StopPreview() clears m_capturing/m_frameRunning, so the wait aborts
    // immediately on teardown instead of burning the full budget — the
    // capture thread then exits fast and its join never times out.
    if (!m_sampleCv.wait_for(lock, std::chrono::milliseconds(budgetMs),
                             [this] {
                                 return m_sampleDelivered >= m_sampleSeq ||
                                        !m_capturing || !m_frameRunning;
                             })) {
        return false;   // frame thread didn't answer in time (dead / stalled)
    }
    if (m_latestFrame.size() == 0) return false;   // answered "no frame"
    outFrame = m_latestFrame;
    return true;
}

// ============================================================================
// Enrollment actions (called from JS)
// ============================================================================

std::string EnrollmentWizard::GetUsername() const {
    // Return UPN for MSA accounts, SAM name for local accounts
    const std::wstring& display = m_upn.empty() ? m_username : m_upn;
    int len = WideCharToMultiByte(CP_UTF8, 0, display.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(len - 1, ' ');
    WideCharToMultiByte(CP_UTF8, 0, display.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

bool EnrollmentWizard::CaptureFaceSamples() {
    if (m_capturing) return false;

    // Join previous capture thread if it exists (prevents std::terminate on reassignment)
    if (m_captureThread.joinable())
        m_captureThread.join();

    m_capturing = true;
    m_samplesCollected = 0;
    m_embeddings.clear();
    m_livenessPassed = false;
    m_livenessChecking = true;

    m_captureThread = std::thread([this]() {
        // Phase 1: Liveness check (blink, anti-spoof, or none)
        LivenessMethod method = m_livenessMethod;
        FACELOGIN_INFO(L"Enrollment: starting liveness check (method=%hs)",
                      method == LivenessMethod::Blink ? "blink" :
                      method == LivenessMethod::AntiSpoof ? "antispoof" : "none");

        bool livenessPassed = false;

        if (method == LivenessMethod::None) {
            livenessPassed = true;
        } else if (method == LivenessMethod::AntiSpoof) {
            int totalChecks = AntiSpoofCheckCount(m_antiSpoofThreshold);
            int passRequired = AntiSpoofPassRequired(totalChecks);
            FACELOGIN_INFO(L"Enrollment anti-spoof: threshold=%.3f → %d checks, %d required",
                           m_antiSpoofThreshold, totalChecks, passRequired);
            auto asStart = std::chrono::steady_clock::now();
            int passCount = 0, totalChecked = 0;
            while (m_capturing && totalChecked < totalChecks) {
                auto elapsed = std::chrono::steady_clock::now() - asStart;
                if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 8) break;

                dlib::matrix<dlib::rgb_pixel> frame;
                // Read the latest frame from the frame thread's PUSH stream.
                // The frame thread stays in push mode during liveness (see
                // m_phase2Active) so the camera keeps grabbing at 30fps and
                // auto-exposure stays stable — on-demand grabbing made frames
                // fluctuate and collapsed anti-spoof scores.
                {
                    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
                    if (m_latestFrame.size() != 0) frame = m_latestFrame;
                }
                if (frame.size() == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(33)); continue; }
                // Detect with SCRFD, extract 106-point landmarks.
                dlib::full_object_detection asLandmarks;
                auto asDet = m_onnxDetector->DetectLargestFace(frame);
                if (asDet) {
                    dlib::rectangle asRect(static_cast<long>(asDet->x1),
                                           static_cast<long>(asDet->y1),
                                           static_cast<long>(asDet->x2),
                                           static_cast<long>(asDet->y2));
                    m_detector->DetectLandmarks(frame, asRect, asLandmarks);
                }
                if (asLandmarks.num_parts() == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(33)); continue; }

                float score = m_antiSpoof->Predict(frame, asLandmarks);
                totalChecked++;
                // Map the config slider onto the current model's score scale
                // (facenox logit-diff vs OULU pixel-mean) — see
                // AntiSpoofEffectiveThreshold in liveness_types.h.
                float effThr = AntiSpoofEffectiveThreshold(m_antiSpoofThreshold,
                                                           m_antiSpoof->IsFacenoxMode());
                if (score >= effThr) passCount++; // model-mapped threshold
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            livenessPassed = (totalChecked > 0 && passCount >= passRequired);
        } else {
            // blink (default)
            LivenessDetector liveness;
            liveness.Configure(kDefaultEarThreshold, kDefaultBlinkFrames,
                               m_config.blink_glasses_mode);
            auto livenessStart = std::chrono::steady_clock::now();
            bool blinked = false;
            while (m_capturing && !blinked) {
                auto elapsed = std::chrono::steady_clock::now() - livenessStart;
                if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 8) {
                    FACELOGIN_WARN(L"Enrollment liveness timeout");
                    break;
                }
                dlib::matrix<dlib::rgb_pixel> frame;
                // Read the latest frame from the PUSH stream (see antispoof
                // branch above — continuous grabbing keeps exposure stable).
                {
                    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
                    if (m_latestFrame.size() != 0) frame = m_latestFrame;
                }
                if (frame.size() == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(33)); continue; }
                // Detect with SCRFD, extract 106-point landmarks for EAR.
                dlib::full_object_detection livenessLandmarks;
                auto livenessDet = m_onnxDetector->DetectLargestFace(frame);
                if (livenessDet) {
                    dlib::rectangle lRect(static_cast<long>(livenessDet->x1),
                                          static_cast<long>(livenessDet->y1),
                                          static_cast<long>(livenessDet->x2),
                                          static_cast<long>(livenessDet->y2));
                    m_detector->DetectLandmarks(frame, lRect, livenessLandmarks);
                }
                if (livenessLandmarks.num_parts() == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(33)); continue; }
                if (liveness.ProcessFrame(livenessLandmarks)) {
                    blinked = true;
                    FACELOGIN_INFO(L"Enrollment: blink detected");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
            livenessPassed = blinked;
        }

        m_livenessChecking = false;

        if (!livenessPassed) {
            FACELOGIN_WARN(L"Enrollment liveness check failed");
            m_capturing = false;
            return;
        }

        m_livenessPassed = true;
        // Sampling phase: the frame thread switches to pull mode (on-demand
        // grabbing, no 30fps churn). Liveness deliberately stays on the push
        // stream — see m_phase2Active in the header.
        m_phase2Active = true;

        // Phase 2: Collect face samples
        int failCount = 0;
        // Hard cap on Phase 2 wall time. A normal capture of 10 samples takes
        // ~2.5s (150ms sleep + ~100ms inference each); 8s gives >3x headroom
        // while guaranteeing the loop ALWAYS terminates — so even if the frame
        // thread dies or detection never succeeds, the backend stops and the
        // frontend can show "采集超时" instead of hanging at 90% forever.
        constexpr int kPhase2TimeoutMs = 8000;
        auto phase2Start = std::chrono::steady_clock::now();
        for (int i = 0; i < TARGET_SAMPLES && m_capturing;) {
            // Total-phase timeout: bail no matter what the cause.
            auto phase2Elapsed = std::chrono::steady_clock::now() - phase2Start;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(phase2Elapsed).count() >= kPhase2TimeoutMs) {
                FACELOGIN_WARN(L"Enrollment Phase 2 timed out after %d ms (captured=%d/10) — aborting",
                               kPhase2TimeoutMs, i);
                m_capturing = false;
                break;
            }
            // Pull one fresh frame per sample — the frame thread grabs a camera
            // frame on demand (pull mode) instead of streaming 30fps, so no
            // redundant full-frame conversion/copy runs during capture.
            dlib::matrix<dlib::rgb_pixel> frame;
            bool haveFrame = RequestFreshFrame(frame, 1500);
            if (!haveFrame) {
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
                continue;
            }
            // Detect with SCRFD (the only detector), extract 106-point landmarks.
            dlib::full_object_detection landmarks;
            auto onnxDet = m_onnxDetector->DetectLargestFace(frame);
            if (onnxDet) {
                dlib::rectangle rect(static_cast<long>(onnxDet->x1),
                                     static_cast<long>(onnxDet->y1),
                                     static_cast<long>(onnxDet->x2),
                                     static_cast<long>(onnxDet->y2));
                m_detector->DetectLandmarks(frame, rect, landmarks);
            }
            if (landmarks.num_parts() == 0) {
                // 80 × 100ms ≈ 8s of consecutive detection failures — the
                // Phase-2 total timeout already bounds the whole loop, so this
                // is a belt-and-suspenders early exit.
                if (++failCount > 80) { m_capturing = false; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // Compute the embedding with InsightFace ONNX (the only recognizer).
            // Store the FULL 512-D embedding (no truncation).
            auto onnxEmb = m_onnxRecognizer->ComputeEmbedding(frame, landmarks);
            if (onnxEmb.empty()) {
                if (++failCount > 80) { m_capturing = false; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            dlib::matrix<float, 0, 1> emb;
            emb.set_size(static_cast<long>(onnxEmb.size()));
            for (size_t k = 0; k < onnxEmb.size(); k++)
                emb(static_cast<long>(k)) = onnxEmb[k];

            failCount = 0;
            m_embeddings.push_back(std::move(emb));
            m_samplesCollected = ++i;
            FACELOGIN_INFO(L"Enrollment sample collected: %d/%d", i, TARGET_SAMPLES);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        FACELOGIN_INFO(L"Enrollment sampling loop ended: captured=%d/%d, failCount=%d, m_capturing=%d",
                       m_samplesCollected, TARGET_SAMPLES, failCount, static_cast<int>(m_capturing));
        m_phase2Active = false;   // frame thread back to push mode
        m_capturing = false;
    });

    return true;
}

// Helper: convert wstring to UTF-8 string for JS
static std::string WstrToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

// Helper: UTF-8 string with JSON escaping — safe to embed directly inside a
// JSON string literal for the face list and account-change responses. Escapes
// per RFC 8259: quote, backslash, and the mandatory control characters
// (U+0000–U+001F). Forward slash is intentionally NOT escaped (legal in JSON
// and never needed for these fields). Other characters pass through UTF-8.
static std::string WstrToUtf8Escaped(const std::wstring& ws) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (wchar_t ch : ws) {
        if (ch == L'"')            out += "\\\"";
        else if (ch == L'\\')      out += "\\\\";
        else if (ch == L'\b')      out += "\\b";
        else if (ch == L'\f')      out += "\\f";
        else if (ch == L'\n')      out += "\\n";
        else if (ch == L'\r')      out += "\\r";
        else if (ch == L'\t')      out += "\\t";
        else if (ch < 0x20) {      // other C0 control chars → \uXXXX
            out += "\\u00";
            out += kHex[(ch >> 4) & 0xF];
            out += kHex[ch & 0xF];
        }
        else {
            char mb[4] = {};
            int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 4, nullptr, nullptr);
            if (n > 0) out.append(mb, static_cast<size_t>(n));
        }
    }
    return out;
}

// Notify the FaceLogin service to reload the user database after a write.
static void NotifyServiceReload() {
    HANDLE hPipe = CreateFileW(ipc::PIPE_NAME, GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (hPipe != INVALID_HANDLE_VALUE) {
        DWORD written;
        std::wstring msg(ipc::MSG_RELOAD_DB);
        msg.push_back(L'\0');
        WriteFile(hPipe, msg.c_str(), static_cast<DWORD>(msg.size() * sizeof(wchar_t)),
                  &written, nullptr);
        CloseHandle(hPipe);
    }
}

std::string EnrollmentWizard::GetUserSid() const {
    return WstrToUtf8(m_sid);
}

std::string EnrollmentWizard::GetUserUpn() const {
    return WstrToUtf8(m_upn);
}

bool EnrollmentWizard::ValidatePassword(const std::wstring& password) {
    // The MSA/local decision MUST come from the live session identity
    // (GetSessionUpn), never from m_upn: m_upn used to be polluted with
    // another account's email by a registry fallback (docs/todo.md bug1),
    // which routed local users into the MSA path below and made the account
    // refresh loop impossible to complete.
    std::wstring sessionUpn = GetSessionUpn();
    bool isMsa = !sessionUpn.empty() && sessionUpn.find(L'@') != std::wstring::npos;

    if (isMsa) {
        // MSA: domain=NULL routes through CloudAP for an online validation of
        // the CURRENT password. (LogonUserW with domain="." would validate
        // against only the local account database — i.e. the STALE cached MSA
        // credential, not the user's current Microsoft password.) INTERACTIVE
        // also refreshes the cached credential when it succeeds (NETWORK
        // never caches).
        HANDLE hToken = nullptr;
        BOOL ok = LogonUserW(sessionUpn.c_str(), nullptr, password.c_str(),
                             LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &hToken);
        if (ok && hToken) {
            CloseHandle(hToken);
            FACELOGIN_INFO(L"ValidatePassword: MSA online validation succeeded");
            return true;
        }
        DWORD err = GetLastError();
        FACELOGIN_WARN(L"ValidatePassword: MSA interactive logon failed (err=%lu)", err);
        return false;
    }

    // Local account: validate against the local SAM database (reliable offline).
    HANDLE hToken = nullptr;
    BOOL ok = LogonUserW(m_username.c_str(), L".", password.c_str(),
                         LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &hToken);
    if (ok && hToken) {
        CloseHandle(hToken);
        return true;
    }
    FACELOGIN_WARN(L"ValidatePassword: local logon failed (err=%lu)", GetLastError());
    return false;
}

bool EnrollmentWizard::SaveEnrollment(const std::wstring& password,
                                      const std::wstring& label) {
    return SaveEnrollmentImpl(password, /*passwordless=*/false, label);
}

bool EnrollmentWizard::SaveEnrollmentNoPassword(const std::wstring& label) {
    // Re-verify the current session identity before allowing a passwordless
    // save — the user must be the logged-on owner of this account.
    std::wstring tokenSid = GetCurrentProcessUserSid();
    if (tokenSid.empty() || tokenSid != m_sid) {
        FACELOGIN_ERROR(L"Passwordless enrollment refused: session identity mismatch");
        return false;
    }
    FACELOGIN_INFO(L"Passwordless enrollment confirmed by session identity");
    return SaveEnrollmentImpl(L"", /*passwordless=*/true, label);
}

// Returns the SID of the currently logged-on session identity (the process
// token's user), used as the "self" proof for passwordless enrollment.
std::wstring EnrollmentWizard::GetCurrentProcessUserSid() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return L"";
    }
    std::wstring result;
    DWORD sz = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &sz);
    if (sz > 0) {
        std::vector<BYTE> buf(sz);
        if (GetTokenInformation(hToken, TokenUser, buf.data(), sz, &sz)) {
            auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
            LPWSTR sidStr = nullptr;
            if (ConvertSidToStringSidW(tu->User.Sid, &sidStr)) {
                result = sidStr;
                LocalFree(sidStr);
            }
        }
    }
    CloseHandle(hToken);
    return result;
}

// Detect whether the enrolled account is passwordless (no password — PIN/Hello
// only). Layered, conservative:
//   1. The current session identity must be the enrolled account.
//   2. MSA/Xbox: SAM never reflects the online password → 2 (UI checkbox lets
//      the user confirm). Never derive it from SAM/LogonUser.
//   3. Local accounts: empty-password LogonUser succeeds → 1 (passwordless).
//   4. NetUserGetInfo(1003) shows an empty SAM password → 1 (passwordless).
//   5. Anything else → 0 (has a password).
int EnrollmentWizard::GetPasswordlessState() const {
    // 1) Session identity must be the account being enrolled.
    std::wstring tokenSid = GetCurrentProcessUserSid();
    if (tokenSid.empty() || tokenSid != m_sid) {
        FACELOGIN_WARN(L"GetPasswordlessState: session identity mismatch → state=0");
        return 0;
    }

    // 2) MSA accounts: the SAM password field / empty LogonUser probe is NEVER
    // authoritative — the real credential is online (or a local cache of the
    // online password), so a SAM probe can falsely report "no password" for an
    // MSA account whose local cache was never written. Ask the user instead.
    if (m_accountType == "msa") {
        FACELOGIN_INFO(L"GetPasswordlessState: MSA account → state=2 (user confirm)");
        return 2;
    }

    // 3) Local accounts only: empty-password LogonUser probe.
    HANDLE hToken = nullptr;
    BOOL okEmpty = LogonUserW(m_username.c_str(), L".", L"",
                              LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &hToken);
    if (okEmpty && hToken) { CloseHandle(hToken); return 1; }

    // 4) NetUserGetInfo(1003): SAM password field empty → passwordless.
    // (USER_INFO_1003 exposes the SAM password; 23 does not include it.)
    USER_INFO_1003* ui1003 = nullptr;
    if (NetUserGetInfo(nullptr, m_username.c_str(), 1003,
                       reinterpret_cast<LPBYTE*>(&ui1003)) == NERR_Success && ui1003) {
        bool noPw = (ui1003->usri1003_password == nullptr ||
                     ui1003->usri1003_password[0] == L'\0');
        NetApiBufferFree(ui1003);
        if (noPw) { FACELOGIN_INFO(L"GetPasswordlessState: SAM password empty → state=1"); return 1; }
    }

    // 5) Local account with a non-empty SAM password → has a password.
    FACELOGIN_INFO(L"GetPasswordlessState: local account with password → state=0");
    return 0;
}

bool EnrollmentWizard::SaveEnrollmentImpl(const std::wstring& password, bool passwordless,
                                          const std::wstring& label) {
    if (m_embeddings.empty()) { FACELOGIN_ERROR(L"No face samples"); return false; }

    // Embedding consistency check: verify all samples are from the same person.
    // Compute average pairwise distance — if it exceeds the cap, reject.
    // Same-person distances are typically well below 0.80 (the ONNX boundary);
    // different people exceed it.
    //
    {
        double totalDist = 0.0;
        int pairs = 0;
        for (size_t i = 0; i < m_embeddings.size(); i++) {
            for (size_t j = i + 1; j < m_embeddings.size(); j++) {
                double sum = 0.0;
                for (long k = 0; k < m_embeddings[i].size(); k++) {
                    double diff = m_embeddings[i](k) - m_embeddings[j](k);
                    sum += diff * diff;
                }
                totalDist += std::sqrt(sum);
                pairs++;
            }
        }
        double avgPairDist = (pairs > 0) ? totalDist / pairs : 0.0;
        const float maxAllowed = 0.45f;
        FACELOGIN_INFO(L"Enrollment consistency: avg pairwise dist=%.4f (max=%.3f, %d pairs)",
                      avgPairDist, maxAllowed, pairs);
        if (avgPairDist > maxAllowed) {
            FACELOGIN_ERROR(L"Embedding consistency check failed: avg pairwise dist %.4f > %.3f. "
                           L"Samples may be from different faces.", avgPairDist, maxAllowed);
            return false;
        }
    }

    // Average the samples. Initialize to the samples' dimensionality — the
    // matrix adds require matching sizes (dlib asserts otherwise).
    dlib::matrix<float, 0, 1> avgEmbedding;
    if (!m_embeddings.empty()) {
        avgEmbedding.set_size(m_embeddings[0].size());
        avgEmbedding = 0;
        for (const auto& emb : m_embeddings) {
            if (emb.size() != avgEmbedding.size()) continue;  // defensive
            avgEmbedding += emb;
        }
        avgEmbedding /= static_cast<float>(m_embeddings.size());

        // The recognizer returns unit vectors, but averaging several samples
        // shortens the resultant vector. Re-normalize so V5 templates keep
        // the same Euclidean geometry as a single 512-D inference.
        float avgNorm = 0.0f;
        for (long i = 0; i < avgEmbedding.size(); ++i)
            avgNorm += avgEmbedding(i) * avgEmbedding(i);
        avgNorm = std::sqrt(avgNorm);
        if (avgNorm > 1e-8f) avgEmbedding /= avgNorm;
    }

    m_store.LoadDatabase();

    // Copy the average embedding into a plain float vector (full dimensionality —
    // 512-D for ONNX, 128-D for dlib). Never truncate.
    std::vector<float> ef(static_cast<size_t>(avgEmbedding.size()));
    for (long i = 0; i < avgEmbedding.size(); i++)
        ef[static_cast<size_t>(i)] = avgEmbedding(static_cast<long>(i));

    // create-or-append (1.3.0): the same account may hold several faces.
    // First-time enrollment stores the (protected) password and face #1;
    // subsequent enrollments APPEND a face and leave the stored password
    // untouched (the user is the logged-on session owner, already trusted).
    size_t idx = m_store.FindUserIndex(m_sid, m_upn, m_username);
    uint32_t newFaceId = 0;
    if (idx >= m_store.GetUsers().size()) {
        // First face for this account — protect the password now.
        std::vector<uint8_t> protectedPassword;
        if (passwordless) {
            protectedPassword = { facelogin::kPasswordlessSentinelByte };
            FACELOGIN_INFO(L"Storing passwordless enrollment sentinel");
        } else {
            protectedPassword = DpapiUtil::Protect(
                reinterpret_cast<const uint8_t*>(password.c_str()),
                static_cast<UINT>(password.size() * sizeof(wchar_t)));
            if (protectedPassword.empty()) { FACELOGIN_ERROR(L"DPAPI encryption failed"); return false; }
        }
        if (!m_store.AddFace(m_username, m_upn, m_sid, protectedPassword, ef, label, &newFaceId)) {
            FACELOGIN_ERROR(L"Failed to create enrollment record");
            return false;
        }
    } else {
        // Append a face to an existing account. AddFace ignores the password
        // argument here, so the stored password/sentinel is preserved.
        if (m_store.GetUsers()[idx].faces.size() >= facelogin::kMaxFacesPerUser) {
            FACELOGIN_ERROR(L"Cannot append: account already has %zu faces (max %zu)",
                            m_store.GetUsers()[idx].faces.size(),
                            facelogin::kMaxFacesPerUser);
            return false;
        }
        if (!m_store.AddFace(m_username, m_upn, m_sid, {}, ef, label, &newFaceId)) {
            FACELOGIN_ERROR(L"Failed to append face to enrollment record");
            return false;
        }
    }
    if (!m_store.SaveDatabase()) { FACELOGIN_ERROR(L"Failed to save database"); return false; }

    FACELOGIN_INFO(L"Enrollment saved (face #%u, emb=%zu-D%s)",
                   newFaceId, ef.size(),
                   passwordless ? L", passwordless" : L"");

    // Notify service to reload database
    HANDLE hPipe = CreateFileW(ipc::PIPE_NAME, GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (hPipe != INVALID_HANDLE_VALUE) {
        DWORD written;
        std::wstring msg(ipc::MSG_RELOAD_DB);
        msg.push_back(L'\0');
        WriteFile(hPipe, msg.c_str(), static_cast<DWORD>(msg.size() * sizeof(wchar_t)),
                  &written, nullptr);
        CloseHandle(hPipe);
    }
    return true;
}

// ============================================================================
// Multi-face management (1.3.0)
// ============================================================================

int EnrollmentWizard::GetFaceCount() {
    m_store.LoadDatabase();
    return static_cast<int>(m_store.GetFaceCount(m_sid));
}

bool EnrollmentWizard::NeedsReenrollment() {
    m_store.LoadDatabase();
    return m_store.NeedsReenrollment();
}

std::string EnrollmentWizard::GetFacesJson() {
    m_store.LoadDatabase();
    m_adaptiveLearning.Load();
    size_t idx = m_store.FindUserIndex(m_sid, m_upn, m_username);
    if (idx >= m_store.GetUsers().size()) return "[]";

    std::ostringstream js;
    js << "[";
    const auto& faces = m_store.GetUsers()[idx].faces;
    for (size_t i = 0; i < faces.size(); i++) {
        if (i > 0) js << ",";
        const auto& f = faces[i];
        js << "{\"id\":" << f.id
           << ",\"label\":\"" << WstrToUtf8Escaped(f.label) << "\""
           << ",\"legacy\":" << (f.legacy ? "true" : "false");
        const auto* archive = m_adaptiveLearning.FindArchive(m_sid, f.id);
        js << ",\"learning\":{\"sampleCount\":"
           << (archive ? archive->samples.size() : 0)
           << ",\"usedSampleCount\":"
           << (archive ? archive->UsedSampleCount() : 0)
           << ",\"prototypeCount\":"
           << (archive ? archive->groups.size() : 0)
           << ",\"enabled\":" << (archive && archive->enabled ? "true" : "false")
           << "}}";
    }
    js << "]";
    return js.str();
}

bool EnrollmentWizard::SaveEnrollmentAppend(const std::wstring& label) {
    // The appended face belongs to the logged-on session owner — the session
    // token SID must match the enrolled account (same self-proof as the
    // passwordless flow). No password is required for an append.
    std::wstring tokenSid = GetCurrentProcessUserSid();
    if (tokenSid.empty() || tokenSid != m_sid) {
        FACELOGIN_ERROR(L"Face append refused: session identity mismatch");
        return false;
    }
    return SaveEnrollmentImpl(L"", /*passwordless=*/false, label);
}

bool EnrollmentWizard::DeleteFace(int faceId) {
    if (faceId <= 0) return false;
    m_store.LoadDatabase();
    if (!m_store.DeleteFace(m_sid, static_cast<uint32_t>(faceId))) return false;
    if (!m_store.SaveDatabase()) return false;
    m_adaptiveLearning.DeleteArchive(m_sid, static_cast<uint32_t>(faceId));
    NotifyServiceReload();
    return true;
}

bool EnrollmentWizard::ClearAllFaces() {
    m_store.LoadDatabase();
    if (!m_store.ClearAllFaces(m_sid)) return false;
    if (!m_store.SaveDatabase()) return false;
    m_adaptiveLearning.DeleteAllForSid(m_sid);
    NotifyServiceReload();
    return true;
}

bool EnrollmentWizard::RenameFace(int faceId, const std::wstring& label) {
    if (faceId <= 0) return false;
    m_store.LoadDatabase();
    if (!m_store.RenameFace(m_sid, static_cast<uint32_t>(faceId), label)) return false;
    if (!m_store.SaveDatabase()) return false;
    NotifyServiceReload();
    return true;
}

std::string EnrollmentWizard::GetAdaptiveArchiveJson(int faceId) {
    if (faceId <= 0) return "[]";
    m_store.LoadDatabase();
    const size_t userIndex = m_store.FindUserIndex(m_sid, m_upn, m_username);
    if (userIndex >= m_store.GetUsers().size()) {
        return "[]";
    }
    const auto& faces = m_store.GetUsers()[userIndex].faces;
    const bool found = std::any_of(faces.begin(), faces.end(), [faceId](const FaceRecord& face) {
        return face.id == static_cast<uint32_t>(faceId);
    });
    if (!found) {
        return "[]";
    }
    m_adaptiveLearning.Load();
    return m_adaptiveLearning.GetArchivesJson(m_sid);
}

bool EnrollmentWizard::ClaimUnknownFaceForLearning(const std::string& file, int faceId) {
    if (faceId <= 0 || file.empty() || file.find_first_of("/\\\\:") != std::string::npos) {
        return false;
    }
    m_store.LoadDatabase();
    const size_t userIndex = m_store.FindUserIndex(m_sid, m_upn, m_username);
    if (userIndex >= m_store.GetUsers().size()) return false;
    const auto& faces = m_store.GetUsers()[userIndex].faces;
    const auto faceIt = std::find_if(faces.begin(), faces.end(), [faceId](const FaceRecord& face) {
        return face.id == static_cast<uint32_t>(faceId) && !face.legacy;
    });
    if (faceIt == faces.end()) return false;

    const std::wstring sourceName = Utf8ToWstr(file);
    const std::wstring sourcePath = m_dataDir + L"\\data\\unknown\\" + sourceName;
    const DWORD attrs = GetFileAttributesW(sourcePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;

    // A historical failed frame is never accepted blindly. Re-run the current
    // detector, landmarks and recognizer against the stored source frame.
    if (!EnsureModelsLoaded()) return false;
    dlib::matrix<dlib::rgb_pixel> image;
    if (!DecodeJpegWithWic(sourcePath, image) || image.size() == 0) return false;
    const auto detections = m_onnxDetector->Detect(image);
    if (detections.size() != 1) return false;
    const auto& detection = detections.front();
    dlib::rectangle rect(static_cast<long>(detection.x1), static_cast<long>(detection.y1),
                         static_cast<long>(detection.x2), static_cast<long>(detection.y2));
    if (rect.is_empty() || rect.width() < 80 || rect.height() < 80) return false;
    dlib::full_object_detection landmarks;
    if (!m_detector->DetectLandmarks(image, rect, landmarks) || landmarks.num_parts() != 106) return false;
    const auto embedding = m_onnxRecognizer->ComputeEmbedding(image, landmarks);
    if (embedding.empty()) return false;

    if (!m_adaptiveLearning.Load()) return false;
    const std::wstring sampleName = AdaptiveSampleName(static_cast<uint32_t>(faceId));
    const std::wstring samplePath = m_dataDir + L"\\data\\adaptive\\samples\\" + sampleName;
    if (!CopyFileW(sourcePath.c_str(), samplePath.c_str(), TRUE)) return false;
    if (!m_adaptiveLearning.AddSample(m_sid, static_cast<uint32_t>(faceId), sampleName,
                                      embedding, CurrentFileTimeTicks())) {
        DeleteFileW(samplePath.c_str());
        return false;
    }
    NotifyServiceReload();
    return true;
}

std::string EnrollmentWizard::RebuildAdaptiveArchive(int faceId) {
    AdaptiveBuildResult result;
    if (faceId <= 0) return result.ToJson();
    m_store.LoadDatabase();
    const size_t userIndex = m_store.FindUserIndex(m_sid, m_upn, m_username);
    if (userIndex >= m_store.GetUsers().size()) return result.ToJson();
    const auto& faces = m_store.GetUsers()[userIndex].faces;
    const bool found = std::any_of(faces.begin(), faces.end(), [faceId](const FaceRecord& face) {
        return face.id == static_cast<uint32_t>(faceId) && !face.legacy;
    });
    if (!found) return result.ToJson();
    result = m_adaptiveLearning.RebuildArchive(m_sid, static_cast<uint32_t>(faceId));
    if (result.status == AdaptiveBuildStatus::Success) NotifyServiceReload();
    return result.ToJson();
}

bool EnrollmentWizard::SetAdaptiveArchiveEnabled(int faceId, bool enabled) {
    if (faceId <= 0) return false;
    m_store.LoadDatabase();
    const size_t userIndex = m_store.FindUserIndex(m_sid, m_upn, m_username);
    if (userIndex >= m_store.GetUsers().size()) return false;
    const auto& faces = m_store.GetUsers()[userIndex].faces;
    const bool found = std::any_of(faces.begin(), faces.end(), [faceId](const FaceRecord& face) {
        return face.id == static_cast<uint32_t>(faceId) && !face.legacy;
    });
    if (!found) return false;
    if (!m_adaptiveLearning.SetArchiveEnabled(m_sid, static_cast<uint32_t>(faceId), enabled)) return false;
    NotifyServiceReload();
    return true;
}

bool EnrollmentWizard::DeleteAdaptiveArchive(int faceId) {
    if (faceId <= 0) return false;
    m_store.LoadDatabase();
    const size_t userIndex = m_store.FindUserIndex(m_sid, m_upn, m_username);
    if (userIndex >= m_store.GetUsers().size()) return false;
    const auto& faces = m_store.GetUsers()[userIndex].faces;
    const bool found = std::any_of(faces.begin(), faces.end(), [faceId](const FaceRecord& face) {
        return face.id == static_cast<uint32_t>(faceId) && !face.legacy;
    });
    if (!found) return false;
    if (!m_adaptiveLearning.DeleteArchive(m_sid, static_cast<uint32_t>(faceId))) return false;
    NotifyServiceReload();
    return true;
}

// Detect a stale account-type record (symmetric MSA ↔ local). We never trust
// m_upn/m_accountType here — they are derived from the session at construction
// and could be empty for local accounts; instead we re-query the CURRENT
// session identity with the authoritative GetSessionUpn() (docs/todo.md bug1):
//   UPN contains '@'  → current account is MSA
//   empty (err 1332)  → current account is local
// Then compare against the stored record (matched by SID, which Windows keeps
// across MSA↔local conversions):
//   local + record UPN is an MSA email  → state 1 (MSA→local, clear UPN)
//   MSA   + record UPN empty/different  → state 2 (local→MSA, write current UPN)
//   everything else                     → state 0 (no refresh needed)
int EnrollmentWizard::GetAccountTypeChanged() {
    // Determine the CURRENT session's account type via the authoritative
    // query used everywhere else (docs/todo.md bug1). Empty (err 1332) means
    // no UPN → local account.
    std::wstring curUpn = GetSessionUpn();
    bool sessionIsMsa = !curUpn.empty() && curUpn.find(L'@') != std::wstring::npos;

    // Match the current identity against stored records (same priority as
    // FindUserIndex: SID > UPN > username).
    m_store.LoadDatabase();
    std::wstring tokenSid = GetCurrentProcessUserSid();
    size_t idx = m_store.FindUserIndex(tokenSid, m_upn, m_username);
    if (idx >= m_store.GetUsers().size()) return 0;  // not enrolled → normal first-time flow

    const auto& rec = m_store.GetUsers()[idx];
    if (!sessionIsMsa) {
        // Current account is local. Flag if the record still carries an MSA email.
        if (rec.upn.find(L'@') != std::wstring::npos) {
            FACELOGIN_INFO(L"GetAccountTypeChanged: stale MSA→local record (faces=%zu)",
                           rec.faces.size());
            return 1;
        }
        return 0;
    }

    // Current account is MSA. Flag if the record UPN is empty (local-era) or a
    // different email than the current session's MSA identity.
    if (rec.upn.empty() || rec.upn != curUpn) {
        FACELOGIN_INFO(L"GetAccountTypeChanged: stale local→MSA record (storedUpn=%d, faces=%zu)",
                       rec.upn.empty() ? 0 : 1, rec.faces.size());
        return 2;
    }
    return 0;
}

std::string EnrollmentWizard::CheckAccountTypeChanged() {
    int state = GetAccountTypeChanged();
    if (state == 0) return "{\"state\":0}";

    // Attach the face count so the prompt can say "你已录入 N 张人脸", and for
    // state 2 the current MSA email so the prompt can show what will be written.
    m_store.LoadDatabase();
    std::wstring tokenSid = GetCurrentProcessUserSid();
    size_t idx = m_store.FindUserIndex(tokenSid, m_upn, m_username);
    size_t faces = (idx < m_store.GetUsers().size()) ? m_store.GetUsers()[idx].faces.size() : 0;

    if (state == 2) {
        // Re-derive the current MSA email (same authoritative query).
        std::wstring curUpn = GetSessionUpn();
        return "{\"state\":2,\"faces\":" + std::to_string(faces) +
               ",\"upn\":\"" + WstrToUtf8Escaped(curUpn) + "\"}";
    }
    return "{\"state\":1,\"faces\":" + std::to_string(faces) + "}";
}

bool EnrollmentWizard::RefreshAccountIdentity(const std::wstring& password) {
    int state = GetAccountTypeChanged();
    if (state == 0) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: no stale record to refresh");
        return false;
    }
    if (password.empty()) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: empty password");
        return false;
    }

    // Validate the CURRENT password before touching the database.
    // ValidatePassword routes by account type: MSA (UPN contains '@') goes
    // through CloudAP online/cached validation, local uses LogonUserW(".").
    if (!ValidatePassword(password)) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: password validation failed");
        return false;
    }

    // Re-encrypt with DPAPI (machine scope — matches first enrollment).
    std::vector<uint8_t> protectedPassword = DpapiUtil::Protect(password);
    if (protectedPassword.empty()) {
        FACELOGIN_ERROR(L"RefreshAccountIdentity: DPAPI encryption failed");
        return false;
    }

    std::wstring tokenSid = GetCurrentProcessUserSid();
    m_store.LoadDatabase();
    size_t idx = m_store.FindUserIndex(tokenSid, m_upn, m_username);
    if (idx >= m_store.GetUsers().size()) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: record vanished before update");
        return false;
    }

    // Write the identity matching the CURRENT account type:
    //   state 1 (MSA→local): clear the old MSA UPN.
    //   state 2 (local→MSA): write the current MSA email (session UPN).
    std::wstring newUpn;
    if (state == 2) {
        newUpn = GetSessionUpn();
    }
    // state 1 → newUpn stays empty (local account).

    if (!m_store.UpdateAccountIdentity(idx, m_username, newUpn, tokenSid, protectedPassword)) {
        FACELOGIN_ERROR(L"RefreshAccountIdentity: identity update failed");
        return false;
    }
    if (!m_store.SaveDatabase()) {
        FACELOGIN_ERROR(L"RefreshAccountIdentity: save failed");
        return false;
    }

    NotifyServiceReload();
    FACELOGIN_INFO(L"RefreshAccountIdentity: identity refreshed (upnPresent=%d accountKind=%s, faces preserved)",
                   newUpn.empty() ? 0 : 1, state == 2 ? L"online" : L"local");
    return true;
}

// Dismiss path for the stale-account prompt. Unlike RefreshAccountIdentity
// this needs NO user input: it only drops the '@'-carrying UPN that a buggy
// build wrote into a LOCAL account's record (docs/todo.md bug1), keeping
// username, SID, faces and the DPAPI-encrypted password byte-for-byte intact.
//   - state 1 (current session local + record carries an MSA email): the
//     email is either a misattribution (another account / converted MSA) —
//     clearing it makes the lock-screen pack domain\username, fixing the
//     silent login failure — or a genuine MSA→local conversion where the user
//     simply doesn't want to re-enter the password; identity is corrected and
//     if the password changed they should still run the full refresh.
//   - state 2 (local→MSA) is deliberately NOT touched: the record needs the
//     CURRENT email + re-encrypted password, which only the refresh provides.
bool EnrollmentWizard::ClearStaleAccountUpn() {
    int state = GetAccountTypeChanged();
    if (state != 1) {
        FACELOGIN_INFO(L"ClearStaleAccountUpn: not a stale MSA→local record (state=%d), no-op", state);
        return false;
    }

    // FindUserIndex tries SID first, then UPN, then username. We pass an
    // EMPTY upn on purpose: m_upn is the value we are about to clear (the
    // stale MSA email that a buggy build wrote into a local record), so
    // matching by it would be circular and could pick another account's
    // record if SIDs ever collided. The process-token SID is the trusted
    // identity proof here (same source as passwordless enrollment), and
    // username is kept as a last-resort fallback. See docs/todo.md bug1.
    std::wstring tokenSid = GetCurrentProcessUserSid();
    m_store.LoadDatabase();
    size_t idx = m_store.FindUserIndex(tokenSid, L"", m_username);
    if (idx >= m_store.GetUsers().size()) {
        FACELOGIN_WARN(L"ClearStaleAccountUpn: record vanished before update");
        return false;
    }
    const auto& rec = m_store.GetUsers()[idx];
    if (rec.upn.find(L'@') == std::wstring::npos) {
        FACELOGIN_INFO(L"ClearStaleAccountUpn: record already has no MSA email, no-op");
        return false;
    }

    // Pass the record's own encryptedPassword back unchanged so the stored
    // credential is not re-encrypted (only the identity email is dropped).
    if (!m_store.UpdateAccountIdentity(idx, rec.username, L"", tokenSid, rec.encryptedPassword)) {
        FACELOGIN_ERROR(L"ClearStaleAccountUpn: identity update failed");
        return false;
    }
    if (!m_store.SaveDatabase()) {
        FACELOGIN_ERROR(L"ClearStaleAccountUpn: save failed");
        return false;
    }

    NotifyServiceReload();
    FACELOGIN_INFO(L"ClearStaleAccountUpn: cleared stale MSA UPN (faces=%zu, password untouched)",
                   rec.faces.size());
    return true;
}

// ============================================================================
// Camera device enumeration
// ============================================================================

std::string EnrollmentWizard::GetCameraList() {
    auto devices = WebcamCapture::ListCameras();
    std::ostringstream js;
    js << "[";
    for (size_t i = 0; i < devices.size(); i++) {
        if (i > 0) js << ",";
        // JSON-escape name/path (device paths contain backslashes).
        auto esc = [](const std::wstring& ws) -> std::string {
            std::string out;
            for (wchar_t ch : ws) {
                if (ch == L'"' || ch == L'\\') out.push_back('\\');
                char mb[4] = {};
                int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 4, nullptr, nullptr);
                if (n > 0) out.append(mb, static_cast<size_t>(n));
            }
            return out;
        };
        js << "{\"path\":\"" << esc(devices[i].devicePath)
           << "\",\"name\":\"" << esc(devices[i].friendlyName) << "\"}";
    }
    js << "]";
    return js.str();
}

// ============================================================================
// Configuration
// ============================================================================

// Console version — bump with each release. Used to decide whether to show the
// About-card star hint again (it reappears on every new version).
static const wchar_t FACELOGIN_CONSOLE_VERSION[] = L"2.0.0";
// Registry value holding the version the user last saw the About card at.
static const wchar_t REGVAL_ABOUT_SEEN_VERSION[] = L"AboutSeenVersion";

// The About-card star is a per-version one-time hint. Persisted in the
// registry (HKLM\SOFTWARE\FaceLogin\AboutSeenVersion) rather than a file so
// it leaves no trace in the data dir and rides along with the normal config
// registry key. (localStorage is unavailable — the page is served via
// NavigateToString → opaque origin.)
bool EnrollmentWizard::GetAboutSeen() {
    std::wstring seen = ReadRegString(REGVAL_ABOUT_SEEN_VERSION, L"");
    return seen == FACELOGIN_CONSOLE_VERSION;
}

void EnrollmentWizard::SetAboutSeen(bool seen) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, FACELOGIN_REG_KEY,
            0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
            &hKey, nullptr) == ERROR_SUCCESS) {
        if (seen) {
            RegSetValueExW(hKey, REGVAL_ABOUT_SEEN_VERSION, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(FACELOGIN_CONSOLE_VERSION),
                static_cast<DWORD>((wcslen(FACELOGIN_CONSOLE_VERSION) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, REGVAL_ABOUT_SEEN_VERSION);
        }
        RegCloseKey(hKey);
    }
}

std::string EnrollmentWizard::GetConsoleVersion() const {
    // Version string is ASCII; narrow conversion is lossless.
    std::wstring wv = FACELOGIN_CONSOLE_VERSION;
    return std::string(wv.begin(), wv.end());
}

std::string EnrollmentWizard::GetConfig() const {
    return ConfigToJson(m_config);
}

bool EnrollmentWizard::SetConfig(const std::string& json) {
    AppConfig newConfig = ConfigFromJson(json);
    // Validate: log a warning if anti-spoof model is unavailable,
    // but still save the user's choice — runtime will fall back as needed.
    if (newConfig.liveness_method == LivenessMethod::AntiSpoof &&
        (!m_antiSpoof || !m_antiSpoof->IsInitialized())) {
        FACELOGIN_WARN(L"SetConfig: anti-spoof configured but model not available; saved anyway (will fall back at runtime)");
    }

    bool cameraChanged = (newConfig.camera_device != m_config.camera_device);

    if (!SaveConfig(m_dataDir, newConfig)) {
        FACELOGIN_ERROR(L"Failed to save config.json");
        return false;
    }
    // Mirror the cold-boot key-trigger choice to the registry — the
    // credential provider (LogonUI) cannot read config.json reliably, so the
    // registry is the CP↔Console channel for this behavior.
    WriteRegDword(REGVAL_COLD_BOOT_KEY_TRIGGER, newConfig.cold_boot_key_trigger ? 1 : 0);
    m_config = newConfig;
    m_livenessMethod = newConfig.liveness_method;
    m_antiSpoofThreshold = newConfig.anti_spoof_threshold;
    // Runtime fallback: if anti-spoof is chosen but model is missing, degrade now
    if (m_livenessMethod == LivenessMethod::AntiSpoof && (!m_antiSpoof || !m_antiSpoof->IsInitialized())) {
        FACELOGIN_WARN(L"SetConfig: runtime fallback to blink (anti-spoof model unavailable)");
        m_livenessMethod = LivenessMethod::Blink;
    }

    // Notify service to reload config
    HANDLE hPipe = CreateFileW(ipc::PIPE_NAME, GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (hPipe != INVALID_HANDLE_VALUE) {
        DWORD written;
        std::wstring msg(ipc::MSG_CONFIG_RELOAD);
        msg.push_back(L'\0');
        WriteFile(hPipe, msg.c_str(), static_cast<DWORD>(msg.size() * sizeof(wchar_t)),
                  &written, nullptr);
        CloseHandle(hPipe);
    }

    FACELOGIN_INFO(L"Configuration updated: rec=%hs det=%hs live=%hs thr=%.2f rotation=%d",
                  m_config.recognition_model.c_str(), m_config.detector.c_str(),
                  LivenessMethodToString(m_config.liveness_method).c_str(),
                  m_config.match_threshold, m_config.camera_rotation);

    // If the camera selection changed and the preview is running, restart the
    // preview so the new camera takes effect immediately.
    if (cameraChanged && m_previewRunning) {
        FACELOGIN_INFO(L"Camera selection changed — restarting preview");
        RestartPreview();
    }

    return true;
}

bool EnrollmentWizard::RestartPreview() {
    StopPreview();
    return StartPreview();
}

// ============================================================================
// Log Viewer
// ============================================================================

std::string EnrollmentWizard::GetLogLines() {
    auto lines = Logger::Instance().GetRecentLogs(500);
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) ss << ",";
        ss << "\"";
        for (wchar_t ch : lines[i]) {
            if (ch == L'\\') ss << "\\\\";
            else if (ch == L'"') ss << "\\\"";
            else if (ch == L'\r' || ch == L'\n') {} // strip newlines — JS renders as <div>
            else {
                char mb[4] = {};
                int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 4, nullptr, nullptr);
                if (n > 0) ss.write(mb, n);
            }
        }
        ss << "\"";
    }
    ss << "]";
    return ss.str();
}

// Minimal JSONL field extractor for the unknown-face events file (fields are
// written by the service as flat "key":"value" / "key":number entries).
static std::string JsonlField(const std::string& line, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = line.find(needle);
    if (pos == std::string::npos) return "";
    pos = line.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) pos++;
    if (pos < line.size() && line[pos] == '"') {
        std::string val;
        pos++;
        while (pos < line.size() && line[pos] != '"') {
            if (line[pos] == '\\' && pos + 1 < line.size()) {
                val += line[pos + 1];
                pos += 2;
            } else {
                val += line[pos++];
            }
        }
        return val;
    }
    std::string val;
    while (pos < line.size() &&
           ((line[pos] >= '0' && line[pos] <= '9') || line[pos] == '.' || line[pos] == '-')) {
        val += line[pos++];
    }
    return val;
}

std::string EnrollmentWizard::GetUnknownFaceEvents() {
    // Read <dataDir>\data\unknown\events.jsonl and return only records whose
    // JPEG still exists on disk (the service rolls the directory at 100 files,
    // so the JSONL may reference deleted photos). Newest first.
    std::wstring dir = m_dataDir + L"\\data\\unknown";

    std::vector<std::string> jpgs;
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW((dir + L"\\*.jpg").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                char mb[MAX_PATH] = {};
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, mb, MAX_PATH, nullptr, nullptr);
                jpgs.push_back(mb);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    std::ifstream f((dir + L"\\events.jsonl").c_str());
    std::vector<std::string> records;   // raw lines that still have a photo
    if (f) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::string file = JsonlField(line, "file");
            if (file.empty()) continue;
            if (std::find(jpgs.begin(), jpgs.end(), file) == jpgs.end()) continue;
            records.push_back(line);
        }
    }

    std::ostringstream out;
    out << "[";
    for (size_t i = records.size(); i-- > 0;) {
        const std::string& line = records[i];
        if (i + 1 < records.size()) out << ",";   // not the first emitted (newest)
        out << "{";
        out << "\"ts\":\"" << JsonlField(line, "ts") << "\",";
        out << "\"reason\":\"" << JsonlField(line, "reason") << "\",";
        std::string dist = JsonlField(line, "bestDistance");
        out << "\"distance\":" << (dist.empty() ? "-1" : dist) << ",";
        out << "\"file\":\"" << JsonlField(line, "file") << "\"";
        out << "}";
    }
    out << "]";
    return out.str();
}

// Rewrite events.jsonl keeping only lines that do NOT reference the given
// file (or all lines when file is empty — i.e. keep-everything is not used;
// empty means "remove nothing" is handled by callers). Returns the lines kept.
static void RewriteUnknownEvents(const std::wstring& dir,
                                 const std::string& removeFile) {
    std::wstring path = dir + L"\\events.jsonl";
    std::vector<std::string> keep;
    {
        std::ifstream f(path.c_str());
        if (f) {
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty()) continue;
                std::string file = JsonlField(line, "file");
                if (!removeFile.empty() && file == removeFile) continue;
                keep.push_back(line);
            }
        }
    }
    std::ofstream out(path.c_str(), std::ios::trunc);
    if (out) {
        for (const auto& line : keep) out << line << "\n";
    }
}

bool EnrollmentWizard::DeleteUnknownFace(const std::string& file) {
    // Filenames are console-supplied ASCII from the events list — still guard
    // against path traversal defensively.
    if (file.empty() || file.find_first_of("/\\:") != std::string::npos) return false;

    std::wstring dir = m_dataDir + L"\\data\\unknown";
    std::wstring wfile(file.begin(), file.end());
    if (!DeleteFileW((dir + L"\\" + wfile).c_str())) return false;

    RewriteUnknownEvents(dir, file);
    return true;
}

bool EnrollmentWizard::ClearUnknownFaces() {
    std::wstring dir = m_dataDir + L"\\data\\unknown";
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            DeleteFileW((dir + L"\\" + n).c_str());
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    DeleteFileW((dir + L"\\events.jsonl").c_str());
    return true;
}

std::string EnrollmentWizard::GetServiceLogLines() {
    // Read the service log file directly — avoids pipe message size limits.
    // The log file is written in UTF-16LE (wchar_t on Windows).
    std::wstring logPath = m_dataDir + L"\\log\\service.log";
    HANDLE hFile = CreateFileW(logPath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return "[\"Service log file not available\"]";
    }

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize < 2) {
        CloseHandle(hFile);
        return "[\"Service log is empty\"]";
    }

    // Cap at ~256KB of raw bytes, reading the TAIL (latest log lines) so a
    // long-running service doesn't show the oldest logs forever.
    DWORD capSize = 256 * 1024;
    LARGE_INTEGER li = {};
    if (fileSize > capSize) {
        li.QuadPart = static_cast<LONGLONG>(fileSize) - capSize;
        SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN);
    }
    DWORD toRead = (fileSize < capSize) ? fileSize : capSize;

    std::vector<wchar_t> wbuf(toRead / sizeof(wchar_t) + 1);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, wbuf.data(), toRead, &bytesRead, nullptr) || bytesRead < 2) {
        CloseHandle(hFile);
        return "[\"Failed to read service log\"]";
    }
    CloseHandle(hFile);

    size_t wlen = bytesRead / sizeof(wchar_t);
    size_t start = 0;
    if (fileSize > capSize) {
        // We started mid-line — drop the partial first line so only complete
        // lines are shown. Find the first \n after the start.
        while (start < wlen && wbuf[start] != L'\r' && wbuf[start] != L'\n') start++;
        while (start < wlen && (wbuf[start] == L'\r' || wbuf[start] == L'\n')) start++;
    }

    // Parse lines: each log line ends with \r\n (wchar_t)
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    size_t pos = start;
    while (pos < wlen) {
        // Find end of line
        size_t lineStart = pos;
        while (pos < wlen && wbuf[pos] != L'\r' && wbuf[pos] != L'\n') pos++;
        size_t lineLen = pos - lineStart;
        // Skip \r\n
        while (pos < wlen && (wbuf[pos] == L'\r' || wbuf[pos] == L'\n')) pos++;
        if (lineLen == 0) continue;

        if (!first) ss << ",";
        first = false;
        ss << "\"";
        for (size_t i = 0; i < lineLen; i++) {
            wchar_t ch = wbuf[lineStart + i];
            if (ch == L'\\') ss << "\\\\";
            else if (ch == L'"') ss << "\\\"";
            else if (ch >= 0x20 && ch < 0x7F) ss << static_cast<char>(ch);
            else {
                // Non-ASCII character — convert via UTF-8
                char mb[4] = {};
                int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 4, nullptr, nullptr);
                if (n > 0) ss.write(mb, n);
            }
        }
        ss << "\"";
    }
    ss << "]";
    return ss.str();
}

void EnrollmentWizard::ClearLog() {
    Logger::Instance().ClearLogs();
}

} // namespace facelogin
