#include "FaceLoginCredential.h"
#include "FaceLoginProvider.h"
#include "resource.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include <wincred.h>
#include <ntstatus.h>
#include <ntsecapi.h>
#include <sddl.h>
#include <shlwapi.h>
#include <vector>
#include <process.h>

#pragma comment(lib, "credui.lib")
#pragma comment(lib, "ntdll.lib")

// ============================================================================
// Input-detection thread (unlock scenario)
// ============================================================================
//
// Runs as a background thread, polling GetLastInputInfo() every ~200 ms.
// When it detects that the user has pressed a key or moved the mouse AFTER
// the baseline tick (recorded in Advise()), it calls StartAuth() which
// connects the pipe asynchronously.  Once auth completes, the pipe callback
// stores credentials and triggers CredentialsChanged(), causing LogonUI to
// re-enumerate and call GetSerialization(), which then packs and returns
// the ready credentials.
//
// The thread stops when:
//   - New input is detected and StartAuth() is called, OR
//   - The stop event is signaled (UnAdvise / destructor / 30s timeout), OR
//   - UnAdvise() sets m_pCredentialEvents = nullptr and the thread notices

struct InputDetectionContext {
    FaceLoginCredential* pCred;
};

static unsigned __stdcall InputDetectionThreadProc(void* pParam) {
    auto* ctx = static_cast<InputDetectionContext*>(pParam);
    FaceLoginCredential* pCred = ctx->pCred;
    delete ctx;

    FACELOGIN_INFO(L"[InputThread] Started — polling for user input every 200ms");

    const DWORD pollIntervalMs = 200;
    const DWORD timeoutSec = 30;
    DWORD startTick = GetTickCount();

    // Loop forever (until the stop event is signaled).  The 30s timeout does
    // NOT kill the thread — it only restarts the idle window so a user who
    // waits longer than 30s before pressing a key can still trigger auth.
    while (true) {
        // Check stop signal (non-blocking)
        DWORD waitResult = WaitForSingleObject(pCred->m_hInputStop, 0);
        if (waitResult == WAIT_OBJECT_0) {
            FACELOGIN_INFO(L"[InputThread] Stop event signaled — exiting");
            break;
        }

        // Idle-window timeout: restart the window instead of exiting, so a
        // late keypress still works. (Previously the thread exited after 30s
        // of no input, leaving no path to restart it — a later keypress did
        // nothing.)
        DWORD elapsedMs = GetTickCount() - startTick;
        if (elapsedMs > timeoutSec * 1000) {
            FACELOGIN_INFO(L"[InputThread] 30s idle — restarting idle window");
            startTick = GetTickCount();
            continue;
        }

        // Poll GetLastInputInfo
        LASTINPUTINFO lii = {};
        lii.cbSize = sizeof(lii);
        if (GetLastInputInfo(&lii)) {
            // 300ms threshold: the first keypress to dismiss the lock-screen
            // wallpaper generates both KEYDOWN and KEYUP events, but the
            // KEYDOWN itself is the user's intent to unlock — trigger on the
            // first keystroke instead of requiring a second one. The small
            // guard only skips stray input recorded right around the
            // baseline (Advise) so we don't fire on noise. 500→300ms shrinks
            // the "dead zone" where a first keypress is silently swallowed
            // after the lock screen appears, so the user's first press more
            // often starts recognition immediately.
            DWORD threshold = pCred->m_waitingStartTick + 300;
            if (lii.dwTime > threshold) {
                FACELOGIN_INFO(L"[InputThread] NEW input detected! (last=%lu > threshold=%lu, diff=%ld)",
                              lii.dwTime, threshold,
                              static_cast<LONG>(lii.dwTime - pCred->m_waitingStartTick));
                pCred->StartAuth();
                break;
            }
        }

        // Sleep (alertable so the stop event can wake us)
        SleepEx(pollIntervalMs, TRUE);
    }

    FACELOGIN_INFO(L"[InputThread] Exiting");
    pCred->m_inputThreadRunning = false;
    return 0;
}

// ============================================================================
// Construction / Destruction
// ============================================================================

FaceLoginCredential::FaceLoginCredential() {
    InitializeCriticalSection(&m_cs);
    m_csInitialized = true;

    m_hCredsReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_hCredsReady) {
        FACELOGIN_ERROR(L"Failed to create credentials ready event");
    }

    const std::wstring installDir = ReadRegString(REGVAL_INSTALL_PATH, L"");
    const std::string uiLang = facelogin::LoadConfig(installDir).ui_language;
    const bool localeOk = m_locale.Load(installDir, uiLang);
    FACELOGIN_INFO(L"[l10n] Credential: installDir='%ls' ui_language='%hs' locale='%hs' loadOk=%d",
                   installDir.c_str(), uiLang.c_str(), m_locale.locale().c_str(), localeOk);

    FACELOGIN_DEBUG(L"FaceLoginCredential created");
}

FaceLoginCredential::~FaceLoginCredential() {
    // SENSITIVE: Zero the password from memory
    SecureZeroMemory(m_password.data(), m_password.size() * sizeof(wchar_t));

    if (m_hCredsReady) {
        CloseHandle(m_hCredsReady);
        m_hCredsReady = nullptr;
    }

    if (m_csInitialized) {
        DeleteCriticalSection(&m_cs);
        m_csInitialized = false;
    }

    FACELOGIN_DEBUG(L"FaceLoginCredential destroyed");
}

void FaceLoginCredential::Initialize(FaceLoginProvider* pProvider) {
    m_pProvider = pProvider;
    FACELOGIN_DEBUG(L"FaceLoginCredential initialized with provider");
}

void FaceLoginCredential::AdviseProvider(ICredentialProviderEvents* pEvents, UINT_PTR upAdviseContext) {
    m_pProviderEvents = pEvents;
    m_upAdviseContext = upAdviseContext;
}

void FaceLoginCredential::UnadviseProvider() {
    m_pProviderEvents = nullptr;
    m_upAdviseContext = 0;
}

// ============================================================================
// IUnknown
// ============================================================================

STDMETHODIMP FaceLoginCredential::QueryInterface(REFIID riid, void** ppv) {
    *ppv = nullptr;

    if (riid == IID_IUnknown ||
        riid == IID_ICredentialProviderCredential) {
        *ppv = static_cast<ICredentialProviderCredential*>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) FaceLoginCredential::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) FaceLoginCredential::Release() {
    LONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// ICredentialProviderCredential — Advise/UnAdvise
// ============================================================================

STDMETHODIMP FaceLoginCredential::Advise(ICredentialProviderCredentialEvents* pcpce) {
    FACELOGIN_INFO(L"=== Advise ENTER (state=%d, pcpce=%p) ===", static_cast<int>(m_state), pcpce);

    if (m_pCredentialEvents) {
        m_pCredentialEvents->Release();
    }
    m_pCredentialEvents = pcpce;
    if (m_pCredentialEvents) {
        m_pCredentialEvents->AddRef();
    }

    // Guard: if we already have credentials ready from a previous
    // auth round, don't restart the flow.  This prevents an infinite
    // loop where OnPipeResponse → CredentialsChanged → Advise()
    // overwrites Ready back to Authenticating.
    // NOTE: m_password may legitimately be EMPTY here (passwordless
    // record — blank-credential unlock). The check is on the Ready
    // state only.
    if (m_state == State::Ready) {
        FACELOGIN_INFO(L"Advise: credentials already ready, skipping auth restart");
        return S_OK;
    }

    // Submitted: the credential is with LogonUI/LSA now — never restart auth
    // while the outcome is pending (would re-recognize and re-submit).
    if (m_state == State::Submitted) {
        FACELOGIN_INFO(L"Advise: credential submitted, skipping auth restart");
        return S_OK;
    }

    // Blocked (passwordless notice shown): keep showing the notice — do not
    // auto-restart authentication on re-enumeration (would loop forever).
    if (m_state == State::Blocked) {
        FACELOGIN_INFO(L"Advise: blocked (passwordless notice), skipping auth restart");
        return S_OK;
    }

    // Guard: if we're already authenticating and have a live pipe,
    // don't create a second connection.
    if (m_state == State::Authenticating && m_pipeClient && m_pipeClient->IsConnected()) {
        FACELOGIN_INFO(L"Advise: already authenticating, skipping auth restart");
        return S_OK;
    }

    // Failed (no match / timeout): do NOT auto-restart authentication on
    // re-enumeration. OnPipeResponse → TriggerReEnumeration → Advise would
    // otherwise loop forever on the lock screen: the camera keeps turning
    // on, and every re-enumeration resets the password field the user is
    // typing into (password box keeps getting cleared/selected). Instead,
    // restart the input-detection thread so the NEXT key press retries —
    // that is the user's expected retry path (clicking the tile also
    // retries via SetSelected).
    if (m_state == State::Failed || m_state == State::Error) {
        // Failed (no match / timeout / submission rejected) and Error
        // (service-side rejection: anti-spoof attack, blink liveness failed,
        // service unavailable) both keep their specific status text — the
        // unconditional Waiting reset below would otherwise show "按下任意键"
        // and drop the real reason. Never auto-restart auth; restart the
        // input-detection thread so the NEXT key press retries (or the user
        // clicks the tile — SetSelected handles that).
        FACELOGIN_INFO(L"Advise: %s state — restarting input detection (key press retries)",
                       m_state == State::Failed ? L"failed" : L"error");
        m_waitingStartTick = GetTickCount();
        StartInputDetectionThread();
        return S_OK;
    }

    // Camera/auth activation is deferred entirely to SetSelected() — the
    // tile only starts recognition (or the input-detection thread) when
    // LogonUI actually SELECTS our tile.
    //
    // Why: Advise is invoked for EVERY credential enumeration, including
    // flows that never intend to use our tile:
    //   - MSA PIN-reset wizard reuses the LogonUI CP enumeration: any key
    //     press / click inside the wizard was treated as "new input" by the
    //     input-detection thread → StartAuth → the camera kept turning on,
    //     the wizard got interrupted and PIN setup failed ("PIN 不可用").
    //   - Typing on the password/PIN tile: SetDeselected stops the thread,
    //     but there was a race window after LogonUI switched tiles where
    //     the camera could still fire once.
    // SetSelected is only called for OUR tile, so deferring activation is
    // exact — no guessing about which UI flow loaded us.
    bool coldBoot = m_pProvider ? m_pProvider->IsColdBoot() : true;
    bool credUI = m_pProvider ? m_pProvider->IsCredUI() : false;
    FACELOGIN_INFO(L"Advise: coldBoot=%d, credUI=%d, m_pProvider=%p",
                  coldBoot, credUI, m_pProvider);

    m_state = State::Waiting;
    m_waitingStartTick = GetTickCount();
    FACELOGIN_INFO(L"Advise: baseline tick = %lu", m_waitingStartTick);

    if (coldBoot) {
        // Cold boot: start auth HERE, not in SetSelected. With autoLogon=TRUE
        // (GetCredentialCount), LogonUI polls GetSerialization directly and
        // does NOT call SetSelected — deferring cold-boot activation to
        // SetSelected left the login screen stuck at "press any key" with no
        // input-detection thread running (1.8.0 regression, user report).
        // StartAuth is idempotent (skips if already connected) and the
        // input-detection thread guards on m_inputThreadRunning, so a
        // concurrent SetSelected is harmless.
        if (ReadRegDword(REGVAL_COLD_BOOT_KEY_TRIGGER, 0) != 0) {
            FACELOGIN_INFO(L"Advise: cold boot + key-trigger enabled — waiting for key press");
            StartInputDetectionThread();
            if (m_pCredentialEvents) {
                const std::wstring waiting = Text("credential.pressAnyKey", L"按下任意按键以开始人脸识别");
                m_pCredentialEvents->SetFieldString(this, 1, waiting.c_str());
            }
        } else {
            FACELOGIN_INFO(L"Advise: cold boot — starting auth immediately");
            StartAuth();
        }
    } else {
        // Non-cold-boot (unlock / switch user / PIN-reset wizard): keep
        // activation deferred to SetSelected. LogonUI calls SetSelected for
        // the default-selected tile here, while flows that never select our
        // tile (MSA PIN-reset wizard, typing on the PIN/password tile) never
        // start the camera — that was the PIN-unavailable fix.
        if (!facelogin::PipeClient::ProbeServiceAvailable()) {
            FACELOGIN_WARN(L"Advise: service pipe missing — showing service-not-running notice");
            m_statusText = Text("credential.serviceNotRunning", L"人脸识别服务未运行，请检查 FaceLoginService");
            m_state = State::Error;
            if (m_pCredentialEvents) {
                m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
            }
            return S_OK;
        }
    }

    FACELOGIN_INFO(L"=== Advise EXIT (state=%d) ===", static_cast<int>(m_state));
    return S_OK;
}

STDMETHODIMP FaceLoginCredential::UnAdvise() {
    FACELOGIN_INFO(L"=== UnAdvise ENTER ===");

    // Stop the input-detection thread if running
    StopInputDetectionThread();

    if (m_pCredentialEvents) {
        m_pCredentialEvents->Release();
        m_pCredentialEvents = nullptr;
    }

    m_pipeClient.reset();
    return S_OK;
}

// ============================================================================
// ICredentialProviderCredential — SetSelected/SetDeselected
// ============================================================================

STDMETHODIMP FaceLoginCredential::SetSelected(BOOL* pbAutoLogon) {
    FACELOGIN_INFO(L"=== SetSelected ENTER (state=%d, *pbAutoLogon=%d) ===",
                  static_cast<int>(m_state),
                  pbAutoLogon ? static_cast<int>(*pbAutoLogon) : -1);

    bool coldBoot = m_pProvider ? m_pProvider->IsColdBoot() : true;
    bool credUI = m_pProvider ? m_pProvider->IsCredUI() : false;

    // Activation model (user-specified):
    //   - ONLY the cold-boot entry triggers auth automatically — and that is
    //     done in Advise() (autoLogon=TRUE means LogonUI never calls
    //     SetSelected for it). The "开机启动需按键触发" setting only governs
    //     that FIRST automatic trigger.
    //   - Every later activation — re-selecting the tile after switching away,
    //     or retrying after a failure/timeout — must wait for a key press.
    //     So SetSelected NEVER starts auth directly; it only (re)starts the
    //     input-detection thread. This matches the lock-screen behavior the
    //     user already relies on (switch back → press any key).
    if (m_state == State::Failed || m_state == State::Error) {
        // Failed (no match / timeout / submission rejected) / Error (anti-spoof,
        // blink liveness, service unavailable) + tile re-selected: start waiting
        // for a key press to retry. Never auto-restart — the failure text stays
        // visible and the next key press is the explicit retry.
        FACELOGIN_INFO(L"SetSelected: %s state — restarting input detection (key press retries)",
                       m_state == State::Failed ? L"failed" : L"error");
        m_waitingStartTick = GetTickCount();
        StartInputDetectionThread();
    } else if (m_state == State::Waiting && !m_inputThreadRunning) {
        // Waiting + (re)selected — either the initial selection after a cold
        // boot that did NOT auto-start (key-trigger on), or the user switched
        // back to the face tile after SetDeselected stopped everything.
        // Either way: start the input-detection thread and require a key press.
        FACELOGIN_INFO(L"SetSelected: tile selected — starting input detection (key press starts auth)");
        m_waitingStartTick = GetTickCount();
        StartInputDetectionThread();
        // Repush the Waiting text (clears any residual "识别中..." / stale text)
        m_statusText.clear();
        if (m_pCredentialEvents) {
            const std::wstring waiting = Text("credential.pressAnyKey", L"按下任意按键以开始人脸识别");
            m_pCredentialEvents->SetFieldString(this, 1, waiting.c_str());
        }
    }

    if (coldBoot) {
        // Cold boot: always poll GetSerialization for credentials.
        // (Advise already started auth; if the tile is re-selected while
        // Waiting, GetSerialization returns S_OK until a key press starts it.)
        *pbAutoLogon = TRUE;
    } else if (m_state == State::Ready) {
        // Unlock / CredUI + credentials ready (bg thread finished auth):
        // enable auto-logon so LogonUI calls GetSerialization to pack creds.
        *pbAutoLogon = TRUE;
    } else {
        // Unlock / CredUI + still Waiting: no auto-logon; we wait for the bg thread.
        *pbAutoLogon = FALSE;
    }

    FACELOGIN_INFO(L"=== SetSelected EXIT (*pbAutoLogon=%d, coldBoot=%d, credUI=%d, state=%d) ===",
                  *pbAutoLogon, static_cast<int>(coldBoot), static_cast<int>(credUI),
                  static_cast<int>(m_state));
    return S_OK;
}

STDMETHODIMP FaceLoginCredential::SetDeselected() {
    FACELOGIN_INFO(L"=== SetDeselected called (state=%d) ===", static_cast<int>(m_state));

    // The user moved to ANOTHER tile (e.g. the password tile) — stop
    // everything face-related so recognition can neither fire off a password
    // keystroke nor keep the camera/pipe busy while the user types elsewhere:
    //   1. Stop the input-detection thread (a key press while typing the
    //      password must NOT start recognition).
    //   2. Cancel any in-flight auth: disconnecting the pipe makes the service
    //      notice the client went away and abort + release the camera.
    //   3. Reset to Waiting ONLY when an auth was actually cancelled, so a
    //      later re-selection (SetSelected) restarts the flow cleanly.
    //      A FAILED state is left untouched: the failure text stays visible
    //      and re-selecting the tile (SetSelected) is the explicit retry.
    StopInputDetectionThread();
    if (m_state == State::Authenticating && m_pipeClient) {
        FACELOGIN_INFO(L"SetDeselected: cancelling in-flight auth (user switched tiles)");
        m_pipeClient->Disconnect();
        m_pipeClient.reset();
        m_state = State::Waiting;
    }
    return S_OK;
}

// ============================================================================
// ICredentialProviderCredential — Field State / Values
// ============================================================================

STDMETHODIMP FaceLoginCredential::GetFieldState(
    DWORD dwFieldID,
    CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
    CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis) {

    FACELOGIN_INFO(L"=== GetFieldState (field=%lu, state=%d) ===",
                  dwFieldID, static_cast<int>(m_state));

    *pcpfs = CPFS_DISPLAY_IN_SELECTED_TILE;
    *pcpfis = CPFIS_NONE;

    switch (dwFieldID) {
    case 0: // "Face Login" label
        *pcpfs = CPFS_DISPLAY_IN_BOTH;
        break;

    case 1: // Status text — always visible so error states are seen
        *pcpfs = CPFS_DISPLAY_IN_BOTH;
        break;

    case 2: // Submit button — hidden in both scenarios
        *pcpfs = CPFS_HIDDEN;
        break;

    case 3: // Command link — visible when not selected
        *pcpfs = CPFS_DISPLAY_IN_DESELECTED_TILE;
        break;

    default:
        return E_INVALIDARG;
    }

    return S_OK;
}

STDMETHODIMP FaceLoginCredential::GetStringValue(DWORD dwFieldID, PWSTR* ppwsz) {
    FACELOGIN_INFO(L"=== GetStringValue (field=%lu, state=%d) ===",
                  dwFieldID, static_cast<int>(m_state));
    *ppwsz = nullptr;

    switch (dwFieldID) {
    case 0: // Label
        return SHStrDupW(Text("credential.title", L"人脸登录").c_str(), ppwsz);

    case 1: // Status
        switch (m_state) {
        case State::Waiting:
            return SHStrDupW(Text("credential.pressAnyKey", L"按下任意按键以开始人脸识别").c_str(), ppwsz);
        case State::Authenticating:
            if (!m_statusText.empty()) {
                return SHStrDupW(m_statusText.c_str(), ppwsz);
            }
            return SHStrDupW(Text("credential.recognizing", L"识别中...").c_str(), ppwsz);
        case State::Ready:
            return SHStrDupW(Text("credential.success", L"人脸识别成功，正在解锁...").c_str(), ppwsz);
        case State::Submitted:
            // Credential handed to LogonUI — outcome is decided by LSA. Avoid
            // the misleading "成功" text on the rejection error page.
            return SHStrDupW(L"正在验证登录，等待 Windows 确认...", ppwsz);        case State::Failed:
            // A specific failure text (e.g. submission rejected by LSA —
            // ReportResult) takes precedence; AUTH_NO_MATCH carries its own
            // wording ("人脸匹配失败..."); plain timeouts keep the generic.
            if (!m_statusText.empty()) {
                return SHStrDupW(m_statusText.c_str(), ppwsz);
            }
            if (m_noMatchFailed) {
                return SHStrDupW(m_statusText.empty() ?
                                 Text("credential.noMatch", L"人脸匹配失败，请重试或使用密码登录").c_str() :
                                 m_statusText.c_str(), ppwsz);

            }
            return SHStrDupW(Text("credential.noFace", L"未识别到人脸，请重试或使用密码登录").c_str(), ppwsz);
        case State::Blocked:
            // Passwordless account notice (set by OnPipeResponse / polling).
            return SHStrDupW(m_statusText.empty() ?
                             Text("credential.passwordless", L"该账号无密码，人脸识别无法用于解锁，请使用 PIN/Hello 登录").c_str() :
                             m_statusText.c_str(), ppwsz);
        case State::Error:
            // Show the specific error message from the service (e.g. anti-spoof
            // rejection) if one was received; otherwise the generic fallback.
            if (!m_statusText.empty()) {
                return SHStrDupW(m_statusText.c_str(), ppwsz);
            }
            return SHStrDupW(Text("credential.serviceUnavailable", L"人脸登录服务不可用").c_str(), ppwsz);
        default:
            return SHStrDupW(L"", ppwsz);
        }

    case 2: // Submit button
        return SHStrDupW(L"", ppwsz);

    case 3: // Command link
        return SHStrDupW(Text("credential.switchToPassword", L"切换到密码登录").c_str(), ppwsz);

    default:
        return E_INVALIDARG;
    }
}

STDMETHODIMP FaceLoginCredential::GetBitmapValue(DWORD dwFieldID, HBITMAP* phbmp) {
    UNREFERENCED_PARAMETER(dwFieldID);
    *phbmp = nullptr;
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::GetSubmitButtonValue(DWORD dwFieldID, DWORD* pdwAdjacentTo) {
    if (dwFieldID == 2) {
        *pdwAdjacentTo = 1; // Next to the status text field
        return S_OK;
    }
    return E_INVALIDARG;
}

STDMETHODIMP FaceLoginCredential::GetCheckboxValue(DWORD dwFieldID, BOOL* pbChecked, PWSTR* ppwszLabel) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(pbChecked);
    UNREFERENCED_PARAMETER(ppwszLabel);
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::GetComboBoxValueCount(DWORD dwFieldID, DWORD* pcItems, DWORD* pdwSelectedItem) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(pcItems);
    UNREFERENCED_PARAMETER(pdwSelectedItem);
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::GetComboBoxValueAt(DWORD dwFieldID, DWORD dwItem, PWSTR* ppwszItem) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(dwItem);
    UNREFERENCED_PARAMETER(ppwszItem);
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::SetStringValue(DWORD dwFieldID, LPCWSTR pwz) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(pwz);
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::SetCheckboxValue(DWORD dwFieldID, BOOL bChecked) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(bChecked);
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::SetComboBoxSelectedValue(DWORD dwFieldID, DWORD dwSelectedItem) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(dwSelectedItem);
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::CommandLinkClicked(DWORD dwFieldID) {
    if (dwFieldID == 3) {
        FACELOGIN_INFO(L"User clicked 'Switch to password login'");
        SwitchToPasswordProvider();
        return S_OK;
    }
    return E_INVALIDARG;
}

// ============================================================================
// ICredentialProviderCredential — GetSerialization (THE KEY METHOD)
// ============================================================================

STDMETHODIMP FaceLoginCredential::GetSerialization(
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
    PWSTR* ppwszOptionalStatusText,
    CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) {

    FACELOGIN_INFO(L"=== GetSerialization ENTER (state=%d) ===", static_cast<int>(m_state));

    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;

    ZeroMemory(pcpcs, sizeof(*pcpcs));

    // Submitted: the credential was already handed over ("finished") and no
    // re-submission is allowed — LSA already judged it (LogonUI error page
    // after a blank-password rejection, or success). Ending without a new
    // credential lets LogonUI settle on its own error/success UI instead of
    // starting another auth round.
    if (m_state == State::Submitted) {
        FACELOGIN_INFO(L"GetSerialization: credential already submitted — ending without re-submit");
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // Unlock scenario: if we're in Waiting state, the background input-
    // detection thread is still waiting for user input. Return "not
    // finished" — no credentials yet.
    if (m_state == State::Waiting) {
        FACELOGIN_INFO(L"GetSerialization: still Waiting for user input");
        return S_OK;
    }

    // Blocked (passwordless account): show the notice, finish without
    // submitting credentials so the tile never hangs.
    if (m_state == State::Blocked) {
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // If we're in Error state, service is not available — don't block login
    if (m_state == State::Error) {
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // Auth failed earlier — don't retry, let user use password
    if (m_state == State::Failed) {
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // Check for pipe disconnection — the server may have disconnected
    // without sending a response (e.g., service crashed or timed out).
    // Without this check, the CP stays in Authenticating forever.
    if (m_pipeClient && !m_pipeClient->IsConnected()) {
        FACELOGIN_WARN(L"Pipe disconnected while waiting for auth response");
        m_state = State::Error;
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // Track auth timeout: if we've been authenticating too long, give up
    if (m_state == State::Authenticating) {
        LONGLONG now = 0;
        GetSystemTimeAsFileTime(reinterpret_cast<FILETIME*>(&now));
        if (m_authStartTime == 0) {
            m_authStartTime = now;
        } else {
            // 20-second hard timeout for auth (15s service timeout + 5s grace)
            const LONGLONG AUTH_TIMEOUT_100NS = 200000000LL;
            if (now - m_authStartTime > AUTH_TIMEOUT_100NS) {
                FACELOGIN_WARN(L"Auth timed out waiting for service response");
                m_state = State::Failed;
                *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
                return S_OK;
            }
        }
    }

    // Check if pipe has a response
    if (m_pipeClient) {
        std::wstring response;
        if (m_pipeClient->CheckResponse(response)) {
            // Parse the response
            auto result = facelogin::ipc::ParseAuthMessage(response);

            if (result.status == facelogin::ipc::AuthResult::Status::Success) {
                FACELOGIN_INFO(L"Auth success: %s\\%s (SID=%s, UPN=%s)",
                              result.domain.c_str(), result.username.c_str(),
                              result.sid.c_str(), result.upn.c_str());
                m_sid = result.sid;
                m_upn = result.upn;
                m_domain = result.domain;
                m_username = result.username;
                m_password = result.password;
                m_state = State::Ready;
                SetEvent(m_hCredsReady);
            }
            else if (result.status == facelogin::ipc::AuthResult::Status::Timeout) {
                FACELOGIN_INFO(L"Auth timeout");
                m_state = State::Failed;
                *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
                return S_OK;
            }
            else if (result.status == facelogin::ipc::AuthResult::Status::Error) {
                FACELOGIN_WARN(L"Auth error: %s", result.errorMessage.c_str());
                // Passwordless account: show the notice in-place and finish.
                if (result.errorMessage == facelogin::ipc::MSG_PASSWORDLESS_NOTICE) {
                    m_statusText = LocalizeKey(result.errorMessage);
                    m_state = State::Blocked;
                    if (m_pCredentialEvents) {
                        m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
                    }
                    *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
                    return S_OK;
                }
                // Surface the service's specific error on the lock screen.
                if (!result.errorMessage.empty()) {
                    m_statusText = LocalizeKey(result.errorMessage);
                }
                m_state = State::Error;
                *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
                return S_OK;
            }
        }
    }

    // If we have credentials ready, pack and return them. A still-empty
    // password is VALID here: it means the record is passwordless (blank
    // password account) and we pack a blank MSV1_0 credential — Windows
    // allows blank-password console logon by default, so face unlock works.
    if (m_state == State::Ready) {
        // NOTE: never log the password or any part of it — it is a credential.
        HRESULT hr = PackCredentials(pcpcs);
        if (SUCCEEDED(hr)) {
            *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
            // Move to Submitted: the credential was handed to LogonUI. If LSA
            // rejects it (wrong password / blank not allowed), the error page
            // and any re-poll must NOT pack the same credential again — the
            // user returns to the tile after the error page and would see
            // "人脸识别成功" and a resubmission loop otherwise. ReportResult
            // drives the real outcome (success → done; failure → Failed).
            m_state = State::Submitted;
            // Push the status text NOW so the stale "人脸识别成功，正在解锁..."
            // (pulled by LogonUI while Ready) is replaced before the LSA
            // rejection error page hides the shell — LogonUI does not re-pull
            // the string once the error page is up.
            if (m_pCredentialEvents) {
                m_pCredentialEvents->SetFieldString(
                    this, 1, L"\u6b63\u5728\u9a8c\u8bc1\u767b\u5f55\uff0c\u7b49\u5f85 Windows \u786e\u8ba4...");
            }
            FACELOGIN_INFO(L"PackCred SUCCESS: cbSerialization=%lu, ulAuthPackage=%lu",
                          pcpcs->cbSerialization, pcpcs->ulAuthenticationPackage);
        } else {
            FACELOGIN_ERROR(L"PackCred FAILED: hr=0x%08X", hr);
        }
        return hr;
    }

    // Not ready yet — LogonUI will call GetSerialization() again
    // due to auto-logon being set. Our auth timeout guard above
    // ensures we eventually give up and don't block forever.
    FACELOGIN_INFO(L"=== GetSerialization EXIT: not ready (state=%d, response=%d) ===",
                  static_cast<int>(m_state), static_cast<int>(*pcpgsr));
    return S_OK;
}

// ============================================================================
// ICredentialProviderCredential — ReportResult
// ============================================================================

STDMETHODIMP FaceLoginCredential::ReportResult(
    NTSTATUS ntsStatus, NTSTATUS ntsSubstatus,
    PWSTR* ppwszOptionalStatusText,
    CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) {

    FACELOGIN_INFO(L"=== ReportResult ENTER (status=0x%08X, substatus=0x%08X, state=%d) ===",
                  ntsStatus, ntsSubstatus, static_cast<int>(m_state));

    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;

    if (ntsStatus == STATUS_SUCCESS) {
        FACELOGIN_INFO(L"Authentication succeeded");
    } else {
        FACELOGIN_WARN(L"Authentication failed: status=0x%08X, substatus=0x%08X",
                      ntsStatus, ntsSubstatus);

        // Turn this into a FAILED state, NOT Waiting: a failed submission
        // (e.g. blank-password credential rejected because the account has a
        // real password, or the policy forbids blank logon) triggers
        // re-enumeration → Advise. The cold-boot Advise branch auto-starts
        // auth when state is Waiting — looping it forever (recognize →
        // submit → reject → re-enumerate → recognize…). Failed instead makes
        // Advise restart the input-detection thread: a key press is the
        // explicit retry, matching the no-match behavior.
        m_state = State::Failed;
        m_statusText = L"登录被拒绝（密码或策略原因），请使用 PIN/密码登录";
        m_password.clear();

        if (m_pCredentialEvents) {
            m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
        }

        if (m_pipeClient) {
            m_pipeClient.reset();
        }
    }

    return S_OK;
}

// ============================================================================
// Private: StartAuth — begin the authentication pipeline
// ============================================================================

void FaceLoginCredential::StartAuth() {
    // CRITICAL SECTION: m_cs is held by the caller (if from background
    // thread). The pipe callbacks also acquire m_cs, so we must NOT hold
    // it here. In our design, only the main thread (Advise) and the
    // input-detection thread call StartAuth, and they do so outside m_cs.
    FACELOGIN_INFO(L"StartAuth: connecting to face service pipe (state=%d)", static_cast<int>(m_state));

    if (m_pipeClient && m_pipeClient->IsConnected()) {
        FACELOGIN_INFO(L"StartAuth: already connected, skipping");
        return;
    }

    m_state = State::Authenticating;
    m_noMatchFailed = false;   // fresh attempt: clear the previous no-match flag
    m_pipeClient = std::make_unique<facelogin::PipeClient>();

    if (m_pipeClient->Connect()) {
        m_pipeClient->SendMessage(facelogin::ipc::MSG_AUTH_REQUEST);

        m_statusText = Text("credential.recognizing", L"识别中...");

        auto self = this;
        m_pipeClient->StartBackgroundRead(
            [self](bool success, const std::wstring& msg) {
                self->OnPipeResponse(success, msg);
            },
            [self](const std::wstring& msg) {
                self->OnPipeStatus(msg);
            });
        FACELOGIN_INFO(L"Pipe connected, auth request sent");
    } else {
        // Pipe connect failed (most likely the FaceLoginService isn't running,
        // e.g. it crashed or was stopped — the named pipe doesn't exist).
        // Set a concrete message and push it to the tile so the user sees WHY
        // nothing is happening, instead of a silent hang or a generic label.
        // (PipeClient::Connect already logged the precise error to
        // credential_provider.log.)
        FACELOGIN_WARN(L"Failed to connect to face service pipe");
        m_statusText = Text("credential.serviceNotRunning", L"人脸识别服务未运行，请检查 FaceLoginService");
        m_state = State::Error;
        if (m_pCredentialEvents) {
            m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
        }
    }
}

// ============================================================================
// Private: StartInputDetectionThread / StopInputDetectionThread
// ============================================================================

void FaceLoginCredential::StartInputDetectionThread() {
    if (m_inputThreadRunning) {
        FACELOGIN_WARN(L"StartInputDetectionThread: thread already running");
        return;
    }

    if (!m_hInputStop) {
        m_hInputStop = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_hInputStop) {
            FACELOGIN_ERROR(L"Failed to create input stop event");
            return;
        }
    } else {
        ResetEvent(m_hInputStop);
    }

    auto* ctx = new InputDetectionContext;
    ctx->pCred = this;

    m_inputThreadRunning = true;
    unsigned threadId = 0;
    m_hInputThread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, InputDetectionThreadProc, ctx, 0, &threadId));
    if (!m_hInputThread || m_hInputThread == INVALID_HANDLE_VALUE) {
        FACELOGIN_ERROR(L"Failed to start input detection thread");
        m_inputThreadRunning = false;
        delete ctx;
    } else {
        FACELOGIN_INFO(L"Input detection thread started (id=%u)", threadId);
    }
}

void FaceLoginCredential::StopInputDetectionThread() {
    if (!m_inputThreadRunning) {
        return;
    }

    FACELOGIN_INFO(L"Stopping input detection thread...");

    // Signal stop
    if (m_hInputStop) {
        SetEvent(m_hInputStop);
    }

    // Wait for thread to exit (up to 2 seconds)
    if (m_hInputThread) {
        DWORD waitResult = WaitForSingleObject(m_hInputThread, 2000);
        if (waitResult == WAIT_TIMEOUT) {
            FACELOGIN_WARN(L"Input thread did not stop within 2s — terminating");
            TerminateThread(m_hInputThread, 0);
        }
        CloseHandle(m_hInputThread);
        m_hInputThread = nullptr;
    }

    if (m_hInputStop) {
        CloseHandle(m_hInputStop);
        m_hInputStop = nullptr;
    }

    m_inputThreadRunning = false;
    FACELOGIN_INFO(L"Input detection thread stopped");
}

// ============================================================================
// Private: Credential Packing
// ============================================================================

HRESULT FaceLoginCredential::PackCredentials(
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs) {

    FACELOGIN_INFO(L"Packing credentials for: %s\\%s (UPN=%s)",
                  m_domain.c_str(), m_username.c_str(),
                  m_upn.empty() ? L"<none>" : m_upn.c_str());

    // Auth package: MSV1_0 for LOGON/UNLOCK.
    // (CredUI/PLAP never reach here — they're filtered in SetUsageScenario.)
    ULONG ulAuthPackage = 0;
    HANDLE hLsa = nullptr;
    NTSTATUS lsastatus = LsaConnectUntrusted(&hLsa);
    if (lsastatus == 0 && hLsa) {
        LSA_STRING pkgName;
        char msvStr[] = "MICROSOFT_AUTHENTICATION_PACKAGE_V1_0";
        pkgName.Buffer = msvStr;
        pkgName.Length = static_cast<USHORT>(strlen(msvStr));
        pkgName.MaximumLength = pkgName.Length;
        lsastatus = LsaLookupAuthenticationPackage(hLsa, &pkgName, &ulAuthPackage);
        if (lsastatus != 0) {
            FACELOGIN_ERROR(L"LsaLookupAuthenticationPackage MSV1_0 failed: 0x%08X", lsastatus);
            ulAuthPackage = 0;
        }
        LsaDeregisterLogonProcess(hLsa);
    } else {
        FACELOGIN_ERROR(L"LsaConnectUntrusted failed: 0x%08X", lsastatus);
    }
    FACELOGIN_INFO(L"Auth package MSV1_0: %lu", ulAuthPackage);

    DWORD packFlags = 0;
    DWORD cbPackedCreds = 0;

    PWSTR pwzPassword = const_cast<PWSTR>(m_password.c_str());

    // Build the packed user name in the correct format.
    // Local/domain: "DOMAIN\Username" (required by CredPackAuthenticationBuffer)
    // MSA/AAD:      UPN "user@domain.com"
    std::wstring packedUser;
    if (!m_upn.empty() && m_upn.find(L'@') != std::wstring::npos) {
        packedUser = m_upn;
    } else {
        packedUser = m_domain + L"\\" + m_username;
    }
    FACELOGIN_INFO(L"CredPack user: \"%s\"", packedUser.c_str());

    if (!CredPackAuthenticationBufferW(
            packFlags,
            const_cast<LPWSTR>(packedUser.c_str()),
            pwzPassword,
            nullptr,
            &cbPackedCreds)) {

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            FACELOGIN_ERROR(L"CredPackAuthenticationBuffer size query failed: %lu",
                           GetLastError());
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }

    // Allocate buffer and pack
    BYTE* pPackedCreds = static_cast<BYTE*>(CoTaskMemAlloc(cbPackedCreds));
    if (!pPackedCreds) {
        return E_OUTOFMEMORY;
    }

    if (!CredPackAuthenticationBufferW(
            packFlags,
            const_cast<LPWSTR>(packedUser.c_str()),
            pwzPassword,
            pPackedCreds,
            &cbPackedCreds)) {
        FACELOGIN_ERROR(L"CredPackAuthenticationBuffer failed: %lu", GetLastError());
        CoTaskMemFree(pPackedCreds);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // CRITICAL: Zero the password immediately after packing
    SecureZeroMemory(pwzPassword, m_password.size() * sizeof(wchar_t));

    pcpcs->rgbSerialization = pPackedCreds;
    pcpcs->cbSerialization = cbPackedCreds;
    pcpcs->ulAuthenticationPackage = ulAuthPackage;
    pcpcs->clsidCredentialProvider = CLSID_FaceLoginProvider;

    FACELOGIN_INFO(L"Credentials packed successfully (%lu bytes, pkg=%lu)",
                   cbPackedCreds, ulAuthPackage);
    return S_OK;
}

// ============================================================================
// Private: Authentication Package Lookup
// ============================================================================

HRESULT FaceLoginCredential::GetAuthenticationPackage(ULONG* pulAuthPackage) {
    // Fall back to Negotiate (0 is treated as Negotiate by LSA)
    *pulAuthPackage = 0;
    return S_OK;
}

// ============================================================================
// Private: Switch to Password Provider
// ============================================================================

HRESULT FaceLoginCredential::SwitchToPasswordProvider() {
    // Signal LogonUI to re-enumerate credentials
    // The user can then select the password provider
    if (m_pProviderEvents) {
        m_pProviderEvents->CredentialsChanged(m_upAdviseContext);
    }

    // Also return NO_CREDENTIAL_FINISHED to deselect our tile
    // This causes LogonUI to show other providers
    // (Actually done in GetSerialization via state change)
    m_state = State::Failed;

    return S_OK;
}

void FaceLoginCredential::OnPipeStatus(const std::wstring& message) {
    const std::wstring localized = LocalizeKey(message);
    EnterCriticalSection(&m_cs);
    m_statusText = localized;
    LeaveCriticalSection(&m_cs);
    FACELOGIN_INFO(L"Status text updated: %s", localized.c_str());
    // Use SetFieldString to update the status text in-place on the lock
    // screen, without triggering re-enumeration (which destroys the pipe).
    if (m_pCredentialEvents) {
        m_pCredentialEvents->SetFieldString(this, 1, localized.c_str());
    }
}

std::wstring FaceLoginCredential::LocalizeKey(const std::wstring& key) const {
    // Pipe payloads are locale keys (see ipc::L10N_* in ipc_protocol.h) — the
    // service never embeds display text. LocaleCatalog resolves: active pack
    // → zh-CN pack → empty (the caller's state default then applies), so a
    // key missing from the packs degrades to Chinese, never to a mixed-
    // language tile or a raw key.
    return m_locale.GetWide(facelogin::WideToUtf8(key));
}

void FaceLoginCredential::OnPipeResponse(bool success, const std::wstring& message) {
    // Ignore LATE results: if the auth was cancelled in the meantime
    // (SetDeselected — user switched to another tile), the read thread may
    // still deliver a terminal message after Disconnect. Applying it would
    // overwrite the Waiting state and make a later tile re-selection auto-
    // restart auth (camera flashes on/off while the user just browses tiles).
    if (m_state != State::Authenticating) {
        FACELOGIN_INFO(L"OnPipeResponse: auth no longer active (state=%d) — ignoring late result",
                       static_cast<int>(m_state));
        return;
    }
    if (success) {
        auto result = facelogin::ipc::ParseAuthMessage(message);

        if (result.status == facelogin::ipc::AuthResult::Status::Success) {
            FACELOGIN_INFO(L"OnPipeResponse: Auth success: domain=%s, username=%s (SID=%s, UPN=%s)",
                          result.domain.c_str(), result.username.c_str(),
                          result.sid.c_str(), result.upn.c_str());
            // NOTE: the password itself is never logged — only metadata.
            m_sid = result.sid;
            m_upn = result.upn;
            m_domain = result.domain;
            m_username = result.username;
            m_password = result.password;
            m_state = State::Ready;
            SetEvent(m_hCredsReady);
            // Ask LogonUI to call GetSerialization again right away
            TriggerReEnumeration();
            return;
        } else if (result.status == facelogin::ipc::AuthResult::Status::Timeout) {
            FACELOGIN_INFO(L"OnPipeResponse: Auth timeout");
            m_state = State::Failed;
        } else if (result.status == facelogin::ipc::AuthResult::Status::NoMatch) {
            // A face was seen but did not match any enrolled face. Show the
            // specific wording (and keep it — the timeout case below shows the
            // generic "未识别到人脸" instead).
            FACELOGIN_INFO(L"OnPipeResponse: Face present but no match");
            m_statusText = Text("credential.noMatch", L"人脸匹配失败，请重试或使用密码登录");
            m_noMatchFailed = true;
            if (m_pCredentialEvents) {
                m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
            }
            m_state = State::Failed;
        } else if (result.status == facelogin::ipc::AuthResult::Status::Error) {
            FACELOGIN_WARN(L"OnPipeResponse: Auth error: %s", result.errorMessage.c_str());
            // Passwordless account: show the notice in-place and stop — do NOT
            // trigger re-enumeration or submit any credentials.
            if (result.errorMessage == facelogin::ipc::MSG_PASSWORDLESS_NOTICE) {
                m_statusText = LocalizeKey(result.errorMessage);
                m_state = State::Blocked;
                if (m_pCredentialEvents) {
                    m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
                }
                return;
            }
            // Surface the service's specific error (e.g. the anti-spoof
            // rejection key) on the lock screen instead of the generic
            // "service unavailable".
            if (!result.errorMessage.empty()) {
                m_statusText = LocalizeKey(result.errorMessage);
            }
            m_state = State::Error;
        }
    } else {
        FACELOGIN_WARN(L"OnPipeResponse: Read failed — server disconnected?");
        m_state = State::Error;
    }
    TriggerReEnumeration();
}

// ============================================================================
// Private: Trigger Re-enumeration
// ============================================================================

void FaceLoginCredential::TriggerReEnumeration() {
    if (m_pProviderEvents) {
        FACELOGIN_DEBUG(L"Triggering CredentialsChanged");
        m_pProviderEvents->CredentialsChanged(m_upAdviseContext);
    }
}
