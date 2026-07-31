#include "FaceLoginProvider.h"
#include "FaceLoginCredential.h"
#include "../common/logger.h"
#include "../common/registry_util.h"
#include <dsrole.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <fstream>
#include <cstdint>

#pragma comment(lib, "credui.lib")
#pragma comment(lib, "advapi32.lib")

// External GUID defined in resource.h / initguid
extern const GUID CLSID_FaceLoginProvider;

FaceLoginProvider::FaceLoginProvider() {
    FACELOGIN_INFO(L"FaceLoginProvider created");

    // Define fields for our credential tile (no tile image — text only)

    // Field 0: Large text ("人脸登录")
    m_rgFieldDescriptors[0].dwFieldID = 0;
    m_rgFieldDescriptors[0].cpft = CPFT_LARGE_TEXT;
    m_rgFieldDescriptors[0].pszLabel = const_cast<LPWSTR>(L"人脸登录");
    m_rgFieldDescriptors[0].guidFieldType = GUID_NULL;

    // Field 1: Small text (status message)
    m_rgFieldDescriptors[1].dwFieldID = 1;
    m_rgFieldDescriptors[1].cpft = CPFT_SMALL_TEXT;
    m_rgFieldDescriptors[1].pszLabel = const_cast<LPWSTR>(L"状态");
    m_rgFieldDescriptors[1].guidFieldType = GUID_NULL;

    // Field 2: Submit button (hidden, auto-logon handles submission)
    m_rgFieldDescriptors[2].dwFieldID = 2;
    m_rgFieldDescriptors[2].cpft = CPFT_SUBMIT_BUTTON;
    m_rgFieldDescriptors[2].pszLabel = const_cast<LPWSTR>(L"提交");
    m_rgFieldDescriptors[2].guidFieldType = GUID_NULL;

    // Field 3: Command link (switch to password)
    m_rgFieldDescriptors[3].dwFieldID = 3;
    m_rgFieldDescriptors[3].cpft = CPFT_COMMAND_LINK;
    m_rgFieldDescriptors[3].pszLabel = const_cast<LPWSTR>(L"切换到密码登录");
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

    // Accept v1 and v2 databases (v2 adds SID/UPN fields)
    if (magic != 0x474F4C46 || (version != 1 && version != 2)) {  // "FLOG"
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

    // ── LOGON / UNLOCK → cold-boot detection ────────────────────────
    // Cold-boot detection via system uptime comparison.
    //
    // The service writes GetTickCount64() to ServiceStartUptime at every startup.
    // GetTickCount64 resets to near-zero on each boot, so comparing the CP's
    // current uptime to the service's recorded uptime tells us whether they are
    // on the same boot cycle.
    //
    // Decision matrix:
    //   serviceUptime == 0          → fallback to UserLoggedIn (service not ready)
    //   currentUptime < serviceUptime → cross-boot: stale registry value from
    //                                   prior boot → definitely cold boot
    //   delta < 120s, UserLoggedIn=0 → cold boot → auto-trigger
    //   delta < 120s, UserLoggedIn=1 → fast-startup resume → manual trigger
    //   delta >= 120s               → unlock (service started long ago) → manual
    ULONGLONG serviceUptime = ReadRegQword(REGVAL_SERVICE_START_UPTIME, 0);
    ULONGLONG currentUptime = GetTickCount64();
    const ULONGLONG COLD_BOOT_THRESHOLD_MS = 120000; // 2 minutes
    DWORD userLoggedIn = ReadRegDword(REGVAL_USER_LOGGED_IN, 0);

    if (serviceUptime == 0) {
        // Service hasn't written ServiceStartUptime yet — fall back to
        // UserLoggedIn only.
        m_isColdBoot = (userLoggedIn == 0);
        FACELOGIN_INFO(L"SetUsageScenario: ServiceStartUptime=0, UserLoggedIn=%lu → coldBoot=%d",
                      userLoggedIn, static_cast<int>(m_isColdBoot));
    } else if (currentUptime < serviceUptime) {
        // Cross-boot: the registry value is from a previous boot (since
        // GetTickCount64 always increases within one boot and resets to
        // near-zero on each new boot).  Current boot is fresh → cold boot.
        m_isColdBoot = true;
        FACELOGIN_INFO(L"SetUsageScenario: cross-boot detected (current=%llu < service=%llu) → coldBoot=true",
                      currentUptime, serviceUptime);
    } else {
        // Same boot: delta tells us how long ago the service started.
        ULONGLONG delta = currentUptime - serviceUptime;
        if (delta < COLD_BOOT_THRESHOLD_MS) {
            // CP and service started close together — could be cold boot
            // or fast-startup resume.  UserLoggedIn disambiguates.
            m_isColdBoot = (userLoggedIn == 0);
            FACELOGIN_INFO(L"SetUsageScenario: delta=%llu < %llums, UserLoggedIn=%lu → coldBoot=%d",
                          delta, COLD_BOOT_THRESHOLD_MS, userLoggedIn,
                          static_cast<int>(m_isColdBoot));
        } else {
            // Far apart: service started long ago → unlock scenario.
            m_isColdBoot = false;
            FACELOGIN_INFO(L"SetUsageScenario: delta=%llu >= %llums → coldBoot=false",
                          delta, COLD_BOOT_THRESHOLD_MS);
        }
    }

    // Clean up NetWkstaUserEnum includes — no longer needed
    // (already removed above)

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

    // ALWAYS set auto-logon so LogonUI selects our tile.
    // The cold-boot vs unlock distinction is handled inside Advise() and
    // SetSelected():
    //   cold boot: auto-logon=TRUE → GetSerialization polled → StartAuth in Advise()
    //   unlock:    auto-logon=FALSE in SetSelected → user must click Submit button
    //              → GetSerialization runs synchronous auth
    *pbAutoLogonWithDefault = m_isColdBoot ? TRUE : FALSE;
    FACELOGIN_INFO(L"GetCredentialCount: count=%d, default=%d, autoLogon=%d, coldBoot=%d",
                  *pdwCount, *pdwDefault, *pbAutoLogonWithDefault, static_cast<int>(m_isColdBoot));

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

// Read the MSA UPN from the IdentityStore registry.  Returns empty
// string if no Microsoft account is associated with this SID.
static std::wstring GetMSAUpnFromIdentityStore(const std::wstring& userSid) {
    std::wstring upn;

    // Path: HKLM\SOFTWARE\Microsoft\IdentityStore\LogonCache\D7F9888F-...\Name2Sid\{hash}
    // This provider GUID is the MicrosoftAccount (MSA) identity provider.
    HKEY hName2Sid = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\IdentityStore\\LogonCache\\D7F9888F-E3FC-49b0-9EA6-A85B5F392A4F\\Name2Sid",
        0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &hName2Sid) == ERROR_SUCCESS) {

        wchar_t hashName[256];
        DWORD idx = 0;
        while (RegEnumKeyW(hName2Sid, idx++, hashName, 256) == ERROR_SUCCESS) {
            HKEY hEntry = nullptr;
            if (RegOpenKeyExW(hName2Sid, hashName, 0, KEY_QUERY_VALUE, &hEntry) == ERROR_SUCCESS) {
                wchar_t identityName[256] = {};
                DWORD nameSize = sizeof(identityName);
                wchar_t sidValue[256] = {};
                DWORD sidSize = sizeof(sidValue);

                RegQueryValueExW(hEntry, L"IdentityName", nullptr, nullptr,
                    reinterpret_cast<LPBYTE>(identityName), &nameSize);
                RegQueryValueExW(hEntry, L"Sid", nullptr, nullptr,
                    reinterpret_cast<LPBYTE>(sidValue), &sidSize);

                if (identityName[0] != L'\0' && wcschr(identityName, L'@')) {
                    upn = identityName;
                    FACELOGIN_INFO(L"MSA UPN found in IdentityStore: %s (SID=%s)",
                                  identityName, sidValue);
                }
                RegCloseKey(hEntry);
                if (!upn.empty()) break;
            }
        }
        RegCloseKey(hName2Sid);
    }
    return upn;
}
