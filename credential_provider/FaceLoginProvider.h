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
//   0: CPFT_LARGE_TEXT — "Face Login"
//   1: CPFT_SMALL_TEXT — Status message
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
    bool IsColdBoot() const { return m_isColdBoot; }
    bool IsCredUI() const { return m_cpus == CPUS_CREDUI || m_cpus == CPUS_PLAP; }

private:
    LONG m_refCount = 1;
    CREDENTIAL_PROVIDER_USAGE_SCENARIO m_cpus = CPUS_LOGON;
    CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR m_rgFieldDescriptors[4];
    ICredentialProviderEvents* m_pEvents = nullptr;
    UINT_PTR m_upAdviseContext = 0;

    // Our credential object (one instance)
    FaceLoginCredential* m_pCredential = nullptr;

    // True = cold boot / first logon (no active user session)
    // False = unlock / switch user (existing user session)
    bool m_isColdBoot = true;

    // Check if we're in a domain-joined environment
    bool IsDomainJoined() const;
};
