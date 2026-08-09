#include "account_identity.h"
#include "logger.h"

#include <vector>
#include <sstream>

namespace facelogin {

namespace {

// The MicrosoftAccount well-known authority. S-1-11-96-<sub-auth> is the
// "shadow SID" Windows puts into the logon token's group list when a local
// account is linked to (or otherwise associated with) a Microsoft account.
// The identifier authority is {0,0,0,0,0,16} and the first sub-authority is
// 96. (Not declared in the SDK headers this project builds against, so it is
// matched structurally rather than via a named constant.)
const BYTE kMicrosoftAccountAuthority[6] = {0, 0, 0, 0, 0, 16};  // SECURITY_MSA_AUTHORITY
const DWORD kMicrosoftAccountRid = 96;

// True when `pSid` is a MicrosoftAccount shadow SID (S-1-11-96-...).
bool IsMicrosoftAccountSid(PSID pSid) {
    if (!IsValidSid(pSid)) return false;
    SID_IDENTIFIER_AUTHORITY* auth = GetSidIdentifierAuthority(pSid);
    if (!auth) return false;
    if (memcmp(auth->Value, kMicrosoftAccountAuthority, 6) != 0) return false;
    return GetSidSubAuthorityCount(pSid) != nullptr &&
           *GetSidSubAuthorityCount(pSid) >= 1 &&
           *GetSidSubAuthority(pSid, 0) == kMicrosoftAccountRid;
}

// Best-effort query of GetUserNameExW(NameUserPrincipal). Returns empty when
// the session has no UPN (local accounts → ERROR_NONE_MAPPED / NO_SUCH_DOMAIN).
std::wstring QueryUserNamePrincipal() {
    std::wstring upn;
    HMODULE hSecur32 = LoadLibraryW(L"secur32.dll");
    if (!hSecur32) {
        FACELOGIN_WARN(L"QueryUserNamePrincipal: LoadLibrary(secur32) failed (err=%lu)",
                       GetLastError());
        return upn;
    }
    typedef BOOLEAN (WINAPI *PFN_GetUserNameExW)(int, LPWSTR, PULONG);
    auto pfn = reinterpret_cast<PFN_GetUserNameExW>(
        GetProcAddress(hSecur32, "GetUserNameExW"));
    if (pfn) {
        // GetUserNameExW returns ERROR_INSUFFICIENT_BUFFER (122) and writes the
        // REQUIRED size (in wchar, INCLUDING the terminator) back into *upnSize
        // when the buffer is too small. A single 256-char probe would silently
        // drop over-long UPNs, mislabeling the account as local. Retry once
        // with the required size.
        ULONG upnSize = 256;
        std::vector<wchar_t> upnBuf(upnSize);
        if (!pfn(8 /* NameUserPrincipal */, upnBuf.data(), &upnSize)) {
            DWORD err = GetLastError();
            if (err == ERROR_INSUFFICIENT_BUFFER && upnSize > 0) {
                upnBuf.resize(upnSize);
                if (pfn(8 /* NameUserPrincipal */, upnBuf.data(), &upnSize)) {
                    upn = upnBuf.data();
                } else {
                    FACELOGIN_WARN(L"QueryUserNamePrincipal: retry failed (err=%lu)", GetLastError());
                }
            } else {
                // Local accounts / linked-MSA-under-local: no UPN.
                FACELOGIN_DEBUG(L"QueryUserNamePrincipal: no UPN (err=%lu)", err);
            }
        } else {
            upn = upnBuf.data();
        }
    }
    FreeLibrary(hSecur32);
    return upn;
}

// Translate a MicrosoftAccount shadow SID to the MSA email.
// LookupAccountSidW returns "<account>\<email>", e.g.
// "MicrosoftAccount\user@outlook.com". Returns just the email, or empty if
// the lookup fails.
std::wstring TranslateMicrosoftAccountSid(PSID pSid) {
    DWORD nameSize = 0, domainSize = 0;
    SID_NAME_USE use;
    LookupAccountSidW(nullptr, pSid, nullptr, &nameSize, nullptr, &domainSize, &use);
    if (nameSize == 0) {
        FACELOGIN_WARN(L"TranslateMicrosoftAccountSid: LookupAccountSidW size probe failed (err=%lu)",
                       GetLastError());
        return L"";
    }
    std::vector<wchar_t> name(nameSize);
    std::vector<wchar_t> domain(domainSize > 0 ? domainSize : 1);
    if (!LookupAccountSidW(nullptr, pSid, name.data(), &nameSize,
                           domain.data(), &domainSize, &use)) {
        FACELOGIN_WARN(L"TranslateMicrosoftAccountSid: LookupAccountSidW failed (err=%lu)",
                       GetLastError());
        return L"";
    }
    std::wstring account = name.data();
    // Account name may be the bare email, or "MicrosoftAccount\email".
    size_t slash = account.find(L'\\');
    std::wstring email = (slash == std::wstring::npos) ? account : account.substr(slash + 1);
    if (email.empty()) return L"";
    if (email.find(L'@') == std::wstring::npos) {
        // Not an email-shaped name — reject rather than mislabel.
        FACELOGIN_WARN(L"TranslateMicrosoftAccountSid: translated name not an email: '%s'",
                       email.c_str());
        return L"";
    }
    return email;
}

// Look for a MicrosoftAccount shadow SID in the process token's group list.
// Returns the MSA email when found, empty otherwise.
std::wstring FindMsaShadowSidInToken() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        FACELOGIN_WARN(L"FindMsaShadowSidInToken: OpenProcessToken failed (err=%lu)", GetLastError());
        return L"";
    }

    std::wstring email;
    DWORD sz = 0;
    GetTokenInformation(hToken, TokenGroups, nullptr, 0, &sz);
    if (sz > 0) {
        std::vector<BYTE> buf(sz);
        if (GetTokenInformation(hToken, TokenGroups, buf.data(), sz, &sz)) {
            auto* groups = reinterpret_cast<TOKEN_GROUPS*>(buf.data());
            for (DWORD i = 0; i < groups->GroupCount && email.empty(); i++) {
                if (IsMicrosoftAccountSid(groups->Groups[i].Sid)) {
                    email = TranslateMicrosoftAccountSid(groups->Groups[i].Sid);
                    if (!email.empty()) {
                        FACELOGIN_INFO(L"FindMsaShadowSidInToken: MSA shadow SID -> email '%s'",
                                       email.c_str());
                    } else {
                        FACELOGIN_WARN(L"FindMsaShadowSidInToken: MSA shadow SID present but "
                                       L"could not translate to an email");
                    }
                }
            }
        } else {
            FACELOGIN_WARN(L"FindMsaShadowSidInToken: GetTokenInformation(TokenGroups) failed (err=%lu)",
                           GetLastError());
        }
    }
    CloseHandle(hToken);
    return email;
}

} // namespace

bool GetLinkedAccountUpn(std::wstring& outUpn) {
    outUpn.clear();

    // 1) Authoritative: a true MSA direct logon session reports its UPN.
    std::wstring upn = QueryUserNamePrincipal();
    if (!upn.empty()) {
        outUpn = upn;
        FACELOGIN_INFO(L"GetLinkedAccountUpn: direct UPN '%s'", outUpn.c_str());
        return true;
    }

    // 2) Linked MSA: the token's group list carries a MicrosoftAccount shadow
    //    SID (S-1-11-96-...). Translate it back to the email.
    std::wstring shadowEmail = FindMsaShadowSidInToken();
    if (!shadowEmail.empty()) {
        outUpn = shadowEmail;
        FACELOGIN_INFO(L"GetLinkedAccountUpn: linked MSA email '%s'", outUpn.c_str());
        return true;
    }

    // 3) Plain local account.
    FACELOGIN_DEBUG(L"GetLinkedAccountUpn: no MSA identity in session — local account");
    return false;
}

bool IsMsaSession() {
    std::wstring upn;
    return GetLinkedAccountUpn(upn);
}

} // namespace facelogin
