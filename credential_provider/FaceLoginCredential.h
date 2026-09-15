#pragma once

#include <windows.h>
#include <credentialprovider.h>
#include <string>
#include <memory>
#include <vector>

#include "../common/secure_buffer.h"
#include "../common/locale_util.h"
#include "pipe_client.h"
#include "status_overlay.h"

// Forward declarations
class FaceLoginProvider;

// ============================================================================
// FaceLoginCredential — ICredentialProviderCredential implementation
//
// The actual credential tile shown on the lock screen. Handles:
//   1. Connecting to the face recognition service via named pipe
//   2. Waiting for face authentication result
//   3. Serializing credentials via CredPackAuthenticationBufferW
//   4. Auto-logon two-pass pattern
//
// State machine:
//   Waiting        — Initial state, trying to establish pipe connection
//   Authenticating — Pipe connected, waiting for face recognition result
//   Ready          — Credentials received, ready to serialize
//   Failed         — Auth timed out or error
// ============================================================================

class FaceLoginCredential : public ICredentialProviderCredential {
public:
    // Allow the input-detection thread to access private members
    friend unsigned __stdcall InputDetectionThreadProc(void* pParam);
    friend unsigned __stdcall AuthConnectThreadProc(void* pParam);

    FaceLoginCredential();
    virtual ~FaceLoginCredential();

    // Called by FaceLoginProvider after creation
    void Initialize(FaceLoginProvider* pProvider);

    // Provider-level advise/unadvise (called by FaceLoginProvider::Advise/UnAdvise)
    void AdviseProvider(ICredentialProviderEvents* pEvents, UINT_PTR upAdviseContext);
    void UnadviseProvider();
    bool IsAutoSubmitReady() const;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ICredentialProviderCredential
    STDMETHODIMP Advise(ICredentialProviderCredentialEvents* pcpce) override;
    STDMETHODIMP_(HRESULT) UnAdvise() override;
    STDMETHODIMP SetSelected(BOOL* pbAutoLogon) override;
    STDMETHODIMP SetDeselected() override;
    STDMETHODIMP GetFieldState(DWORD dwFieldID,
                               CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
                               CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis) override;
    STDMETHODIMP GetStringValue(DWORD dwFieldID, PWSTR* ppwsz) override;
    STDMETHODIMP GetBitmapValue(DWORD dwFieldID, HBITMAP* phbmp) override;
    STDMETHODIMP GetCheckboxValue(DWORD dwFieldID, BOOL* pbChecked, PWSTR* ppwszLabel) override;
    STDMETHODIMP GetSubmitButtonValue(DWORD dwFieldID, DWORD* pdwAdjacentTo) override;
    STDMETHODIMP GetComboBoxValueCount(DWORD dwFieldID, DWORD* pcItems, DWORD* pdwSelectedItem) override;
    STDMETHODIMP GetComboBoxValueAt(DWORD dwFieldID, DWORD dwItem, PWSTR* ppwszItem) override;
    STDMETHODIMP SetStringValue(DWORD dwFieldID, LPCWSTR pwz) override;
    STDMETHODIMP SetCheckboxValue(DWORD dwFieldID, BOOL bChecked) override;
    STDMETHODIMP SetComboBoxSelectedValue(DWORD dwFieldID, DWORD dwSelectedItem) override;
    STDMETHODIMP CommandLinkClicked(DWORD dwFieldID) override;
    STDMETHODIMP GetSerialization(
        CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
        CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
        PWSTR* ppwszOptionalStatusText,
        CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) override;
    STDMETHODIMP ReportResult(NTSTATUS ntsStatus, NTSTATUS ntsSubstatus,
                              PWSTR* ppwszOptionalStatusText,
                              CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) override;

private:
    using AuthAttemptId = unsigned long long;
    using InputActivationId = unsigned long long;

    enum class AuthTrigger {
        LoginEntryAutomatic,
        LoginEntryKeyPress,
        UnlockKeyPress
    };

    // State enum
    enum class State {
        Waiting,
        Authenticating,
        Ready,
        Submitted,  // credential packed & handed to LogonUI — no re-submit
        Failed,
        Error,
        Blocked  // Passwordless account: show notice, never submit creds
    };

    // Authentication package lookup
    HRESULT GetAuthenticationPackage(ULONG* pulAuthPackage);

    // Switch to the password credential provider (fallback)
    HRESULT SwitchToPasswordProvider();

    // Pack credentials into the serialization format
    HRESULT PackCredentials(CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs);

    std::wstring Text(const char* key, const wchar_t* fallback) const {
        return m_locale.GetWide(key, fallback);
    }
    // Translate a pipe payload: the service sends locale keys (ipc::L10N_*),
    // never display text. Lookup is active pack → zh-CN pack → empty; callers
    // fall back to their state's default text when the result is empty.
    std::wstring LocalizeKey(const std::wstring& key) const;

    // Trigger re-enumeration of credentials (via CredentialsChanged)
    void TriggerReEnumeration();

    State GetState() const;
    bool TransitionState(State expected, State next);
    bool IsAttemptActive(AuthAttemptId attemptId) const;
    void SetStatusText(const std::wstring& text);
    std::wstring VisibleStatusText() const;
    facelogin::StatusOverlayPresentation CurrentStatusPresentation() const;
    void PublishCurrentStatus();
    void UpdateStatusField(const std::wstring& text, bool visible);
    void NotifyCredentialsChanged();
    bool EnsureStatusOverlay(
        const facelogin::StatusOverlayPresentation& presentation,
        const wchar_t* reason);
    void ClearCredentials();
    void CancelActiveAttempt(bool resetToWaiting);
    AuthTrigger InputAuthTrigger() const;

    // Start the authentication pipeline without blocking the LogonUI or input
    // thread. The connection worker owns a COM reference until it exits.
    bool StartAuthAsync(AuthTrigger trigger,
                        InputActivationId expectedInputActivationId = 0);
    void JoinAuthConnectThread();
    bool IsInputActivationValid(InputActivationId activationId) const;

    // Start / stop the background input-detection thread (unlock scenario).
    void StartInputDetectionThread();
    void StopInputDetectionThread();

    // Pipe callbacks — called from background read thread
    void OnPipeResponse(AuthAttemptId attemptId, bool success, const std::wstring& message);
    void OnPipeStatus(AuthAttemptId attemptId, const std::wstring& message);

    LONG m_refCount = 1;
    FaceLoginProvider* m_pProvider = nullptr;
    // FaceLogin supports Windows 8 and later only. Retain Events2
    // exclusively: status updates stay inside the active tile instead of
    // falling back to the legacy re-enumeration-prone event API.
    ICredentialProviderCredentialEvents2* m_pCredentialEvents2 = nullptr;
    ICredentialProviderEvents* m_pProviderEvents = nullptr;
    UINT_PTR m_upAdviseContext = 0;
    facelogin::StatusOverlay m_statusOverlay;

    State m_state = State::Waiting;
    // Set when the service reported AUTH_NO_MATCH (face seen, no enrolled
    // face matched). The Failed-state status text then shows "人脸匹配失败"
    // instead of the generic timeout wording. Cleared at each StartAuth.
    bool m_noMatchFailed = false;
    std::shared_ptr<facelogin::PipeClient> m_pipeClient;
    AuthAttemptId m_nextAttemptId = 0;
    AuthAttemptId m_activeAttemptId = 0;
    AuthTrigger m_authTrigger = AuthTrigger::UnlockKeyPress;
    bool m_autoSubmitEligible = false;
    bool m_autoStartConsumed = false;
    bool m_statusOverlayAllowed = false;
    bool m_statusOverlayUnavailable = false;
    bool m_statusFieldFallbackVisible = false;
    facelogin::StatusOverlayTone m_statusOverlayTone =
        facelogin::StatusOverlayTone::Neutral;

    // Received credentials (zeroed after serialization)
    facelogin::SecureBuffer m_authData;
    std::wstring m_sid;
    std::wstring m_upn;
    std::wstring m_domain;
    std::wstring m_username;
    std::wstring m_password;

    // Live status text pushed from service (updated from background thread)
    std::wstring m_statusText;
    facelogin::LocaleCatalog m_locale;

    // Auth timeout tracking uses the monotonic tick count so a system clock
    // adjustment cannot extend or prematurely end an authentication attempt.
    ULONGLONG m_authDeadlineTick = 0;

    // On unlock: a background thread snapshots virtual-key states when it
    // starts, then reacts only to rising edges on keyboard keys or mouse
    // buttons. Mouse movement alone is intentionally ignored.
    HANDLE m_hInputThread = nullptr;   // background input-detection thread
    DWORD m_inputThreadId = 0;
    HANDLE m_hInputStop = nullptr;     // event: signal to stop the thread
    bool m_inputThreadRunning = false;
    InputActivationId m_inputActivationId = 0;
    bool m_inputDetectionEnabled = false;

    HANDLE m_hAuthThread = nullptr;
    HANDLE m_hAuthStop = nullptr;
    bool m_authThreadRunning = false;
    bool m_deselected = false;

    // Synchronization
    HANDLE m_hCredsReady = nullptr;  // Set when auth result received
    mutable CRITICAL_SECTION m_cs;
    bool m_csInitialized = false;
};
