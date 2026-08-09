// Standalone diagnostic for account_identity MSA detection.
// Prints every group SID of the current process token + the MSA decision.
// Build: cl /EHsc /DUNICODE /D_UNICODE diag_msa.cpp
// Also links account_identity.cpp to exercise the real production code path.
#include <windows.h>
#include <sddl.h>
#include <stdio.h>
#include <vector>
#include <string>

// ---- replicate IsMicrosoftAccountSid ----
bool IsMicrosoftAccountSid(PSID pSid) {
    if (!IsValidSid(pSid)) return false;
    SID_IDENTIFIER_AUTHORITY* auth = GetSidIdentifierAuthority(pSid);
    if (!auth) return false;
    static const BYTE kMSA[6] = {0, 0, 0, 0, 0, 11};  // SECURITY_PACKAGE_AUTHORITY (S-1-11)
    if (memcmp(auth->Value, kMSA, 6) != 0) return false;
    return GetSidSubAuthorityCount(pSid) != nullptr &&
           *GetSidSubAuthorityCount(pSid) >= 1 &&
           *GetSidSubAuthority(pSid, 0) == 96;
}

int main() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        printf("OpenProcessToken failed err=%lu\n", GetLastError());
        return 1;
    }
    DWORD sz = 0;
    GetTokenInformation(hToken, TokenGroups, nullptr, 0, &sz);
    if (sz == 0) { printf("GetTokenInformation size=0 err=%lu\n", GetLastError()); return 1; }
    std::vector<BYTE> buf(sz);
    if (!GetTokenInformation(hToken, TokenGroups, buf.data(), sz, &sz)) {
        printf("GetTokenInformation failed err=%lu\n", GetLastError());
        return 1;
    }
    auto* groups = reinterpret_cast<TOKEN_GROUPS*>(buf.data());
    printf("Token user SID + group count: %lu groups\n", groups->GroupCount);

    for (DWORD i = 0; i < groups->GroupCount; i++) {
        PSID sid = groups->Groups[i].Sid;
        LPWSTR sidStr = nullptr;
        ConvertSidToStringSidW(sid, &sidStr);
        wchar_t acct[256] = {}, dom[256] = {};
        DWORD acctSize = 256, domSize = 256;
        SID_NAME_USE use;
        bool okName = LookupAccountSidW(nullptr, sid, acct, &acctSize, dom, &domSize, &use);
        printf("[%02lu] %ls  %s  msaSid=%d%s\n",
               i, sidStr ? sidStr : L"(null)",
               okName ? "name-ok" : "no-name",
               IsMicrosoftAccountSid(sid) ? 1 : 0,
               IsMicrosoftAccountSid(sid) ? L"  <== MSA SHADOW" : L"");
        if (okName) {
            printf("      domain=%ls account=%ls\n", dom, acct);
        }
        if (sidStr) LocalFree(sidStr);
    }
    CloseHandle(hToken);
    return 0;
}
