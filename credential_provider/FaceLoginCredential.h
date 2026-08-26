#pragma once

#include <windows.h>
#include <credentialprovider.h>
#include <string>
#include <memory>
#include <vector>

#include "../common/secure_buffer.h"
#include "../common/locale_util.h"
#include "pipe_client.h"

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

    FaceLoginCredential();
    virtual ~FaceLoginCredential();

    // Called by FaceLoginProvider after creation
    void Initialize(FaceLoginProvider* pProvider);

    // Provider-level advise/unadvise (called by FaceLoginProvider::Advise/UnAdvise)
    void AdviseProvider(ICredentialProviderEvents* pEvents, UINT_PTR upAdviseContext);
    void UnadviseProvider();

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
    // State enum
    enum class State {
        Waiting,
        Authenticating,
        Ready,
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
    std::wstring LocalizeServiceMessage(const std::wstring& message) const;

    // Trigger re-enumeration of credentials (via CredentialsChanged)
    void TriggerReEnumeration();

    // Start the authentication pipeline (connect pipe + send AUTH_REQUEST).
    // Called from Advise() (cold boot) or input-detection thread (unlock).
    void StartAuth();

    // Start / stop the background input-detection thread (unlock scenario).
    void StartInputDetectionThread();
    void StopInputDetectionThread();

    // Pipe callbacks — called from background read thread
    void OnPipeResponse(bool success, const std::wstring& message);
    void OnPipeStatus(const std::wstring& message);

    LONG m_refCount = 1;
    FaceLoginProvider* m_pProvider = nullptr;
    ICredentialProviderCredentialEvents* m_pCredentialEvents = nullptr;
    ICredentialProviderEvents* m_pProviderEvents = nullptr;
    UINT_PTR m_upAdviseContext = 0;

    State m_state = State::Waiting;
    // Set when the service reported AUTH_NO_MATCH (face seen, no enrolled
    // face matched). The Failed-state status text then shows "人脸匹配失败"
    // instead of the generic timeout wording. Cleared at each StartAuth.
    bool m_noMatchFailed = false;
    std::unique_ptr<facelogin::PipeClient> m_pipeClient;

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

    // Auth timeout tracking (so we don't block LogonUI forever)
    LONGLONG m_authStartTime = 0;  // 100ns units, 0 = not yet started

    // On unlock: baseline tick recorded in Advise(). A background thread
    // polls GetLastInputInfo() and calls StartAuth() when NEW input arrives
    // (keyboard or mouse). The first keypress that dismissed the lock-screen
    // wallpaper happened BEFORE our DLL was loaded, so any tick <= baseline
    // is ignored.
    DWORD m_waitingStartTick = 0;
    HANDLE m_hInputThread = nullptr;   // background input-detection thread
    HANDLE m_hInputStop = nullptr;     // event: signal to stop the thread
    bool m_inputThreadRunning = false;

    // Synchronization
    HANDLE m_hCredsReady = nullptr;  // Set when auth result received
    CRITICAL_SECTION m_cs;
    bool m_csInitialized = false;
};
