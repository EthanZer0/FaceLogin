#pragma once

#include <windows.h>
#include <credentialprovider.h>
#include <string>

// Forward declarations
class FaceLoginCredential;

// ============================================================================
// FaceLoginProvider — ICredentialProvider implementation
//
// Enumerates credential tiles. For face login, we always provide exactly
// one credential tile that supports auto-logon.
//
// Field layout (no tile image):
//   0: CPFT_LARGE_TEXT — Hidden title retained for layout compatibility
//   1: CPFT_SMALL_TEXT — Hidden status fallback if the overlay is unavailable
//   2: CPFT_SUBMIT_BUTTON — Submit (hidden, auto-logon)
//   3: CPFT_COMMAND_LINK — "Switch to password login"
// ============================================================================

class FaceLoginProvider : public ICredentialProvider {
public:
    FaceLoginProvider();
    virtual ~FaceLoginProvider();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ICredentialProvider
    STDMETHODIMP SetUsageScenario(
        CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus,
        DWORD dwFlags) override;

    STDMETHODIMP SetSerialization(
        const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs) override;

    STDMETHODIMP Advise(
        ICredentialProviderEvents* pcpe,
        UINT_PTR upAdviseContext) override;

    STDMETHODIMP UnAdvise() override;

    STDMETHODIMP GetFieldDescriptorCount(
        DWORD* pdwCount) override;

    STDMETHODIMP GetFieldDescriptorAt(
        DWORD dwIndex,
        CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** ppcpfd) override;

    STDMETHODIMP GetCredentialCount(
        DWORD* pdwCount,
        DWORD* pdwDefault,
        BOOL* pbAutoLogonWithDefault) override;

    STDMETHODIMP GetCredentialAt(
        DWORD dwIndex,
        ICredentialProviderCredential** ppcpc) override;

    // Accessors for our credential
    CREDENTIAL_PROVIDER_USAGE_SCENARIO GetUsageScenario() const { return m_cpus; }
    ICredentialProviderEvents* GetEvents() const { return m_pEvents; }
    UINT_PTR GetAdviseContext() const { return m_upAdviseContext; }
    bool IsLoginEntry() const { return m_isLoginEntry; }
    ULONGLONG GetLoginEntryGeneration() const { return m_loginEntryGeneration; }
    DWORD GetLoginEntrySessionId() const { return m_loginEntrySessionId; }
    bool IsCredUI() const { return m_cpus == CPUS_CREDUI || m_cpus == CPUS_PLAP; }

    // LogonUI may rebuild the credential collection while an automatic login
    // attempt is still running. Preserve one in-process continuation across
    // that transient replacement; completed attempts and user tile switches
    // never arm it.
    void ArmAutomaticResume(ULONGLONG generation);
    bool ConsumeAutomaticResume(ULONGLONG generation);
    void CancelAutomaticResume(ULONGLONG generation);

private:
    LONG m_refCount = 1;
    CREDENTIAL_PROVIDER_USAGE_SCENARIO m_cpus = CPUS_LOGON;
    CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR m_rgFieldDescriptors[4];
    // Own localized labels for the lifetime of the field descriptors.
    std::wstring m_fieldLabels[4];
    ICredentialProviderEvents* m_pEvents = nullptr;
    UINT_PTR m_upAdviseContext = 0;

    // Our credential object (one instance)
    FaceLoginCredential* m_pCredential = nullptr;

    // True for an initial/after-logoff console entry with no interactive user.
    // Ordinary lock/unlock remains key-triggered.
    bool m_isLoginEntry = false;
    ULONGLONG m_loginEntryGeneration = 0;
    DWORD m_loginEntrySessionId = 0xFFFFFFFF;

    SRWLOCK m_autoResumeLock = SRWLOCK_INIT;
    ULONGLONG m_autoResumeGeneration = 0;
    bool m_autoResumeAvailable = false;
    bool m_autoResumeConsumed = false;

    // Check if we're in a domain-joined environment
    bool IsDomainJoined() const;
};
