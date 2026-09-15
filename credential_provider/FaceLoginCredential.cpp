#include "FaceLoginCredential.h"
#include "FaceLoginProvider.h"
#include "resource.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include "../common/session_util.h"
#include <wincred.h>
#include <ntstatus.h>
#include <ntsecapi.h>
#include <sddl.h>
#include <shlwapi.h>
#include <array>
#include <vector>
#include <process.h>
#include <algorithm>
#include <new>

#pragma comment(lib, "credui.lib")
#pragma comment(lib, "ntdll.lib")

namespace {

facelogin::StatusOverlayTone OverlayToneForStatusKey(
    const std::wstring& key) {
    if (key == facelogin::ipc::L10N_POSE_ACCEPTABLE ||
        key == facelogin::ipc::L10N_POSE_INVALID ||
        key == facelogin::ipc::L10N_POSE_YAW_LEFT ||
        key == facelogin::ipc::L10N_POSE_YAW_RIGHT ||
        key == facelogin::ipc::L10N_POSE_PITCH_DOWN ||
        key == facelogin::ipc::L10N_POSE_PITCH_UP ||
        key == facelogin::ipc::L10N_POSE_ROLL_RIGHT ||
        key == facelogin::ipc::L10N_POSE_ROLL_LEFT ||
        key == facelogin::ipc::L10N_BLINK_PROMPT) {
        return facelogin::StatusOverlayTone::Guidance;
    }
    return facelogin::StatusOverlayTone::Progress;
}

} // namespace

// ============================================================================
// Input-detection thread (unlock scenario)
// ============================================================================
//
// Runs as a background thread, polling the physical state of virtual keys.
// A rising edge on a keyboard key or mouse button calls StartAuth(). Mouse
// movement alone does not change any of these states and cannot trigger
// recognition. Once auth completes, the pipe callback stores credentials and
// triggers CredentialsChanged(), causing LogonUI to re-enumerate and call
// GetSerialization(), which then packs and returns the ready credentials.
//
// The thread stops when:
//   - New input is detected and StartAuthAsync() succeeds, OR
//   - The stop event is signaled (UnAdvise / destructor / 30s timeout), OR
//   - UnAdvise() clears the Events2 callback and the thread notices

struct InputDetectionContext {
    FaceLoginCredential* pCred;
    HANDLE stopEvent;
    unsigned long long activationId;
};

struct AuthConnectContext {
    FaceLoginCredential* pCred;
    std::shared_ptr<facelogin::PipeClient> client;
    unsigned long long attemptId;
};

struct CredentialCallbackLifetime {
    explicit CredentialCallbackLifetime(FaceLoginCredential* value) : cred(value) {
        cred->AddRef();
    }
    ~CredentialCallbackLifetime() { cred->Release(); }
    FaceLoginCredential* cred;
};

namespace {

constexpr DWORD kInputPollIntervalMs = 40;
constexpr ULONGLONG kInputBaselineGraceMs = 350;

bool IsMouseButtonVirtualKey(int virtualKey) {
    switch (virtualKey) {
    case VK_LBUTTON:
    case VK_RBUTTON:
    case VK_MBUTTON:
    case VK_XBUTTON1:
    case VK_XBUTTON2:
        return true;
    default:
        return false;
    }
}

bool IsVirtualKeyDown(int virtualKey) {
    // Use only the high bit. The low transition bit is legacy process-global
    // state and is not reliable for polling.
    return (GetAsyncKeyState(virtualKey) & static_cast<SHORT>(0x8000)) != 0;
}

} // namespace

unsigned __stdcall InputDetectionThreadProc(void* pParam) {
    auto* ctx = static_cast<InputDetectionContext*>(pParam);
    FaceLoginCredential* pCred = ctx->pCred;
    const HANDLE stopEvent = ctx->stopEvent;
    const auto activationId = ctx->activationId;
    delete ctx;

    FACELOGIN_INFO(L"[InputThread] Started — polling keyboard and mouse-button states every %lums",
                   kInputPollIntervalMs);

    const DWORD timeoutSec = 30;
    ULONGLONG startTick = GetTickCount64();
    const ULONGLONG baselineGraceUntil = startTick + kInputBaselineGraceMs;
    std::array<bool, 256> previousDown{};

    // Prime the snapshot so the click/key used to select the credential tile
    // is not mistaken for the input that should start authentication.
    for (int virtualKey = 1; virtualKey <= 0xFF; ++virtualKey) {
        previousDown[virtualKey] = IsVirtualKeyDown(virtualKey);
    }
    FACELOGIN_INFO(L"[InputThread] Baseline captured; selection-input grace=%llums",
                   kInputBaselineGraceMs);

    // Loop forever (until the stop event is signaled).  The 30s timeout does
    // NOT kill the thread — it only restarts the idle window so a user who
    // waits longer than 30s before pressing a key can still trigger auth.
    while (true) {
        // Check stop signal (non-blocking)
        DWORD waitResult = WaitForSingleObject(stopEvent, 0);
        if (waitResult == WAIT_OBJECT_0) {
            FACELOGIN_INFO(L"[InputThread] Stop event signaled — exiting");
            break;
        }

        // Idle-window timeout: restart the window instead of exiting, so a
        // late keypress still works. (Previously the thread exited after 30s
        // of no input, leaving no path to restart it — a later keypress did
        // nothing.)
        ULONGLONG nowTick = GetTickCount64();
        if (nowTick - startTick > static_cast<ULONGLONG>(timeoutSec) * 1000) {
            FACELOGIN_INFO(L"[InputThread] 30s idle — restarting idle window");
            startTick = nowTick;
        }

        bool inputDetected = false;
        for (int virtualKey = 1; virtualKey <= 0xFF; ++virtualKey) {
            const bool currentDown = IsVirtualKeyDown(virtualKey);
            const bool risingEdge = currentDown && !previousDown[virtualKey];
            previousDown[virtualKey] = currentDown;
            if (risingEdge && nowTick >= baselineGraceUntil) {
                const bool mouseButton = IsMouseButtonVirtualKey(virtualKey);
                FACELOGIN_INFO(L"[InputThread] New %s input detected (VK=0x%02X)",
                               mouseButton ? L"mouse-button" : L"keyboard",
                               virtualKey);
                inputDetected = true;
                break;
            }
        }

        if (inputDetected) {
            // UnAdvise/SetDeselected can invalidate the detector between the
            // key snapshot above and this point.  Re-check the same stop
            // event and activation generation immediately before starting
            // authentication; otherwise a stale key can create a second
            // AUTH_REQUEST while LogonUI is already leaving this credential.
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0 ||
                !pCred->IsInputActivationValid(activationId)) {
                FACELOGIN_INFO(L"[InputThread] Input discarded — activation invalidated");
                break;
            }

            bool authStarted = false;
            const auto state = pCred->GetState();
            if (state == FaceLoginCredential::State::Waiting) {
                authStarted = pCred->StartAuthAsync(pCred->InputAuthTrigger(), activationId);
            } else if (state == FaceLoginCredential::State::Failed ||
                       state == FaceLoginCredential::State::Error) {
                // Failed/error states deliberately keep their message visible
                // until the user provides a new input. Once that input
                // arrives, atomically consume the retry transition before
                // starting a new attempt.
                if (pCred->TransitionState(state,
                                           FaceLoginCredential::State::Waiting)) {
                    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0 ||
                        !pCred->IsInputActivationValid(activationId)) {
                        FACELOGIN_INFO(L"[InputThread] Retry input discarded — activation invalidated");
                        break;
                    }
                    pCred->SetStatusText(L"");
                    FACELOGIN_INFO(L"[InputThread] Retry input accepted — restarting authentication");
                    authStarted = pCred->StartAuthAsync(pCred->InputAuthTrigger(), activationId);
                }
            }
            if (authStarted) break;

            // A rare local startup failure (for example, unable to allocate
            // the connection context) must not make the selected tile inert.
            // Keep this detector alive so the next deliberate key/button edge
            // can retry; the failure reason remains visible in the tile.
            FACELOGIN_WARN(L"[InputThread] Authentication did not start — remaining armed for retry");
        }

        // Wait with a timeout so StopInputDetectionThread() wakes the worker
        // immediately instead of waiting for the next polling interval.
        WaitForSingleObject(stopEvent, kInputPollIntervalMs);
    }

    FACELOGIN_INFO(L"[InputThread] Exiting");
    EnterCriticalSection(&pCred->m_cs);
    pCred->m_inputThreadRunning = false;
    pCred->m_inputDetectionEnabled = false;
    LeaveCriticalSection(&pCred->m_cs);
    pCred->Release();
    return 0;
}

unsigned __stdcall AuthConnectThreadProc(void* pParam) {
    std::unique_ptr<AuthConnectContext> ctx(
        static_cast<AuthConnectContext*>(pParam));
    FaceLoginCredential* cred = ctx->pCred;
    const auto client = ctx->client;
    const auto attemptId = ctx->attemptId;

    ULONGLONG deadline = 0;
    EnterCriticalSection(&cred->m_cs);
    deadline = cred->m_authDeadlineTick;
    LeaveCriticalSection(&cred->m_cs);

    const ULONGLONG now = GetTickCount64();
    const DWORD connectBudget = deadline > now
        ? static_cast<DWORD>(std::min<ULONGLONG>(5000ULL, deadline - now))
        : 0;
    FACELOGIN_INFO(L"AuthAttempt: attempt=%llu connectStart budgetMs=%lu",
                   attemptId, connectBudget);

    bool startedReader = false;
    if (connectBudget != 0 &&
        WaitForSingleObject(cred->m_hAuthStop, 0) != WAIT_OBJECT_0 &&
        client->Connect(connectBudget, cred->m_hAuthStop) &&
        cred->IsAttemptActive(attemptId) &&
        client->SendMessage(facelogin::ipc::MSG_AUTH_REQUEST) &&
        cred->IsAttemptActive(attemptId)) {
        const ULONGLONG readNow = GetTickCount64();
        const DWORD remaining = deadline > readNow
            ? static_cast<DWORD>(std::min<ULONGLONG>(deadline - readNow, MAXDWORD))
            : 1;
        auto lifetime = std::make_shared<CredentialCallbackLifetime>(cred);
        startedReader = client->StartBackgroundRead(
            [lifetime, attemptId](bool success, const std::wstring& msg) {
                lifetime->cred->OnPipeResponse(attemptId, success, msg);
            },
            [lifetime, attemptId](const std::wstring& msg) {
                lifetime->cred->OnPipeStatus(attemptId, msg);
            },
            remaining);
        if (startedReader) {
            FACELOGIN_INFO(L"AuthAttempt: attempt=%llu pipeConnected=1 requestSent=1 readTimeoutMs=%lu",
                           attemptId, remaining);
        }
    }

    if (!startedReader) {
        const bool cancelled = WaitForSingleObject(cred->m_hAuthStop, 0) == WAIT_OBJECT_0 ||
                               !cred->IsAttemptActive(attemptId);
        client->Disconnect();
        if (!cancelled && cred->IsAttemptActive(attemptId)) {
            cred->OnPipeResponse(attemptId, false, L"");
        }
        FACELOGIN_INFO(L"AuthAttempt: attempt=%llu connectionFinished started=0 cancelled=%d",
                       attemptId, static_cast<int>(cancelled));
    }

    EnterCriticalSection(&cred->m_cs);
    cred->m_authThreadRunning = false;
    LeaveCriticalSection(&cred->m_cs);
    cred->Release();
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
    m_hAuthStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_hAuthStop) {
        FACELOGIN_ERROR(L"Failed to create auth stop event");
    }

    const std::wstring installDir = ReadRegString(REGVAL_INSTALL_PATH, L"");
    const std::string uiLang = facelogin::LoadConfig(installDir).ui_language;
    const bool localeOk = m_locale.Load(installDir, uiLang);
    FACELOGIN_INFO(L"[l10n] Credential: installDir='%ls' ui_language='%hs' locale='%hs' loadOk=%d",
                   installDir.c_str(), uiLang.c_str(), m_locale.locale().c_str(), localeOk);

    FACELOGIN_DEBUG(L"FaceLoginCredential created");
}

FaceLoginCredential::~FaceLoginCredential() {
    EnterCriticalSection(&m_cs);
    m_statusOverlayAllowed = false;
    LeaveCriticalSection(&m_cs);
    m_statusOverlay.Destroy(L"credential_destructor");
    CancelActiveAttempt(false);
    StopInputDetectionThread();

    // LogonUI normally calls UnAdvise before releasing the credential.  Keep
    // destruction self-contained as well, because teardown after a service
    // failure or LogonUI restart must not leak the Events2 interface.
    ICredentialProviderCredentialEvents2* events2 = nullptr;
    EnterCriticalSection(&m_cs);
    events2 = m_pCredentialEvents2;
    m_pCredentialEvents2 = nullptr;
    LeaveCriticalSection(&m_cs);
    if (events2) events2->Release();

    // SENSITIVE: Zero the password from memory
    ClearCredentials();

    if (m_hCredsReady) {
        CloseHandle(m_hCredsReady);
        m_hCredsReady = nullptr;
    }
    if (m_hAuthStop) {
        CloseHandle(m_hAuthStop);
        m_hAuthStop = nullptr;
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
    EnterCriticalSection(&m_cs);
    m_pProviderEvents = pEvents;
    m_upAdviseContext = upAdviseContext;
    LeaveCriticalSection(&m_cs);
}

void FaceLoginCredential::UnadviseProvider() {
    EnterCriticalSection(&m_cs);
    m_pProviderEvents = nullptr;
    m_upAdviseContext = 0;
    LeaveCriticalSection(&m_cs);
}

bool FaceLoginCredential::IsAutoSubmitReady() const {
    EnterCriticalSection(const_cast<CRITICAL_SECTION*>(&m_cs));
    const bool ready = m_state == State::Ready &&
                       m_authTrigger == AuthTrigger::LoginEntryAutomatic &&
                       m_autoSubmitEligible;
    LeaveCriticalSection(const_cast<CRITICAL_SECTION*>(&m_cs));
    return ready;
}

FaceLoginCredential::AuthTrigger FaceLoginCredential::InputAuthTrigger() const {
    return m_pProvider && m_pProvider->IsLoginEntry()
        ? AuthTrigger::LoginEntryKeyPress
        : AuthTrigger::UnlockKeyPress;
}

FaceLoginCredential::State FaceLoginCredential::GetState() const {
    EnterCriticalSection(const_cast<CRITICAL_SECTION*>(&m_cs));
    const State state = m_state;
    LeaveCriticalSection(const_cast<CRITICAL_SECTION*>(&m_cs));
    return state;
}

bool FaceLoginCredential::TransitionState(State expected, State next) {
    EnterCriticalSection(&m_cs);
    if (m_state != expected) {
        LeaveCriticalSection(&m_cs);
        return false;
    }
    m_state = next;
    LeaveCriticalSection(&m_cs);
    return true;
}

bool FaceLoginCredential::IsAttemptActive(AuthAttemptId attemptId) const {
    EnterCriticalSection(const_cast<CRITICAL_SECTION*>(&m_cs));
    const bool active = m_state == State::Authenticating &&
                        m_activeAttemptId == attemptId;
    LeaveCriticalSection(const_cast<CRITICAL_SECTION*>(&m_cs));
    return active;
}

bool FaceLoginCredential::IsInputActivationValid(InputActivationId activationId) const {
    EnterCriticalSection(const_cast<CRITICAL_SECTION*>(&m_cs));
    const bool valid = m_inputDetectionEnabled &&
                       !m_deselected &&
                       m_inputThreadRunning &&
                       m_inputActivationId == activationId &&
                       m_pCredentialEvents2 != nullptr;
    LeaveCriticalSection(const_cast<CRITICAL_SECTION*>(&m_cs));
    return valid;
}

void FaceLoginCredential::SetStatusText(const std::wstring& text) {
    EnterCriticalSection(&m_cs);
    m_statusText = text;
    LeaveCriticalSection(&m_cs);
}

std::wstring FaceLoginCredential::VisibleStatusText() const {
    return CurrentStatusPresentation().text;
}

facelogin::StatusOverlayPresentation
FaceLoginCredential::CurrentStatusPresentation() const {
    facelogin::StatusOverlayPresentation presentation;
    State state;
    std::wstring statusText;
    bool noMatchFailed = false;
    bool overlayAllowed = false;
    bool deselected = false;
    facelogin::StatusOverlayTone liveTone =
        facelogin::StatusOverlayTone::Neutral;
    EnterCriticalSection(const_cast<CRITICAL_SECTION*>(&m_cs));
    state = m_state;
    statusText = m_statusText;
    noMatchFailed = m_noMatchFailed;
    overlayAllowed = m_statusOverlayAllowed;
    deselected = m_deselected;
    liveTone = m_statusOverlayTone;
    LeaveCriticalSection(const_cast<CRITICAL_SECTION*>(&m_cs));

    switch (state) {
    case State::Waiting:
        presentation.text =
            Text("credential.pressAnyKey", L"按下任意按键以开始人脸识别");
        presentation.tone = facelogin::StatusOverlayTone::Neutral;
        break;
    case State::Authenticating:
        presentation.text = statusText.empty()
            ? Text("credential.recognizing", L"识别中...")
            : statusText;
        presentation.tone = liveTone;
        break;
    case State::Ready:
        presentation.text =
            Text("credential.success", L"人脸识别成功，正在解锁...");
        presentation.tone = facelogin::StatusOverlayTone::Success;
        break;
    case State::Submitted:
        presentation.text =
            Text("credential.submitted", L"正在验证登录，等待 Windows 确认...");
        presentation.tone = facelogin::StatusOverlayTone::Neutral;
        break;
    case State::Failed:
        if (!statusText.empty()) {
            presentation.text = statusText;
        } else if (noMatchFailed) {
            presentation.text =
                Text("credential.noMatch", L"人脸匹配失败，请重试或使用密码登录");
        } else {
            presentation.text =
                Text("credential.noFace", L"未识别到人脸，请重试或使用密码登录");
        }
        presentation.tone = facelogin::StatusOverlayTone::Error;
        break;
    case State::Blocked:
        presentation.text = statusText.empty()
            ? Text("credential.passwordless", L"该账号无密码，人脸识别无法用于解锁，请使用 PIN/Hello 登录")
            : statusText;
        presentation.tone = facelogin::StatusOverlayTone::Error;
        break;
    case State::Error:
        presentation.text = statusText.empty()
            ? Text("credential.serviceUnavailable", L"人脸登录服务不可用")
            : statusText;
        presentation.tone = facelogin::StatusOverlayTone::Error;
        break;
    default:
        break;
    }

    presentation.visible = overlayAllowed && !deselected &&
                           state != State::Submitted &&
                           !presentation.text.empty();
    return presentation;
}

void FaceLoginCredential::NotifyFieldString(const std::wstring& text) {
    ICredentialProviderCredentialEvents2* events2 = nullptr;
    EnterCriticalSection(&m_cs);
    events2 = m_pCredentialEvents2;
    if (events2) events2->AddRef();
    LeaveCriticalSection(&m_cs);

    if (events2) {
        // Do not hold m_cs while calling LogonUI.  Callbacks can re-enter the
        // credential provider, and Events2 keeps a single status transition
        // from being rendered as intermediate blank/stale text.
        const HRESULT beginHr = events2->BeginFieldUpdates();
        events2->SetFieldString(this, 1, text.c_str());
        if (SUCCEEDED(beginHr)) events2->EndFieldUpdates();
        events2->Release();
    }

    const auto presentation = CurrentStatusPresentation();
    if (presentation.visible) {
        EnsureStatusOverlay(L"status_publish");
    } else {
        m_statusOverlay.Hide();
    }
}

void FaceLoginCredential::NotifyCurrentStatus() {
    NotifyFieldString(VisibleStatusText());
}

void FaceLoginCredential::NotifyCredentialsChanged() {
    ICredentialProviderEvents* events = nullptr;
    UINT_PTR context = 0;
    EnterCriticalSection(&m_cs);
    events = m_pProviderEvents;
    context = m_upAdviseContext;
    if (events) events->AddRef();
    LeaveCriticalSection(&m_cs);

    if (events) {
        events->CredentialsChanged(context);
        events->Release();
    }
}

void FaceLoginCredential::EnsureStatusOverlay(const wchar_t* reason) {
    if (m_pProvider && m_pProvider->IsCredUI()) return;

    const auto presentation = CurrentStatusPresentation();
    if (!presentation.visible) {
        m_statusOverlay.Hide();
        return;
    }
    if (m_statusOverlay.IsCreated()) {
        m_statusOverlay.Update(presentation);
        return;
    }

    ICredentialProviderCredentialEvents2* events2 = nullptr;
    EnterCriticalSection(&m_cs);
    if (m_statusOverlayUnavailable) {
        LeaveCriticalSection(&m_cs);
        return;
    }
    events2 = m_pCredentialEvents2;
    if (events2) events2->AddRef();
    LeaveCriticalSection(&m_cs);
    if (!events2) return;

    const bool created = m_statusOverlay.Create(events2, presentation, reason);
    events2->Release();
    if (!created) {
        EnterCriticalSection(&m_cs);
        m_statusOverlayUnavailable = true;
        LeaveCriticalSection(&m_cs);
    }
}

void FaceLoginCredential::ClearCredentials() {
    EnterCriticalSection(&m_cs);
    SecureZeroMemory(m_password.data(), m_password.size() * sizeof(wchar_t));
    m_sid.clear();
    m_upn.clear();
    m_domain.clear();
    m_username.clear();
    m_password.clear();
    LeaveCriticalSection(&m_cs);
}

void FaceLoginCredential::CancelActiveAttempt(bool resetToWaiting) {
    std::shared_ptr<facelogin::PipeClient> client;
    bool cancelledAttempt = false;
    bool detachedCompletedPipe = false;
    if (m_hAuthStop) SetEvent(m_hAuthStop);
    EnterCriticalSection(&m_cs);
    const bool authenticating = m_state == State::Authenticating;
    const bool preserveCredentials =
        m_state == State::Ready || m_state == State::Submitted;

    // A completed authentication still owns the pipe until the read thread
    // has been joined.  LogonUI may call UnAdvise while it is re-enumerating
    // the credential, immediately before GetSerialization().  Detach that
    // pipe, but never treat its existence as proof that authentication is
    // still active: doing so would erase the credentials that OnPipeResponse
    // has just prepared for serialization.
    if (authenticating || m_pipeClient) {
        if (authenticating) {
            cancelledAttempt = true;
            ++m_activeAttemptId;
            m_autoSubmitEligible = false;
        } else if (preserveCredentials) {
            detachedCompletedPipe = true;
        }

        client = std::move(m_pipeClient);
        m_authDeadlineTick = 0;

        if (!preserveCredentials) {
            SecureZeroMemory(m_password.data(), m_password.size() * sizeof(wchar_t));
            m_sid.clear();
            m_upn.clear();
            m_domain.clear();
            m_username.clear();
            m_password.clear();
            m_noMatchFailed = false;
            if (resetToWaiting && authenticating) {
                m_state = State::Waiting;
                m_statusText.clear();
            }
        }
    }
    LeaveCriticalSection(&m_cs);

    if (client) client->Disconnect();
    JoinAuthConnectThread();
    if (cancelledAttempt) {
        FACELOGIN_INFO(L"Authentication attempt cancelled");
    } else if (detachedCompletedPipe) {
        FACELOGIN_INFO(L"Completed authentication pipe detached; credentials preserved");
    }
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
    FACELOGIN_INFO(L"=== Advise ENTER (state=%d, pcpce=%p) ===",
                   static_cast<int>(GetState()), pcpce);

    ICredentialProviderCredentialEvents2* advisedEvents2 = nullptr;
    if (!pcpce || FAILED(pcpce->QueryInterface(
            IID_ICredentialProviderCredentialEvents2,
            reinterpret_cast<void**>(&advisedEvents2))) || !advisedEvents2) {
        // ICredentialProviderCredential::Advise has a legacy base-interface
        // parameter by ABI design. FaceLogin deliberately does not retain or
        // use it: all supported systems must provide Events2.
        FACELOGIN_ERROR(L"Advise: ICredentialProviderCredentialEvents2 is required");
        return E_NOINTERFACE;
    }

    EnterCriticalSection(&m_cs);
    auto* previousEvents2 = m_pCredentialEvents2;
    m_pCredentialEvents2 = advisedEvents2;
    m_statusOverlayUnavailable = false;
    LeaveCriticalSection(&m_cs);
    if (previousEvents2) previousEvents2->Release();

    const State state = GetState();
    bool deselected = false;
    EnterCriticalSection(&m_cs);
    deselected = m_deselected;
    LeaveCriticalSection(&m_cs);

    // Guard: if we already have credentials ready from a previous
    // auth round, don't restart the flow.  This prevents an infinite
    // loop where OnPipeResponse → CredentialsChanged → Advise()
    // overwrites Ready back to Authenticating.
    // NOTE: m_password may legitimately be EMPTY here (passwordless
    // record — blank-credential unlock). The check is on the Ready
    // state only.
    if (state == State::Ready) {
        FACELOGIN_INFO(L"Advise: credentials already ready, skipping auth restart");
        NotifyCurrentStatus();
        return S_OK;
    }

    // Submitted: the credential is with LogonUI/LSA now — never restart auth
    // while the outcome is pending (would re-recognize and re-submit).
    if (state == State::Submitted) {
        FACELOGIN_INFO(L"Advise: credential submitted, skipping auth restart");
        return S_OK;
    }

    // Blocked (passwordless notice shown): keep showing the notice — do not
    // auto-restart authentication on re-enumeration (would loop forever).
    if (state == State::Blocked) {
        FACELOGIN_INFO(L"Advise: blocked (passwordless notice), skipping auth restart");
        NotifyCurrentStatus();
        return S_OK;
    }

    // A connection worker may still be waiting for the service, so state is
    // the authoritative guard; pipe connectivity is deliberately irrelevant.
    if (state == State::Authenticating) {
        FACELOGIN_INFO(L"Advise: already authenticating, skipping auth restart");
        NotifyCurrentStatus();
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
    if (state == State::Failed || state == State::Error) {
        // Failed (no match / timeout / submission rejected) and Error
        // (service-side rejection: anti-spoof attack, blink liveness failed,
        // service unavailable) both keep their specific status text — the
        // unconditional Waiting reset below would otherwise show "按下任意键"
        // and drop the real reason. Never auto-restart auth; restart the
        // input-detection thread so the NEXT key press retries (or the user
        // clicks the tile — SetSelected handles that).
        FACELOGIN_INFO(L"Advise: %s state — restarting input detection (key press retries)",
                       state == State::Failed ? L"failed" : L"error");
        NotifyCurrentStatus();
        if (!deselected) StartInputDetectionThread();
        return S_OK;
    }

    if (deselected) {
        FACELOGIN_INFO(L"Advise: face tile is deselected — activation remains suspended");
        return S_OK;
    }

    // Except for the single claimed login-entry automatic attempt, camera/auth
    // activation is deferred to SetSelected() so ordinary unlock and retry
    // flows only react while our tile is active.
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
    bool loginEntry = m_pProvider ? m_pProvider->IsLoginEntry() : false;
    bool credUI = m_pProvider ? m_pProvider->IsCredUI() : false;
    FACELOGIN_INFO(L"Advise: loginEntry=%d, credUI=%d, m_pProvider=%p",
                  loginEntry, credUI, m_pProvider);

    EnterCriticalSection(&m_cs);
    m_state = State::Waiting;
    LeaveCriticalSection(&m_cs);

    if (loginEntry) {
        if (ReadRegDword(REGVAL_COLD_BOOT_KEY_TRIGGER, 0) != 0) {
            FACELOGIN_INFO(L"Advise: login entry + key-trigger enabled — waiting for key press");
            StartInputDetectionThread();
            NotifyFieldString(Text("credential.pressAnyKey", L"按下任意按键以开始人脸识别"));
        } else {
            bool firstInstanceAttempt = false;
            EnterCriticalSection(&m_cs);
            if (!m_autoStartConsumed) {
                m_autoStartConsumed = true;
                firstInstanceAttempt = true;
            }
            LeaveCriticalSection(&m_cs);

            const ULONGLONG generation = m_pProvider
                ? m_pProvider->GetLoginEntryGeneration()
                : 0;
            const DWORD sessionId = m_pProvider
                ? m_pProvider->GetLoginEntrySessionId()
                : 0xFFFFFFFF;
            const bool generationClaimed = firstInstanceAttempt &&
                facelogin::TryClaimAutomaticLoginEntry(generation, sessionId);
            const bool resumeClaimed = firstInstanceAttempt &&
                !generationClaimed && m_pProvider &&
                m_pProvider->ConsumeAutomaticResume(generation);
            FACELOGIN_INFO(L"LoginEntry: generation=%llu sessionId=%lu "
                           L"autoAttemptGeneration=%llu keyTrigger=0 autoClaim=%d autoResume=%d",
                           generation, sessionId,
                           facelogin::GetAutoAttemptGeneration(),
                           static_cast<int>(generationClaimed),
                           static_cast<int>(resumeClaimed));
            if (generationClaimed) {
                StartAuthAsync(AuthTrigger::LoginEntryAutomatic);
            } else if (resumeClaimed) {
                // Wait until LogonUI confirms that the replacement credential
                // is actually selected. This prevents a transient rebuild from
                // opening the camera while the password/PIN tile is active.
                EnterCriticalSection(&m_cs);
                m_pendingAutomaticResume = true;
                LeaveCriticalSection(&m_cs);
                FACELOGIN_INFO(L"AutoResume: continuation pending SetSelected");
            } else {
                StartInputDetectionThread();
                NotifyFieldString(Text("credential.pressAnyKey", L"按下任意按键以开始人脸识别"));
            }
        }
    } else {
        // Non-cold-boot (unlock / switch user / PIN-reset wizard): keep
        // activation deferred to SetSelected. LogonUI calls SetSelected for
        // the default-selected tile here, while flows that never select our
        // tile (MSA PIN-reset wizard, typing on the PIN/password tile) never
        // start the camera — that was the PIN-unavailable fix.
        if (!facelogin::PipeClient::ProbeServiceAvailable()) {
            FACELOGIN_WARN(L"Advise: service pipe missing — showing service-not-running notice");
            const std::wstring status = Text("credential.serviceNotRunning", L"人脸识别服务未运行，请检查 FaceLoginService");
            SetStatusText(status);
            EnterCriticalSection(&m_cs);
            m_state = State::Error;
            LeaveCriticalSection(&m_cs);
            NotifyFieldString(status);
            return S_OK;
        }
    }

    FACELOGIN_INFO(L"=== Advise EXIT (state=%d) ===", static_cast<int>(GetState()));
    return S_OK;
}

STDMETHODIMP FaceLoginCredential::UnAdvise() {
    FACELOGIN_INFO(L"=== UnAdvise ENTER ===");

    EnterCriticalSection(&m_cs);
    m_statusOverlayAllowed = false;
    LeaveCriticalSection(&m_cs);
    m_statusOverlay.Destroy(L"unadvise");

    bool armAutomaticResume = false;
    ULONGLONG resumeGeneration = 0;

    // Invalidate UI/input activation before cancelling or joining anything.
    // LogonUI can call UnAdvise while the input worker is between a physical
    // key edge and StartAuthAsync(); the worker must see this fence and drop
    // that stale input instead of creating another pipe request.
    EnterCriticalSection(&m_cs);
    armAutomaticResume =
        m_state == State::Authenticating &&
        m_authTrigger == AuthTrigger::LoginEntryAutomatic &&
        !m_deselected;
    m_deselected = true;
    m_pendingAutomaticResume = false;
    ++m_inputActivationId;
    m_inputDetectionEnabled = false;
    LeaveCriticalSection(&m_cs);

    if (armAutomaticResume && m_pProvider && m_pProvider->IsLoginEntry()) {
        resumeGeneration = m_pProvider->GetLoginEntryGeneration();
        m_pProvider->ArmAutomaticResume(resumeGeneration);
        FACELOGIN_INFO(L"AutoResume: interrupted automatic attempt generation=%llu",
                       resumeGeneration);
    }

    CancelActiveAttempt(false);
    // Stop the input-detection thread after invalidating its activation and
    // cancelling the pipe. StartAuthAsync() may currently be connecting from
    // that thread, and the cancellation event must wake it before we join it.
    StopInputDetectionThread();

    EnterCriticalSection(&m_cs);
    auto* events2 = m_pCredentialEvents2;
    m_pCredentialEvents2 = nullptr;
    LeaveCriticalSection(&m_cs);
    if (events2) events2->Release();
    return S_OK;
}

// ============================================================================
// ICredentialProviderCredential — SetSelected/SetDeselected
// ============================================================================

STDMETHODIMP FaceLoginCredential::SetSelected(BOOL* pbAutoLogon) {
    FACELOGIN_INFO(L"=== SetSelected ENTER (state=%d, *pbAutoLogon=%d) ===",
                  static_cast<int>(GetState()),
                  pbAutoLogon ? static_cast<int>(*pbAutoLogon) : -1);

    bool loginEntry = m_pProvider ? m_pProvider->IsLoginEntry() : false;
    bool credUI = m_pProvider ? m_pProvider->IsCredUI() : false;

    bool resumeAutomaticAttempt = false;
    EnterCriticalSection(&m_cs);
    m_deselected = false;
    m_statusOverlayAllowed = !credUI;
    m_statusOverlayUnavailable = false;
    if (m_state == State::Waiting && m_pendingAutomaticResume) {
        m_pendingAutomaticResume = false;
        resumeAutomaticAttempt = true;
    }
    LeaveCriticalSection(&m_cs);

    EnsureStatusOverlay(L"tile_selected");

    if (resumeAutomaticAttempt) {
        FACELOGIN_INFO(L"AutoResume: selected replacement credential — resuming automatic authentication");
        if (!StartAuthAsync(AuthTrigger::LoginEntryAutomatic)) {
            StartInputDetectionThread();
        }
    }

    // Activation model (user-specified):
    //   - ONLY the claimed login entry triggers auth automatically, from
    //     Advise(). autoLogon remains FALSE until its credentials are Ready.
    //     The "开机启动需按键触发" setting governs that first trigger.
    //   - A one-shot continuation interrupted by LogonUI re-enumeration starts
    //     here only after the replacement face tile is selected.
    //   - Every other later activation — re-selecting after switching away, or
    //     retrying after a failure/timeout — must wait for a key press. This
    //     preserves the lock-screen behavior the user already relies on.
    const State state = GetState();
    bool inputThreadRunning = false;
    EnterCriticalSection(&m_cs);
    inputThreadRunning = m_inputThreadRunning;
    LeaveCriticalSection(&m_cs);
    if (!resumeAutomaticAttempt && (state == State::Failed || state == State::Error)) {
        // Failed (no match / timeout / submission rejected) / Error (anti-spoof,
        // blink liveness, service unavailable) + tile re-selected: start waiting
        // for a key press to retry. Never auto-restart — the failure text stays
        // visible and the next key press is the explicit retry.
        FACELOGIN_INFO(L"SetSelected: %s state — restarting input detection (key press retries)",
                       state == State::Failed ? L"failed" : L"error");
        StartInputDetectionThread();
    } else if (!resumeAutomaticAttempt && state == State::Waiting && !inputThreadRunning) {
        // Waiting + (re)selected — either the initial selection after a cold
        // boot that did NOT auto-start (key-trigger on), or the user switched
        // back to the face tile after SetDeselected stopped everything.
        // Either way: start the input-detection thread and require a key press.
        FACELOGIN_INFO(L"SetSelected: tile selected — starting input detection (key press starts auth)");
        StartInputDetectionThread();
        // Repush the Waiting text (clears any residual "识别中..." / stale text)
        SetStatusText(L"");
        NotifyCurrentStatus();
    }

    if (GetState() == State::Ready) {
        // User-triggered recognition still needs SetSelected's auto-logon
        // response; provider-level auto-logon is reserved for an automatic
        // login-entry attempt that has reached Ready.
        *pbAutoLogon = TRUE;
    } else {
        // Unlock / CredUI + still Waiting: no auto-logon; we wait for the bg thread.
        *pbAutoLogon = FALSE;
    }

    FACELOGIN_INFO(L"=== SetSelected EXIT (*pbAutoLogon=%d, loginEntry=%d, credUI=%d, state=%d) ===",
                  *pbAutoLogon, static_cast<int>(loginEntry), static_cast<int>(credUI),
                  static_cast<int>(GetState()));
    return S_OK;
}

STDMETHODIMP FaceLoginCredential::SetDeselected() {
    EnterCriticalSection(&m_cs);
    m_deselected = true;
    m_pendingAutomaticResume = false;
    m_statusOverlayAllowed = false;
    const State state = m_state;
    LeaveCriticalSection(&m_cs);

    m_statusOverlay.Destroy(L"tile_deselected");

    if (m_pProvider) {
        m_pProvider->CancelAutomaticResume(m_pProvider->GetLoginEntryGeneration());
    }
    FACELOGIN_INFO(L"=== SetDeselected called (state=%d) ===", static_cast<int>(state));

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
    if (state == State::Authenticating) {
        FACELOGIN_INFO(L"SetDeselected: cancelling in-flight auth (user switched tiles)");
        CancelActiveAttempt(true);
    }
    StopInputDetectionThread();
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
                  dwFieldID, static_cast<int>(GetState()));

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
                  dwFieldID, static_cast<int>(GetState()));
    *ppwsz = nullptr;

    switch (dwFieldID) {
    case 0: // Label
        return SHStrDupW(Text("credential.title", L"人脸登录").c_str(), ppwsz);

    case 1: // Status
        {
        const std::wstring status = VisibleStatusText();
        return SHStrDupW(status.c_str(), ppwsz);
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
        EnterCriticalSection(&m_cs);
        m_statusOverlayAllowed = false;
        LeaveCriticalSection(&m_cs);
        m_statusOverlay.Destroy(L"switch_to_password");
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

    const State initialState = GetState();
    FACELOGIN_INFO(L"=== GetSerialization ENTER (state=%d) ===", static_cast<int>(initialState));

    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;

    ZeroMemory(pcpcs, sizeof(*pcpcs));

    // Submitted: the credential was already handed over ("finished") and no
    // re-submission is allowed — LSA already judged it (LogonUI error page
    // after a blank-password rejection, or success). Ending without a new
    // credential lets LogonUI settle on its own error/success UI instead of
    // starting another auth round.
    if (initialState == State::Submitted) {
        FACELOGIN_INFO(L"GetSerialization: credential already submitted — ending without re-submit");
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // Unlock scenario: if we're in Waiting state, the background input-
    // detection thread is still waiting for user input. Return "not
    // finished" — no credentials yet.
    if (initialState == State::Waiting) {
        FACELOGIN_INFO(L"GetSerialization: still Waiting for user input");
        return S_OK;
    }

    // Blocked (passwordless account): show the notice, finish without
    // submitting credentials so the tile never hangs.
    if (initialState == State::Blocked) {
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // If we're in Error state, service is not available — don't block login
    if (initialState == State::Error) {
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // Auth failed earlier — don't retry, let user use password
    if (initialState == State::Failed) {
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // Terminal responses are parsed only by OnPipeResponse on the pipe read
    // thread. GetSerialization is deliberately a state consumer now.
    ULONGLONG deadline = 0;
    EnterCriticalSection(&m_cs);
    deadline = m_authDeadlineTick;
    LeaveCriticalSection(&m_cs);
    if (initialState == State::Authenticating && deadline != 0 &&
        GetTickCount64() >= deadline) {
        FACELOGIN_WARN(L"Auth timed out waiting for service response");
        const std::wstring timeoutStatus =
            Text("credential.noFace", L"未识别到人脸，请重试或使用密码登录");
        EnterCriticalSection(&m_cs);
        if (m_state == State::Authenticating) {
            m_statusText = timeoutStatus;
            m_autoSubmitEligible = false;
            m_state = State::Failed;
        }
        LeaveCriticalSection(&m_cs);
        NotifyCurrentStatus();
        CancelActiveAttempt(false);
        bool canWaitForRetry = false;
        EnterCriticalSection(&m_cs);
        canWaitForRetry = m_state == State::Failed && m_pCredentialEvents2 != nullptr;
        LeaveCriticalSection(&m_cs);
        if (canWaitForRetry) {
            FACELOGIN_INFO(L"Local auth timeout: waiting for a new input to retry");
            StartInputDetectionThread();
        }
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // If we have credentials ready, pack and return them. A still-empty
    // password is VALID here: it means the record is passwordless (blank
    // password account) and we pack a blank MSV1_0 credential — Windows
    // allows blank-password console logon by default, so face unlock works.
    if (GetState() == State::Ready) {
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
            EnterCriticalSection(&m_cs);
            m_state = State::Submitted;
            m_statusText = Text("credential.submitted", L"正在验证登录，等待 Windows 确认...");
            LeaveCriticalSection(&m_cs);
            // Push the status text NOW so the stale "人脸识别成功，正在解锁..."
            // (pulled by LogonUI while Ready) is replaced before the LSA
            // rejection error page hides the shell — LogonUI does not re-pull
            // the string once the error page is up.
            NotifyCurrentStatus();
            FACELOGIN_INFO(L"PackCred SUCCESS: cbSerialization=%lu, ulAuthPackage=%lu",
                          pcpcs->cbSerialization, pcpcs->ulAuthenticationPackage);
        } else {
            FACELOGIN_ERROR(L"PackCred FAILED: hr=0x%08X", hr);
        }
        return hr;
    }

    // Not ready yet. Background callbacks own normal completion and timeout;
    // this remains only as a defensive state-consumer path.
    FACELOGIN_INFO(L"=== GetSerialization EXIT: not ready (state=%d, response=%d) ===",
                  static_cast<int>(GetState()), static_cast<int>(*pcpgsr));
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
                  ntsStatus, ntsSubstatus, static_cast<int>(GetState()));

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
        const std::wstring status = L"登录被拒绝（密码或策略原因），请使用 PIN/密码登录";
        EnterCriticalSection(&m_cs);
        m_state = State::Failed;
        m_autoSubmitEligible = false;
        m_statusText = status;
        SecureZeroMemory(m_password.data(), m_password.size() * sizeof(wchar_t));
        m_password.clear();
        LeaveCriticalSection(&m_cs);
        NotifyCurrentStatus();
        CancelActiveAttempt(false);

        bool canWaitForRetry = false;
        EnterCriticalSection(&m_cs);
        canWaitForRetry = m_pCredentialEvents2 != nullptr && !m_deselected &&
                          m_state == State::Failed;
        LeaveCriticalSection(&m_cs);
        if (canWaitForRetry) {
            FACELOGIN_INFO(L"ReportResult: submission rejected — waiting for new input to retry");
            StartInputDetectionThread();
        }
    }

    return S_OK;
}

// ============================================================================
// Private: asynchronous authentication startup
// ============================================================================

bool FaceLoginCredential::StartAuthAsync(
    AuthTrigger trigger, InputActivationId expectedInputActivationId) {
    JoinAuthConnectThread();
    if (!m_hAuthStop) return false;

    std::shared_ptr<facelogin::PipeClient> client =
        std::make_shared<facelogin::PipeClient>();
    AuthAttemptId attemptId = 0;
    ResetEvent(m_hAuthStop);
    EnterCriticalSection(&m_cs);
    const bool inputActivationValid =
        expectedInputActivationId == 0 ||
        (m_inputDetectionEnabled && m_inputThreadRunning &&
         m_inputActivationId == expectedInputActivationId);
    if (m_state != State::Waiting || m_authThreadRunning || m_deselected ||
        !inputActivationValid) {
        const State rejectedState = m_state;
        const bool authRunning = m_authThreadRunning;
        const bool deselected = m_deselected;
        const auto currentActivationId = m_inputActivationId;
        LeaveCriticalSection(&m_cs);
        FACELOGIN_INFO(L"AuthAttempt: start rejected state=%d authRunning=%d "
                       L"deselected=%d activation=%llu expected=%llu",
                       static_cast<int>(rejectedState),
                       static_cast<int>(authRunning),
                       static_cast<int>(deselected),
                       currentActivationId,
                       expectedInputActivationId);
        return false;
    }
    attemptId = ++m_nextAttemptId;
    m_activeAttemptId = attemptId;
    m_authTrigger = trigger;
    m_autoSubmitEligible = false;
    m_state = State::Authenticating;
    if (trigger == AuthTrigger::LoginEntryAutomatic) {
        m_statusOverlayAllowed = true;
    }
    m_statusOverlayTone = facelogin::StatusOverlayTone::Progress;
    m_noMatchFailed = false;
    m_authDeadlineTick = GetTickCount64() + 20000ULL;
    SecureZeroMemory(m_password.data(), m_password.size() * sizeof(wchar_t));
    m_sid.clear();
    m_upn.clear();
    m_domain.clear();
    m_username.clear();
    m_password.clear();
    m_statusText = Text("credential.recognizing", L"识别中...");
    m_pipeClient = client;
    m_authThreadRunning = true;
    LeaveCriticalSection(&m_cs);

    // Recognition starts asynchronously.  Push the state directly into the
    // active tile instead of re-enumerating every provider and risking a
    // focus change while the user is on the logon screen.
    NotifyCurrentStatus();

    auto* ctx = new (std::nothrow) AuthConnectContext{this, client, attemptId};
    if (!ctx) {
        const std::wstring unavailable =
            Text("credential.serviceUnavailable", L"人脸登录服务不可用");
        EnterCriticalSection(&m_cs);
        m_authThreadRunning = false;
        m_authDeadlineTick = 0;
        m_state = State::Error;
        m_statusText = unavailable;
        m_pipeClient.reset();
        LeaveCriticalSection(&m_cs);
        NotifyCurrentStatus();
        return false;
    }

    AddRef();
    unsigned threadId = 0;
    HANDLE thread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, AuthConnectThreadProc, ctx, 0, &threadId));
    if (!thread || thread == INVALID_HANDLE_VALUE) {
        delete ctx;
        Release();
        const std::wstring unavailable =
            Text("credential.serviceUnavailable", L"人脸登录服务不可用");
        EnterCriticalSection(&m_cs);
        m_authThreadRunning = false;
        m_authDeadlineTick = 0;
        m_state = State::Error;
        m_statusText = unavailable;
        m_pipeClient.reset();
        LeaveCriticalSection(&m_cs);
        FACELOGIN_ERROR(L"AuthAttempt: failed to start connection thread error=%lu",
                        GetLastError());
        NotifyCurrentStatus();
        return false;
    }

    EnterCriticalSection(&m_cs);
    m_hAuthThread = thread;
    LeaveCriticalSection(&m_cs);
    FACELOGIN_INFO(L"AuthAttempt: attempt=%llu trigger=%d thread=%u deadline=%llu",
                   attemptId, static_cast<int>(trigger), threadId,
                   GetTickCount64() + 20000ULL);
    return true;
}

void FaceLoginCredential::JoinAuthConnectThread() {
    HANDLE thread = nullptr;
    EnterCriticalSection(&m_cs);
    thread = m_hAuthThread;
    LeaveCriticalSection(&m_cs);
    if (!thread) return;

    if (GetThreadId(thread) == GetCurrentThreadId()) {
        EnterCriticalSection(&m_cs);
        if (m_hAuthThread == thread) {
            CloseHandle(m_hAuthThread);
            m_hAuthThread = nullptr;
        }
        LeaveCriticalSection(&m_cs);
        return;
    }

    const DWORD waitResult = WaitForSingleObject(thread, INFINITE);
    if (waitResult != WAIT_OBJECT_0) {
        FACELOGIN_ERROR(L"AuthAttempt: failed to join connection thread wait=%lu",
                        waitResult);
        return;
    }
    EnterCriticalSection(&m_cs);
    if (m_hAuthThread == thread) {
        CloseHandle(m_hAuthThread);
        m_hAuthThread = nullptr;
    }
    LeaveCriticalSection(&m_cs);
}

// ============================================================================
// Private: StartInputDetectionThread / StopInputDetectionThread
// ============================================================================

void FaceLoginCredential::StartInputDetectionThread() {
    // A previous detector can have exited naturally after observing input,
    // leaving only its completed thread handle behind. Reap that handle before
    // assigning a new one so repeated retries do not leak kernel handles.
    StopInputDetectionThread();

    EnterCriticalSection(&m_cs);
    if (m_deselected || m_inputThreadRunning) {
        const bool alreadyRunning = m_inputThreadRunning;
        LeaveCriticalSection(&m_cs);
        if (alreadyRunning) {
            FACELOGIN_WARN(L"StartInputDetectionThread: thread already running");
        }
        return;
    }

    if (!m_hInputStop) {
        // Manual-reset is essential. An auto-reset event can be consumed by
        // the worker's timed wait before its next non-blocking check, allowing
        // a stale input edge to start authentication during UnAdvise.
        m_hInputStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_hInputStop) {
            LeaveCriticalSection(&m_cs);
            FACELOGIN_ERROR(L"Failed to create input stop event");
            return;
        }
    } else {
        ResetEvent(m_hInputStop);
    }

    const HANDLE stopEvent = m_hInputStop;
    const InputActivationId activationId = ++m_inputActivationId;
    m_inputDetectionEnabled = true;
    m_inputThreadRunning = true;
    LeaveCriticalSection(&m_cs);

    auto* ctx = new (std::nothrow) InputDetectionContext;
    if (!ctx) {
        EnterCriticalSection(&m_cs);
        m_inputDetectionEnabled = false;
        m_inputThreadRunning = false;
        SetEvent(m_hInputStop);
        LeaveCriticalSection(&m_cs);
        FACELOGIN_ERROR(L"Failed to allocate input detection context");
        return;
    }
    ctx->pCred = this;
    ctx->stopEvent = stopEvent;
    ctx->activationId = activationId;
    AddRef();

    unsigned threadId = 0;
    HANDLE thread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, InputDetectionThreadProc, ctx, 0, &threadId));
    EnterCriticalSection(&m_cs);
    m_hInputThread = thread;
    m_inputThreadId = thread ? threadId : 0;
    LeaveCriticalSection(&m_cs);
    if (!thread || thread == INVALID_HANDLE_VALUE) {
        FACELOGIN_ERROR(L"Failed to start input detection thread");
        EnterCriticalSection(&m_cs);
        m_inputDetectionEnabled = false;
        m_inputThreadRunning = false;
        SetEvent(m_hInputStop);
        LeaveCriticalSection(&m_cs);
        delete ctx;
        Release();
    } else {
        FACELOGIN_INFO(L"Input detection thread started (id=%u)", threadId);
    }
}

void FaceLoginCredential::StopInputDetectionThread() {
    EnterCriticalSection(&m_cs);
    // Invalidate the detector before signaling it. The worker may be between
    // GetAsyncKeyState and StartAuthAsync, so the generation check is the
    // definitive cancellation fence rather than the event alone.
    ++m_inputActivationId;
    m_inputDetectionEnabled = false;
    const bool running = m_inputThreadRunning;
    HANDLE thread = m_hInputThread;
    HANDLE stop = m_hInputStop;
    LeaveCriticalSection(&m_cs);
    if (!running && !thread) {
        if (stop) {
            EnterCriticalSection(&m_cs);
            if (m_hInputStop == stop) {
                CloseHandle(m_hInputStop);
                m_hInputStop = nullptr;
            }
            LeaveCriticalSection(&m_cs);
        }
        return;
    }

    FACELOGIN_INFO(L"Stopping input detection thread...");

    // Signal stop
    if (stop) {
        SetEvent(stop);
    }

    // Deterministically wait for the worker; its stop event breaks the poll.
    if (thread) {
        DWORD inputThreadId = 0;
        EnterCriticalSection(&m_cs);
        inputThreadId = m_inputThreadId;
        LeaveCriticalSection(&m_cs);
        if (inputThreadId != 0 && inputThreadId == GetCurrentThreadId()) {
            // The worker holds a COM reference and can be the last releaser.
            // In that case the destructor may run on this thread; there is no
            // thread to join, and waiting here would deadlock the destructor.
            EnterCriticalSection(&m_cs);
            if (m_hInputThread == thread) {
                CloseHandle(m_hInputThread);
                m_hInputThread = nullptr;
                m_inputThreadId = 0;
            }
            if (m_hInputStop == stop && m_hInputStop) {
                CloseHandle(m_hInputStop);
                m_hInputStop = nullptr;
            }
            m_inputThreadRunning = false;
            LeaveCriticalSection(&m_cs);
            return;
        }
        const DWORD waitResult = WaitForSingleObject(thread, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
            FACELOGIN_ERROR(L"Input thread failed to stop (wait=%lu)", waitResult);
            return;
        }
        EnterCriticalSection(&m_cs);
        if (m_hInputThread == thread) {
            CloseHandle(m_hInputThread);
            m_hInputThread = nullptr;
            m_inputThreadId = 0;
        }
        LeaveCriticalSection(&m_cs);
    }

    EnterCriticalSection(&m_cs);
    if (m_hInputStop == stop && m_hInputStop) {
        CloseHandle(m_hInputStop);
        m_hInputStop = nullptr;
    }
    m_inputThreadRunning = false;
    LeaveCriticalSection(&m_cs);
    FACELOGIN_INFO(L"Input detection thread stopped");
}

// ============================================================================
// Private: Credential Packing
// ============================================================================

HRESULT FaceLoginCredential::PackCredentials(
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs) {

    std::wstring domain;
    std::wstring username;
    std::wstring upn;
    std::wstring password;
    EnterCriticalSection(&m_cs);
    domain = m_domain;
    username = m_username;
    upn = m_upn;
    password = m_password;
    LeaveCriticalSection(&m_cs);

    FACELOGIN_INFO(L"Packing credentials for: %s\\%s (UPN=%s)",
                  domain.c_str(), username.c_str(),
                  upn.empty() ? L"<none>" : upn.c_str());

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

    PWSTR pwzPassword = password.empty() ? const_cast<PWSTR>(L"") : password.data();

    // Build the packed user name in the correct format.
    // Local/domain: "DOMAIN\Username" (required by CredPackAuthenticationBuffer)
    // MSA/AAD:      UPN "user@domain.com"
    std::wstring packedUser;
    if (!upn.empty() && upn.find(L'@') != std::wstring::npos) {
        packedUser = upn;
    } else {
        packedUser = domain + L"\\" + username;
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
    SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    EnterCriticalSection(&m_cs);
    SecureZeroMemory(m_password.data(), m_password.size() * sizeof(wchar_t));
    m_password.clear();
    LeaveCriticalSection(&m_cs);

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
    NotifyCredentialsChanged();

    // Also return NO_CREDENTIAL_FINISHED to deselect our tile
    // This causes LogonUI to show other providers
    // (Actually done in GetSerialization via state change)
    EnterCriticalSection(&m_cs);
    m_state = State::Failed;
    LeaveCriticalSection(&m_cs);

    return S_OK;
}

void FaceLoginCredential::OnPipeStatus(AuthAttemptId attemptId,
                                        const std::wstring& message) {
    if (!IsAttemptActive(attemptId)) return;
    const std::wstring localized = LocalizeKey(message);
    if (localized.empty()) {
        FACELOGIN_WARN(L"Status text ignored: no localization for key '%s'", message.c_str());
        return;
    }
    EnterCriticalSection(&m_cs);
    if (m_state != State::Authenticating || m_activeAttemptId != attemptId) {
        LeaveCriticalSection(&m_cs);
        return;
    }
    m_statusText = localized;
    m_statusOverlayTone = OverlayToneForStatusKey(message);
    LeaveCriticalSection(&m_cs);
    if (!IsAttemptActive(attemptId)) return;
    FACELOGIN_INFO(L"Status text updated: %s (attempt=%llu)",
                   localized.c_str(), attemptId);
    NotifyCurrentStatus();
}

std::wstring FaceLoginCredential::LocalizeKey(const std::wstring& key) const {
    // Pipe payloads are locale keys (see ipc::L10N_* in ipc_protocol.h) — the
    // service never embeds display text. LocaleCatalog resolves: active pack
    // → zh-CN pack → empty (the caller's state default then applies), so a
    // key missing from the packs degrades to Chinese, never to a mixed-
    // language tile or a raw key.
    return m_locale.GetWide(facelogin::WideToUtf8(key));
}

void FaceLoginCredential::OnPipeResponse(AuthAttemptId attemptId,
                                         bool success,
                                         const std::wstring& message) {
    // Ignore LATE results: if the auth was cancelled in the meantime
    // (SetDeselected — user switched to another tile), the read thread may
    // still deliver a terminal message after Disconnect. Applying it would
    // overwrite the Waiting state and make a later tile re-selection auto-
    // restart auth (camera flashes on/off while the user just browses tiles).
    if (!IsAttemptActive(attemptId)) {
        FACELOGIN_INFO(L"OnPipeResponse: auth no longer active (state=%d) — ignoring late result",
                       static_cast<int>(GetState()));
        return;
    }

    std::wstring statusToNotify;
    bool notifyChanged = false;
    if (success) {
        auto result = facelogin::ipc::ParseAuthMessage(message);

        if (result.status == facelogin::ipc::AuthResult::Status::Success) {
            FACELOGIN_INFO(L"OnPipeResponse: Auth success: domain=%s, username=%s (SID=%s, UPN=%s)",
                          result.domain.c_str(), result.username.c_str(),
                          result.sid.c_str(), result.upn.c_str());
            // NOTE: the password itself is never logged — only metadata.
            EnterCriticalSection(&m_cs);
            if (m_state != State::Authenticating || m_activeAttemptId != attemptId) {
                LeaveCriticalSection(&m_cs);
                return;
            }
            m_sid = result.sid;
            m_upn = result.upn;
            m_domain = result.domain;
            m_username = result.username;
            m_password = result.password;
            m_authDeadlineTick = 0;
            m_autoSubmitEligible =
                m_authTrigger == AuthTrigger::LoginEntryAutomatic;
            m_state = State::Ready;
            LeaveCriticalSection(&m_cs);
            FACELOGIN_INFO(L"AutoSubmit: attempt=%llu state=Ready eligible=%d",
                           attemptId, static_cast<int>(IsAutoSubmitReady()));
            SetEvent(m_hCredsReady);
            // Ready credentials are the only state that needs a provider
            // re-enumeration: it asks LogonUI to consume our default tile for
            // automatic submission.  All other state changes update the
            // existing tile in place through ICredentialProviderCredentialEvents2.
            NotifyCurrentStatus();
            TriggerReEnumeration();
            return;
        } else if (result.status == facelogin::ipc::AuthResult::Status::Timeout) {
            FACELOGIN_INFO(L"OnPipeResponse: Auth timeout");
            statusToNotify =
                Text("credential.noFace", L"未识别到人脸，请重试或使用密码登录");
            EnterCriticalSection(&m_cs);
            m_statusText = statusToNotify;
            m_noMatchFailed = false;
            m_authDeadlineTick = 0;
            m_autoSubmitEligible = false;
            m_state = State::Failed;
            LeaveCriticalSection(&m_cs);
            notifyChanged = true;
        } else if (result.status == facelogin::ipc::AuthResult::Status::PoseTimeout) {
            FACELOGIN_INFO(L"OnPipeResponse: Auth timed out without a legal head pose");
            statusToNotify = LocalizeKey(facelogin::ipc::L10N_POSE_TIMEOUT);
            EnterCriticalSection(&m_cs);
            m_statusText = statusToNotify;
            m_noMatchFailed = false;
            m_authDeadlineTick = 0;
            m_autoSubmitEligible = false;
            m_state = State::Failed;
            LeaveCriticalSection(&m_cs);
            notifyChanged = true;
        } else if (result.status == facelogin::ipc::AuthResult::Status::NoMatch) {
            // A face was seen but did not match any enrolled face. Show the
            // specific wording (and keep it — the timeout case below shows the
            // generic "未识别到人脸" instead).
            FACELOGIN_INFO(L"OnPipeResponse: Face present but no match");
            statusToNotify = Text("credential.noMatch", L"人脸匹配失败，请重试或使用密码登录");
            EnterCriticalSection(&m_cs);
            m_statusText = statusToNotify;
            m_noMatchFailed = true;
            m_authDeadlineTick = 0;
            m_autoSubmitEligible = false;
            m_state = State::Failed;
            LeaveCriticalSection(&m_cs);
            notifyChanged = true;
        } else if (result.status == facelogin::ipc::AuthResult::Status::Error) {
            FACELOGIN_WARN(L"OnPipeResponse: Auth error: %s", result.errorMessage.c_str());
            // Passwordless account: show the notice in-place and stop — do NOT
            // trigger re-enumeration or submit any credentials.
            if (result.errorMessage == facelogin::ipc::MSG_PASSWORDLESS_NOTICE) {
                statusToNotify = LocalizeKey(result.errorMessage);
                EnterCriticalSection(&m_cs);
                m_statusText = statusToNotify;
                m_authDeadlineTick = 0;
                m_autoSubmitEligible = false;
                m_state = State::Blocked;
                LeaveCriticalSection(&m_cs);
                NotifyCurrentStatus();
                return;
            }
            // Surface the service's specific error (e.g. the anti-spoof
            // rejection key) on the lock screen instead of the generic
            // "service unavailable".
            if (!result.errorMessage.empty()) {
                statusToNotify = LocalizeKey(result.errorMessage);
            }
            EnterCriticalSection(&m_cs);
            m_authDeadlineTick = 0;
            m_autoSubmitEligible = false;
            if (!statusToNotify.empty()) m_statusText = statusToNotify;
            m_state = State::Error;
            LeaveCriticalSection(&m_cs);
            notifyChanged = true;
        }
    } else {
        FACELOGIN_WARN(L"OnPipeResponse: Read failed — server disconnected?");
        statusToNotify =
            Text("credential.serviceUnavailable", L"人脸登录服务不可用");
        EnterCriticalSection(&m_cs);
        m_statusText = statusToNotify;
        m_authDeadlineTick = 0;
        m_autoSubmitEligible = false;
        m_state = State::Error;
        LeaveCriticalSection(&m_cs);
        notifyChanged = true;
    }
    if (notifyChanged) NotifyCurrentStatus();
    if (notifyChanged) {
        bool canWaitForRetry = false;
        EnterCriticalSection(&m_cs);
        canWaitForRetry = (m_state == State::Failed || m_state == State::Error) &&
                          m_pCredentialEvents2 != nullptr && !m_deselected;
        LeaveCriticalSection(&m_cs);
        if (canWaitForRetry) StartInputDetectionThread();
    }
}

// ============================================================================
// Private: Trigger Re-enumeration
// ============================================================================

void FaceLoginCredential::TriggerReEnumeration() {
    FACELOGIN_DEBUG(L"Triggering CredentialsChanged");
    NotifyCredentialsChanged();
}
