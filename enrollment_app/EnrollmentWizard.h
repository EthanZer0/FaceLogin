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
    // Save enrollment with the given password. label names this face (may be
    // empty — the backend falls back to L"脸N").
    bool SaveEnrollment(const std::wstring& password, const std::wstring& label = L"");

    // Passwordless account support (MSA accounts with no password — PIN/Hello
    // only). Returns:
    //   0 = account has a password (or detection inconclusive but likely has one)
    //   1 = confirmed passwordless (auto-skip the password screen)
    //   2 = MSA, cannot auto-confirm — UI offers a checkbox for the user to confirm
    int GetPasswordlessState() const;
    // Save enrollment with no password (uses the current logged-on session
    // identity as the "self" proof). Stores a passwordless sentinel.
    // label names this face (empty → L"脸N").
    bool SaveEnrollmentNoPassword(const std::wstring& label = L"");

    // Multi-face management (1.3.0). The current account can enroll several
    // faces; each save appends one face instead of replacing the old one.
    // Number of faces enrolled for the current account (0 = not enrolled).
    int GetFaceCount();
    // JSON list of the current account's faces: [{"id":1,"label":"脸1"},...]
    std::string GetFacesJson();
    // Append a new face for the current account without re-entering a password
    // (identity proven by the session token SID). label may be empty.
    bool SaveEnrollmentAppend(const std::wstring& label = L"");
    // Delete one face of the current account (removes the account if it was
    // the last face). Returns false on unknown id / not enrolled.
    bool DeleteFace(int faceId);
    // Remove all faces of the current account (= remove the account).
    bool ClearAllFaces();
    // Rename one face of the current account.
    bool RenameFace(int faceId, const std::wstring& label);

    // Account-type change detection (symmetric MSA ↔ local conversions).
    // Windows keeps the same SID when a user converts their account between a
    // Microsoft account (MSA) and a local account, so the stored record is
    // matched by SID but may carry stale identity/password from the previous
    // account type:
    //   MSA→local:  record UPN still an MSA email, password is the old MSA one.
    //   local→MSA:  record UPN empty (local-era), password is the old local one.
    // Returns 0 = normal / nothing to refresh,
    //         1 = stale MSA→local record detected,
    //         2 = stale local→MSA record detected (current session is an MSA
    //             but the record UPN is empty or differs from the current email).
    int GetAccountTypeChanged();
    // JSON wrapper for JS: {"state":0} | {"state":1,"faces":N} |
    // {"state":2,"faces":N,"upn":"user@mail.com"}.
    std::string CheckAccountTypeChanged();
    // Validate the CURRENT password, then rewrite the stale record IN PLACE
    // (preserving all faces): state 1 clears the old MSA UPN (local account),
    // state 2 writes the current MSA email; both refresh username/SID and
    // re-encrypt the password via DPAPI. Refuses (returns false) unless
    // GetAccountTypeChanged()!=0 and the password validates.
    bool RefreshAccountIdentity(const std::wstring& password);

    // Light-weight dismiss path for the stale-account prompt. State 1 only:
    // clears the bogus MSA email from the current local account's record
    // WITHOUT validating or re-encrypting the password (no input needed).
    // Faces and the stored password are preserved; the lock-screen credential
    // then packs with domain\username instead of the misattributed email.
    // This is also the only self-heal for passwordless local accounts, whose
    // full refresh always fails because RefreshAccountIdentity requires a
    // non-empty password. Returns false unless the record is in state 1.
    bool ClearStaleAccountUpn();

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

    // About-card "star seen once" flag for the CURRENT version, persisted in
    // the registry (HKLM\SOFTWARE\FaceLogin\AboutSeenVersion). The star shows
    // again whenever the console version changes (AboutSeenVersion != current).
    bool GetAboutSeen();
    void SetAboutSeen(bool seen);

private:
    std::string EncodeJPEGBase64(const dlib::matrix<dlib::rgb_pixel>& frame);
    std::string FacesToJson(const std::vector<facelogin::FaceWithLandmarks>& faces);

    // Load the shape predictor + ONNX models if not already loaded (called from
    // the background frame thread, so a cold start never blocks the UI thread).
    bool EnsureModelsLoaded();

    bool SaveEnrollmentImpl(const std::wstring& password, bool passwordless,
                            const std::wstring& label);
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
    // Consecutive camera re-inits inside the frame thread (stalled SourceReader
    // after the credential provider took the camera). Bounded so a truly-dead
    // device doesn't cause an infinite re-init loop; reset on success or start.
    int m_frameReinitCount = 0;

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
