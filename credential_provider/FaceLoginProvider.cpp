#include "FaceLoginProvider.h"
#include "FaceLoginCredential.h"
#include "../common/logger.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include "../common/locale_util.h"
#include "../common/session_util.h"
#include <dsrole.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <fstream>
#include <cstdint>

#pragma comment(lib, "credui.lib")
#pragma comment(lib, "advapi32.lib")

// External GUID defined in resource.h / initguid
extern const GUID CLSID_FaceLoginProvider;

FaceLoginProvider::FaceLoginProvider() {
    FACELOGIN_INFO(L"FaceLoginProvider created");

    const std::wstring installDir = ReadRegString(REGVAL_INSTALL_PATH, L"");
    const std::string uiLang = facelogin::LoadConfig(installDir).ui_language;
    facelogin::LocaleCatalog locale;
    const bool localeOk = locale.Load(installDir, uiLang);
    FACELOGIN_INFO(L"[l10n] Provider: installDir='%ls' ui_language='%hs' locale='%hs' loadOk=%d",
                   installDir.c_str(), uiLang.c_str(), locale.locale().c_str(), localeOk);
    m_fieldLabels[0] = locale.GetWide("credential.title", L"人脸登录");
    m_fieldLabels[1] = locale.GetWide("credential.field.status", L"状态");
    m_fieldLabels[2] = locale.GetWide("credential.field.submit", L"提交");
    m_fieldLabels[3] = locale.GetWide("credential.switchToPassword", L"切换到密码登录");

    // Define fields for our credential tile (no tile image — text only)

    // Field 0: Large text ("人脸登录")
    m_rgFieldDescriptors[0].dwFieldID = 0;
    m_rgFieldDescriptors[0].cpft = CPFT_LARGE_TEXT;
    m_rgFieldDescriptors[0].pszLabel = m_fieldLabels[0].data();
    m_rgFieldDescriptors[0].guidFieldType = GUID_NULL;

    // Field 1: Small text (status message)
    m_rgFieldDescriptors[1].dwFieldID = 1;
    m_rgFieldDescriptors[1].cpft = CPFT_SMALL_TEXT;
    m_rgFieldDescriptors[1].pszLabel = m_fieldLabels[1].data();
    m_rgFieldDescriptors[1].guidFieldType = GUID_NULL;

    // Field 2: Submit button (hidden, auto-logon handles submission)
    m_rgFieldDescriptors[2].dwFieldID = 2;
    m_rgFieldDescriptors[2].cpft = CPFT_SUBMIT_BUTTON;
    m_rgFieldDescriptors[2].pszLabel = m_fieldLabels[2].data();
    m_rgFieldDescriptors[2].guidFieldType = GUID_NULL;

    // Field 3: Command link (switch to password)
    m_rgFieldDescriptors[3].dwFieldID = 3;
    m_rgFieldDescriptors[3].cpft = CPFT_COMMAND_LINK;
    m_rgFieldDescriptors[3].pszLabel = m_fieldLabels[3].data();
    m_rgFieldDescriptors[3].guidFieldType = GUID_NULL;
}

FaceLoginProvider::~FaceLoginProvider() {
    FACELOGIN_INFO(L"FaceLoginProvider destroyed");
    if (m_pCredential) {
        m_pCredential->Release();
        m_pCredential = nullptr;
    }
}

// ============================================================================
// Helper: check if any enrolled users exist
// ============================================================================

static DWORD ReadUserCountFromDatabase() {
    // Build path: %PROGRAMDATA%\FaceLogin\data\users.dat
    // Fall back to the registry DataPath if set
    wchar_t programData[MAX_PATH];
    std::wstring dataDir;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
        dataDir = std::wstring(programData) + L"\\FaceLogin";
    } else {
        dataDir = L"C:\\ProgramData\\FaceLogin";
    }
    // Registry DataPath may override
    std::wstring regPath = ReadRegString(REGVAL_DATA_PATH, L"");
    if (!regPath.empty()) {
        dataDir = regPath;
    }
    std::wstring filePath = dataDir + L"\\data\\users.dat";

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return 0;  // No database → no users
    }

    // Read header: magic (4), version (4), count (4)
    uint32_t magic = 0, version = 0, count = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    // Accept v1..v5 databases. The header fields this function reads
    // (magic / version / count) are identical across all versions —
    // v2 added SID/UPN fields, v3 made the embedding length-prefixed,
    // v4 added the per-account faces array, v5 (1.6.0) is a version bump
    // marking new-alignment embeddings — but none changes the header layout.
    if (magic != 0x474F4C46 || (version < 1 || version > 5)) {  // "FLOG"
        return 0;  // Invalid database → treat as no users
    }

    return count;
}

// ============================================================================
// SetUsageScenario
// ============================================================================

// ============================================================================
// IUnknown
// ============================================================================

STDMETHODIMP FaceLoginProvider::QueryInterface(REFIID riid, void** ppv) {
    *ppv = nullptr;
    HRESULT hr = E_NOINTERFACE;

    if (riid == IID_IUnknown || riid == IID_ICredentialProvider) {
        *ppv = static_cast<ICredentialProvider*>(this);
        AddRef();
        hr = S_OK;
    }

    return hr;
}

STDMETHODIMP_(ULONG) FaceLoginProvider::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) FaceLoginProvider::Release() {
    LONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// ICredentialProvider
// ============================================================================

STDMETHODIMP FaceLoginProvider::SetUsageScenario(
    CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD dwFlags) {
    FACELOGIN_INFO(L"SetUsageScenario: cpus=%d, flags=0x%08X", cpus, dwFlags);
    m_cpus = cpus;

    // CPUS_CHANGE_PASSWORD: we don't support changing passwords via face
    // recognition. Let the built-in password provider handle this.
    if (cpus == CPUS_CHANGE_PASSWORD) {
        FACELOGIN_INFO(L"SetUsageScenario: CPUS_CHANGE_PASSWORD — delegating to password provider");
        return E_NOTIMPL;
    }

    // CPUS_CREDUI / CPUS_PLAP: Windows Security dialog popped up from within
    // an active user session (e.g. PIN change, fingerprint enrollment, Edge
    // password viewing, etc.).  These dialogs are multi-step workflows that
    // don't work reliably with our face recognition flow — some dismiss
    // prematurely, others don't accept the credential format.  Always delegate
    // to the built-in password/pin providers.
    if (cpus == CPUS_CREDUI || cpus == CPUS_PLAP) {
        FACELOGIN_INFO(L"SetUsageScenario: CPUS_CREDUI/CPUS_PLAP — delegating to password provider");
        return E_NOTIMPL;
    }

    // Windows 10+ can report CPUS_LOGON for both initial logon and unlock.
    // Only a service-owned Kernel-Boot/logoff generation authorizes automatic
    // recognition. The provider deliberately does not inspect WTS user or
    // lock state: those values race LogonUI during startup.
    m_loginEntryGeneration = facelogin::GetLoginEntryGeneration();
    m_loginEntrySessionId = WTSGetActiveConsoleSessionId();
    const bool generationPending = facelogin::IsLoginEntryPending(
        m_loginEntryGeneration, m_loginEntrySessionId);
    m_isLoginEntry = cpus == CPUS_LOGON && generationPending;

    FACELOGIN_INFO(L"LoginEntry: cpus=%d sessionId=%lu generation=%llu "
                   L"autoGeneration=%llu pending=%d decision=%s",
                   cpus, m_loginEntrySessionId,
                   m_loginEntryGeneration,
                   facelogin::GetAutoAttemptGeneration(),
                   static_cast<int>(generationPending),
                   m_isLoginEntry ? L"automatic-entry" : L"key-triggered-unlock");

    // Check if the Disabled registry flag is set
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers\\{B8F4C7A1-3D5E-4F2B-A9C6-1D8E7F3A5B2C}",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD disabled = 0, size = sizeof(disabled);
        RegQueryValueExW(hKey, L"Disabled", nullptr, nullptr,
                        reinterpret_cast<LPBYTE>(&disabled), &size);
        RegCloseKey(hKey);
        if (disabled) {
            FACELOGIN_INFO(L"Provider is disabled via registry");
            return E_NOTIMPL;  // This will cause LogonUI to skip this provider
        }
    }

    // Check if any users have been enrolled. If not, hide the face login
    // tile entirely — no point showing it to a first-time user.
    DWORD userCount = ReadUserCountFromDatabase();
    FACELOGIN_INFO(L"User count from database: %lu", userCount);
    if (userCount == 0) {
        FACELOGIN_INFO(L"No enrolled users — hiding face login tile");
        return E_NOTIMPL;
    }

    // Create our credential
    m_pCredential = new FaceLoginCredential();
    if (!m_pCredential) {
        return E_OUTOFMEMORY;
    }
    m_pCredential->Initialize(this);

    return S_OK;
}

STDMETHODIMP FaceLoginProvider::SetSerialization(
    const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs) {
    // We don't handle incoming credential serialization.
    // This is called for things like RDP pre-fill or CredUI callbacks.
    UNREFERENCED_PARAMETER(pcpcs);
    return S_OK;
}

STDMETHODIMP FaceLoginProvider::Advise(
    ICredentialProviderEvents* pcpe, UINT_PTR upAdviseContext) {
    FACELOGIN_INFO(L"Advise called");

    if (m_pEvents) {
        m_pEvents->Release();
    }

    m_pEvents = pcpe;
    m_upAdviseContext = upAdviseContext;

    if (m_pEvents) {
        m_pEvents->AddRef();
    }

    if (m_pCredential) {
        m_pCredential->AdviseProvider(m_pEvents, upAdviseContext);
    }

    return S_OK;
}

STDMETHODIMP FaceLoginProvider::UnAdvise() {
    FACELOGIN_INFO(L"UnAdvise called");

    if (m_pEvents) {
        m_pEvents->Release();
        m_pEvents = nullptr;
    }

    if (m_pCredential) {
        m_pCredential->UnadviseProvider();
    }

    return S_OK;
}

STDMETHODIMP FaceLoginProvider::GetFieldDescriptorCount(DWORD* pdwCount) {
    *pdwCount = ARRAYSIZE(m_rgFieldDescriptors);
    return S_OK;
}

STDMETHODIMP FaceLoginProvider::GetFieldDescriptorAt(
    DWORD dwIndex, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** ppcpfd) {

    if (dwIndex >= ARRAYSIZE(m_rgFieldDescriptors)) {
        return E_INVALIDARG;
    }

    // Allocate and copy the field descriptor (replacing FieldDescriptorCoAllocCopy)
    CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR* pcpfd =
        static_cast<CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR*>(
            CoTaskMemAlloc(sizeof(CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR)));
    if (!pcpfd) {
        return E_OUTOFMEMORY;
    }
    *pcpfd = m_rgFieldDescriptors[dwIndex];
    pcpfd->pszLabel = nullptr;
    HRESULT hr = ::SHStrDupW(m_rgFieldDescriptors[dwIndex].pszLabel, &pcpfd->pszLabel);
    if (FAILED(hr)) {
        CoTaskMemFree(pcpfd);
        return hr;
    }
    *ppcpfd = pcpfd;

    return hr;
}

STDMETHODIMP FaceLoginProvider::GetCredentialCount(
    DWORD* pdwCount, DWORD* pdwDefault, BOOL* pbAutoLogonWithDefault) {

    // We always provide exactly one credential
    *pdwCount = 1;
    *pdwDefault = 0;

    // Do not put LogonUI into its auto-submit spinner while recognition is
    // still running. Automatic submission is armed only after credentials are
    // ready; the credential then requests re-enumeration.
    const bool autoSubmitReady = m_pCredential && m_pCredential->IsAutoSubmitReady();
    *pbAutoLogonWithDefault = autoSubmitReady ? TRUE : FALSE;
    FACELOGIN_INFO(L"AutoSubmit: GetCredentialCount autoLogon=%d loginEntry=%d ready=%d",
                   *pbAutoLogonWithDefault, static_cast<int>(m_isLoginEntry),
                   static_cast<int>(autoSubmitReady));

    return S_OK;
}

STDMETHODIMP FaceLoginProvider::GetCredentialAt(
    DWORD dwIndex, ICredentialProviderCredential** ppcpc) {

    if (dwIndex != 0 || !m_pCredential) {
        return E_INVALIDARG;
    }

    return m_pCredential->QueryInterface(IID_ICredentialProviderCredential,
                                         reinterpret_cast<void**>(ppcpc));
}

bool FaceLoginProvider::IsDomainJoined() const {
    PDSROLE_PRIMARY_DOMAIN_INFO_BASIC info = nullptr;
    bool result = false;

    if (DsRoleGetPrimaryDomainInformation(nullptr,
            DsRolePrimaryDomainInfoBasic,
            reinterpret_cast<PBYTE*>(&info)) == ERROR_SUCCESS) {
        result = (info->MachineRole == DsRole_RoleMemberWorkstation ||
                  info->MachineRole == DsRole_RoleMemberServer ||
                  info->MachineRole == DsRole_RoleBackupDomainController ||
                  info->MachineRole == DsRole_RolePrimaryDomainController);
        DsRoleFreeMemory(info);
    }

    return result;
}
