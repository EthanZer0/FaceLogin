#pragma once

#include <windows.h>
#include <wincodec.h>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <dlib/matrix.h>
#include <dlib/pixel.h>

#include "../face_service/face_detector.h"
#include "../face_service/liveness_detector.h"
#include "../face_service/liveness_types.h"
#include "../face_service/onnx_models.h"
#include "../face_service/webcam_capture.h"
#include "../face_service/credential_store.h"
#include "../common/config_util.h"

namespace facelogin {

// Backend controller for face enrollment.
// All UI is handled by WebView2 + HTML; this class provides data & actions
// to JavaScript via a COM host object.
class EnrollmentWizard {
public:
    EnrollmentWizard();
    ~EnrollmentWizard();

    // === Called from JS via host object ===

    bool StartPreview();
    void StopPreview();
    int  GetSampleCount() const { return m_samplesCollected; }
    std::string GetUsername() const;
    std::string GetUserSid() const;
    std::string GetUserUpn() const;
    std::string GetAccountType() const { return m_accountType; }
    bool CaptureFaceSamples();          // blocking: captures 10 samples
    bool IsLivenessPassed() const { return m_livenessPassed; }
    bool IsLivenessChecking() const { return m_livenessChecking; }
    bool ValidatePassword(const std::wstring& password);
    bool SaveEnrollment(const std::wstring& password);

    // Passwordless account support (MSA accounts with no password — PIN/Hello
    // only). Returns:
    //   0 = account has a password (or detection inconclusive but likely has one)
    //   1 = confirmed passwordless (auto-skip the password screen)
    //   2 = MSA, cannot auto-confirm — UI offers a checkbox for the user to confirm
    int GetPasswordlessState() const;
    // Save enrollment with no password (uses the current logged-on session
    // identity as the "self" proof). Stores a passwordless sentinel.
    bool SaveEnrollmentNoPassword();

    // Configuration
    std::string GetConfig() const;
    bool SetConfig(const std::string& json);
    bool RestartPreview();

    // Camera device enumeration for the settings UI.
    // Returns JSON: [{path, name}, ...] — path is the stable symbolic link,
    // name is the friendly display name.
    std::string GetCameraList();

    // Log viewer
    std::string GetLogLines();
    std::string GetServiceLogLines();
    void ClearLog();

    // Per-frame data for JS canvas rendering (pull model — JS calls these from rAF)
    std::string GetLatestFrameBase64(); // JPEG base64, ~200KB
    std::string GetLatestFacesJson();   // [{x,y,w,h,landmarks:[{x,y},...]},...]
    // Atomically returns "<frame base64>\x1E<faces json>" from the SAME frame.
    std::string GetLatestFrameAndFaces();

    bool IsPreviewRunning() const { return m_previewRunning; }
    std::wstring GetDataDir() const { return m_dataDir; }

private:
    std::string EncodeJPEGBase64(const dlib::matrix<dlib::rgb_pixel>& frame);
    std::string FacesToJson(const std::vector<facelogin::FaceWithLandmarks>& faces);

    bool SaveEnrollmentImpl(const std::wstring& password, bool passwordless);
    static std::wstring GetCurrentProcessUserSid();

    // Camera & face processing
    std::unique_ptr<WebcamCapture>  m_webcam;
    std::unique_ptr<FaceDetector>   m_detector;       // 68-point shape predictor
    std::unique_ptr<OnnxDetector>   m_onnxDetector;   // SCRFD detection
    std::unique_ptr<OnnxRecognizer> m_onnxRecognizer; // InsightFace recognition
    std::unique_ptr<OnnxAntiSpoof>  m_antiSpoof;
    CredentialStore m_store;

    // Configuration
    AppConfig m_config;
    LivenessMethod m_livenessMethod = LivenessMethod::Blink;
    float m_antiSpoofThreshold = 0.30f;

    // Frame-grab thread (runs off UI thread — GrabFrame + JPEG encode + detection)
    std::thread m_frameThread;
    bool m_frameRunning = false;

    // WIC factory (created once)
    IWICImagingFactory* m_wicFactory = nullptr;

    // Per-frame caches (produced by frame thread, consumed by JS on UI thread)
    std::mutex  m_frameCacheMutex;
    std::string m_latestFrameB64;
    std::string m_latestFacesJson;
    dlib::matrix<dlib::rgb_pixel> m_latestFrame;   // for capture to read

    // Preview state
    bool m_previewRunning = false;

    // Enrollment state
    std::vector<dlib::matrix<float, 0, 1>> m_embeddings;
    int m_samplesCollected = 0;
    bool m_capturing = false;
    bool m_livenessPassed = false;
    bool m_livenessChecking = false;
    std::thread m_captureThread;
    static constexpr int TARGET_SAMPLES = 10;

    std::wstring m_username;
    std::wstring m_upn;       // UserPrincipalName (e.g. "john@outlook.com") or empty for local
    std::wstring m_sid;       // Security Identifier string
    std::string m_accountType; // "local" or "msa"
    std::wstring m_dataDir;
};

} // namespace facelogin
