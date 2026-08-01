#include "EnrollmentWizard.h"
#include "../common/logger.h"
#include "../common/dpapi_util.h"
#include "../common/ipc_protocol.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include <comdef.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wincred.h>
#include <sddl.h>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <chrono>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace facelogin {

// Helper: convert UTF-8 string to wide string (for config camera_device)
static std::wstring Utf8ToWstr(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
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

    // Get UPN (UserPrincipalName)
    {
        ULONG upnSize = 256;
        std::vector<wchar_t> upnBuf(upnSize);
        HMODULE hSecur32 = LoadLibraryW(L"secur32.dll");
        if (hSecur32) {
            typedef BOOLEAN (WINAPI *PFN_GetUserNameExW)(int, LPWSTR, PULONG);
            auto pfn = reinterpret_cast<PFN_GetUserNameExW>(
                GetProcAddress(hSecur32, "GetUserNameExW"));
            if (pfn) {
                if (pfn(8 /* NameUserPrincipal */, upnBuf.data(), &upnSize)) {
                    m_upn = upnBuf.data();
                } else {
                    DWORD err = GetLastError();
                    FACELOGIN_WARN(L"GetUserNameExW NameUserPrincipal FAILED: err=%lu", err);
                }
            } else {
                FACELOGIN_ERROR(L"GetUserNameExW not found in secur32.dll");
            }
            // Also try NameSamCompatible to see what we get
            {
                ULONG samSize = 256;
                std::vector<wchar_t> samBuf(samSize);
                if (pfn && pfn(2 /* NameSamCompatible */, samBuf.data(), &samSize)) {
                    // SAM-compatible name captured (not logged — may contain PII)
                } else {
                    DWORD err = GetLastError();
                    FACELOGIN_WARN(L"GetUserNameExW NameSamCompatible FAILED: err=%lu", err);
                }
            }
            // Try NameDisplay
            {
                ULONG dispSize = 256;
                std::vector<wchar_t> dispBuf(dispSize);
                if (pfn && pfn(3 /* NameDisplay */, dispBuf.data(), &dispSize)) {
                    // Display name captured (not logged — may contain PII)
                } else {
                    DWORD err = GetLastError();
                    FACELOGIN_WARN(L"GetUserNameExW NameDisplay FAILED: err=%lu", err);
                }
            }
            FreeLibrary(hSecur32);
        } else {
            FACELOGIN_ERROR(L"LoadLibrary secur32.dll FAILED: err=%lu", GetLastError());
        }
    }

    // Try reading MSA UPN from IdentityStore registry
    // On Win10+, MSA accounts store UPN in HKLM\SOFTWARE\Microsoft\IdentityStore\...
    {
        // First, enumerate the IdentityStore provider GUIDs from
        // HKLM\SOFTWARE\Microsoft\IdentityCRL\StoredIdentities\{SID}
        // but only if we have a valid SID.
        if (!m_sid.empty()) {
            wchar_t storedIdentitiesPath[1024];
            swprintf(storedIdentitiesPath, 1023,
                     L"SOFTWARE\\Microsoft\\IdentityCRL\\StoredIdentities\\%s",
                     m_sid.c_str());
            HKEY hStored = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, storedIdentitiesPath,
                0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &hStored) == ERROR_SUCCESS) {
                wchar_t subKeyName[256];
                DWORD idx = 0;
                while (RegEnumKeyW(hStored, idx++, subKeyName, 256) == ERROR_SUCCESS) {
                    wchar_t emailName[512] = {};
                    DWORD emailSize = sizeof(emailName);
                    RegQueryValueExW(hStored, L"UPN", nullptr, nullptr,
                        reinterpret_cast<LPBYTE>(emailName), &emailSize);
                    if (emailName[0] != L'\0') {
                        if (m_upn.empty()) {
                            m_upn = emailName;
                        }
                        break;
                    }
                }
                RegCloseKey(hStored);
            }
        }

        // Also try IdentityStore via the D7F9888F provider (MicrosoftAccount)
        wchar_t idStorePath[] = L"SOFTWARE\\Microsoft\\IdentityStore\\LogonCache\\D7F9888F-E3FC-49b0-9EA6-A85B5F392A4F\\Name2Sid";
        HKEY hIdStore = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, idStorePath,
            0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &hIdStore) == ERROR_SUCCESS) {
            wchar_t hashName[256];
            DWORD idx2 = 0;
            while (RegEnumKeyW(hIdStore, idx2++, hashName, 256) == ERROR_SUCCESS) {
                HKEY hEntry = nullptr;
                if (RegOpenKeyExW(hIdStore, hashName, 0, KEY_QUERY_VALUE, &hEntry) == ERROR_SUCCESS) {
                    wchar_t identityName[256] = {};
                    DWORD nameSize = sizeof(identityName);
                    wchar_t sidValue[256] = {};
                    DWORD sidSize = sizeof(sidValue);

                    RegQueryValueExW(hEntry, L"IdentityName", nullptr, nullptr,
                        reinterpret_cast<LPBYTE>(identityName), &nameSize);
                    RegQueryValueExW(hEntry, L"Sid", nullptr, nullptr,
                        reinterpret_cast<LPBYTE>(sidValue), &sidSize);

                    if (identityName[0] != L'\0' && wcschr(identityName, L'@')) {
                        // This is an MSA email UPN
                        if (m_upn.empty()) {
                            m_upn = identityName;
                        }
                    }
                    RegCloseKey(hEntry);
                }
            }
            RegCloseKey(hIdStore);
        }
    }

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
    m_detector   = std::make_unique<FaceDetector>();
    m_store.SetDataDir(m_dataDir);

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

    if (!m_webcam->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
        FACELOGIN_ERROR(L"Failed to initialize webcam%s",
                        m_config.camera_device.empty() ? L"" : L" (configured device)");
        return false;
    }

    std::wstring modelsDir = m_dataDir + L"\\models";
    std::wstring shapePath = modelsDir + L"\\shape_predictor_68_face_landmarks.dat";

    if (!m_detector->Initialize(shapePath)) {
        FACELOGIN_ERROR(L"Failed to load shape predictor");
        m_webcam->Shutdown();
        return false;
    }

    // Load SCRFD ONNX detector (the only detector).
    m_onnxDetector = std::make_unique<OnnxDetector>();
    std::wstring detPath = modelsDir + L"\\det_500m.onnx";
    if (!m_onnxDetector->Initialize(detPath)) {
        FACELOGIN_ERROR(L"SCRFD detector failed to load — enrollment unavailable");
        m_webcam->Shutdown();
        return false;
    }

    // Load InsightFace ONNX recognizer (the only recognizer).
    m_onnxRecognizer = std::make_unique<OnnxRecognizer>();
    std::wstring onnxPath = modelsDir + L"\\w600k_mbf.onnx";
    if (!m_onnxRecognizer->Initialize(onnxPath)) {
        FACELOGIN_ERROR(L"ONNX recognizer failed to load — enrollment unavailable");
        m_webcam->Shutdown();
        return false;
    }

    // Try loading anti-spoof model
    m_antiSpoof = std::make_unique<OnnxAntiSpoof>();
    std::wstring antiSpoofPath = modelsDir + L"\\OULU_Protocol_2_model_0_0.onnx";
    if (!m_antiSpoof->Initialize(antiSpoofPath)) {
        FACELOGIN_WARN(L"Anti-spoof model not available");
        m_antiSpoof.reset();
    }

    // dlib recognizer/detector were removed — pure ONNX. recognition_model
    // and detector config values are ignored.

    // Validate liveness method
    if (m_livenessMethod == LivenessMethod::AntiSpoof && (!m_antiSpoof || !m_antiSpoof->IsInitialized())) {
        FACELOGIN_WARN(L"Anti-spoof configured but unavailable, falling back to blink");
        m_livenessMethod = LivenessMethod::Blink;
    }

    FACELOGIN_INFO(L"Liveness method: %hs | Preview started: 1280x720",
                  m_livenessMethod == LivenessMethod::Blink ? "blink" :
                  m_livenessMethod == LivenessMethod::AntiSpoof ? "antispoof" : "none");

    m_previewRunning = true;
    m_frameRunning = true;

    // Single background thread: GrabFrame → JPEG encode → detect → update caches.
    // The UI thread stays completely free; JS polls the caches via GetLatest*().
    m_frameThread = std::thread([this]() {
        while (m_frameRunning) {
            dlib::matrix<dlib::rgb_pixel> frame;
            if (!m_webcam->GrabFrame(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            std::string b64 = EncodeJPEGBase64(frame);
            // Preview overlay: detect the face with SCRFD and show its box.
            std::string faceJson = "[]";
            if (m_onnxDetector) {
                auto det = m_onnxDetector->DetectLargestFace(frame);
                if (det) {
                    std::vector<facelogin::FaceWithLandmarks> faces;
                    FaceWithLandmarks fwl;
                    fwl.rect = dlib::rectangle(static_cast<long>(det->x1),
                                               static_cast<long>(det->y1),
                                               static_cast<long>(det->x2),
                                               static_cast<long>(det->y2));
                    fwl.landmarks = m_detector->GetLandmarks(frame, fwl.rect);
                    faces.push_back(std::move(fwl));
                    faceJson = FacesToJson(faces);
                }
            }

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

void EnrollmentWizard::StopPreview() {
    m_previewRunning = false;
    m_frameRunning = false;
    m_capturing = false;

    // Join background threads before shutting down camera
    if (m_captureThread.joinable())
        m_captureThread.join();
    if (m_frameThread.joinable())
        m_frameThread.join();

    if (m_webcam)
        m_webcam->Shutdown();
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

    std::vector<BYTE> bgra(srcW * srcH * 4);
    for (int y = 0; y < srcH; y++) {
        BYTE* row = bgra.data() + y * srcW * 4;
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
    std::vector<BYTE> jpgData(jpgSize);
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
    const std::vector<facelogin::FaceWithLandmarks>& faces) {
    std::ostringstream js;
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
        js << "]}";
    }
    js << "]";
    return js.str();
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
                {
                    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
                    if (m_latestFrame.size() == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(33)); continue; }
                    frame = m_latestFrame;
                }
                // Detect with SCRFD, extract 68-point landmarks.
                dlib::full_object_detection asLandmarks;
                auto asDet = m_onnxDetector->DetectLargestFace(frame);
                if (asDet) {
                    dlib::rectangle asRect(static_cast<long>(asDet->x1),
                                           static_cast<long>(asDet->y1),
                                           static_cast<long>(asDet->x2),
                                           static_cast<long>(asDet->y2));
                    asLandmarks = m_detector->GetLandmarks(frame, asRect);
                }
                if (asLandmarks.num_parts() == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(33)); continue; }

                float score = m_antiSpoof->Predict(frame, asLandmarks);
                totalChecked++;
                if (score >= m_antiSpoofThreshold) passCount++; // config-driven threshold
                FACELOGIN_INFO(L"Enrollment anti-spoof frame %d: score=%.3f (pass=%d)",
                              totalChecked, score, passCount);
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            livenessPassed = (totalChecked > 0 && passCount >= passRequired);
        } else {
            // blink (default)
            LivenessDetector liveness;
            liveness.Configure(kDefaultEarThreshold, kDefaultBlinkFrames);
            auto livenessStart = std::chrono::steady_clock::now();
            bool blinked = false;
            while (m_capturing && !blinked) {
                auto elapsed = std::chrono::steady_clock::now() - livenessStart;
                if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 8) {
                    FACELOGIN_WARN(L"Enrollment liveness timeout");
                    break;
                }
                dlib::matrix<dlib::rgb_pixel> frame;
                {
                    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
                    if (m_latestFrame.size() == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(33)); continue; }
                    frame = m_latestFrame;
                }
                // Detect with SCRFD, extract 68-point landmarks for EAR.
                dlib::full_object_detection livenessLandmarks;
                auto livenessDet = m_onnxDetector->DetectLargestFace(frame);
                if (livenessDet) {
                    dlib::rectangle lRect(static_cast<long>(livenessDet->x1),
                                          static_cast<long>(livenessDet->y1),
                                          static_cast<long>(livenessDet->x2),
                                          static_cast<long>(livenessDet->y2));
                    livenessLandmarks = m_detector->GetLandmarks(frame, lRect);
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

        // Phase 2: Collect face samples
        int failCount = 0;
        for (int i = 0; i < TARGET_SAMPLES && m_capturing;) {
            // Read the latest frame from the frame-grab thread (no camera contention)
            dlib::matrix<dlib::rgb_pixel> frame;
            {
                std::lock_guard<std::mutex> lock(m_frameCacheMutex);
                if (m_latestFrame.size() == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(33));
                    continue;
                }
                frame = m_latestFrame;
            }

            // Detect with SCRFD (the only detector), extract 68-point landmarks.
            dlib::full_object_detection landmarks;
            auto onnxDet = m_onnxDetector->DetectLargestFace(frame);
            if (onnxDet) {
                dlib::rectangle rect(static_cast<long>(onnxDet->x1),
                                     static_cast<long>(onnxDet->y1),
                                     static_cast<long>(onnxDet->x2),
                                     static_cast<long>(onnxDet->y2));
                landmarks = m_detector->GetLandmarks(frame, rect);
            }
            if (landmarks.num_parts() == 0) {
                if (++failCount > 300) { m_capturing = false; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // Compute the embedding with InsightFace ONNX (the only recognizer).
            // Store the FULL 512-D embedding (no truncation).
            auto onnxEmb = m_onnxRecognizer->ComputeEmbedding(frame, landmarks);
            if (onnxEmb.empty()) {
                if (++failCount > 300) { m_capturing = false; break; }
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
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
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

std::string EnrollmentWizard::GetUserSid() const {
    return WstrToUtf8(m_sid);
}

std::string EnrollmentWizard::GetUserUpn() const {
    return WstrToUtf8(m_upn);
}

bool EnrollmentWizard::ValidatePassword(const std::wstring& password) {
    HANDLE hToken = nullptr;
    BOOL ok = LogonUserW(m_username.c_str(), L".", password.c_str(),
                         LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &hToken);
    if (ok && hToken) {
        CloseHandle(hToken);
        return true;
    }

    // If LogonUser fails and we have a UPN (MSA account), try with UPN
    if (!m_upn.empty() && m_upn.find(L'@') != std::wstring::npos) {
        ok = LogonUserW(m_upn.c_str(), L".", password.c_str(),
                         LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &hToken);
        if (ok && hToken) {
            CloseHandle(hToken);
            return true;
        }
        FACELOGIN_WARN(L"ValidatePassword: UPN logon also failed (err=%lu)",
                      GetLastError());
    }

    FACELOGIN_WARN(L"Password validation failed for user %s (UPN=%s)",
                  m_username.c_str(), m_upn.c_str());
    return false;
}

bool EnrollmentWizard::SaveEnrollment(const std::wstring& password) {
    if (m_embeddings.empty()) { FACELOGIN_ERROR(L"No face samples"); return false; }

    // Embedding consistency check: verify all samples are from the same person.
    // Compute average pairwise distance — if it exceeds the cap, reject.
    // Same-person distances are typically well below 0.80 (the ONNX boundary);
    // different people exceed it.
    //
    // The cap is calibrated via EmbeddingThresholdForDim: 512-D InsightFace
    // ONNX uses 0.80 (measured same-person boundary, see credential_store.h).
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
        // All samples share one dimensionality (enrollment uses one recognizer).
        size_t dim = m_embeddings.empty() ? 0 : static_cast<size_t>(m_embeddings[0].size());
        float maxAllowed = EmbeddingThresholdForDim(0.45f, dim);
        FACELOGIN_INFO(L"Enrollment consistency: avg pairwise dist=%.4f (max=%.3f, %d pairs, %zu-D)",
                      avgPairDist, maxAllowed, pairs, dim);
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
    }

    auto protectedPassword = DpapiUtil::Protect(
        reinterpret_cast<const uint8_t*>(password.c_str()),
        static_cast<UINT>(password.size() * sizeof(wchar_t)));

    if (protectedPassword.empty()) { FACELOGIN_ERROR(L"DPAPI encryption failed"); return false; }

    m_store.LoadDatabase();
    // Copy the average embedding into a plain float vector (full dimensionality —
    // 512-D for ONNX, 128-D for dlib). Never truncate.
    std::vector<float> ef(static_cast<size_t>(avgEmbedding.size()));
    for (long i = 0; i < avgEmbedding.size(); i++)
        ef[static_cast<size_t>(i)] = avgEmbedding(static_cast<long>(i));
    m_store.AddUser(m_username, m_upn, m_sid, protectedPassword, ef);
    if (!m_store.SaveDatabase()) { FACELOGIN_ERROR(L"Failed to save database"); return false; }

    FACELOGIN_INFO(L"Enrollment saved for: %s (emb=%zu-D)", m_username.c_str(), ef.size());

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

    FACELOGIN_INFO(L"Configuration updated: rec=%hs det=%hs live=%hs thr=%.2f",
                  m_config.recognition_model.c_str(), m_config.detector.c_str(),
                  LivenessMethodToString(m_config.liveness_method).c_str(),
                  m_config.match_threshold);

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

    // Cap at ~256KB of raw bytes
    DWORD capSize = fileSize;
    if (capSize > 256 * 1024) capSize = 256 * 1024;

    std::vector<wchar_t> wbuf(capSize / sizeof(wchar_t) + 1);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, wbuf.data(), capSize, &bytesRead, nullptr) || bytesRead < 2) {
        CloseHandle(hFile);
        return "[\"Failed to read service log\"]";
    }
    CloseHandle(hFile);

    size_t wlen = bytesRead / sizeof(wchar_t);

    // Parse lines: each log line ends with \r\n (wchar_t)
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    size_t pos = 0;
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
