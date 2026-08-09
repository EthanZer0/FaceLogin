#pragma once

#include <windows.h>
#include <string>

namespace facelogin {

// ============================================================================
// Account identity — authoritative MSA/local detection.
//
// The single source of truth for "is the current session a Microsoft account?"
// Shared by all three executables (Console / Service / Credential Provider).
//
// Background: Windows exposes a "linked Microsoft account" (a local account
// bound to an MSA, e.g. via Settings → Accounts) WITHOUT converting the local
// SID. The local SID stays S-1-5-21-..., and the MSA's shadow SID is added to
// the logon token as a GROUP SID of the form S-1-11-96-<...> (MicrosoftAccount
// well-known authority). GetUserNameExW(NameUserPrincipal) only returns an
// email when the CURRENT logon session is itself an MSA direct logon — for a
// linked/local session it fails (err 1332 / 203), so a detection that only
// looks at GetUserNameExW mislabels linked MSA accounts as "local".
//
// The correct detection must ALSO inspect the token's group SIDs for the
// MicrosoftAccount shadow SID (S-1-11-96-...) and translate it back to the
// email via LookupAccountSidW.
// ============================================================================

// Determine the current process token's Microsoft-account identity.
//
// Returns:
//   true   — the session is a Microsoft account (direct MSA logon, or a local
//            account linked to an MSA). *outUpn receives the MSA email
//            (e.g. "user@outlook.com").
//   false  — the session is a plain local account. *outUpn is cleared.
//
// Order of evidence (authoritative first):
//   1. GetUserNameExW(NameUserPrincipal)  — true MSA direct logon.
//   2. Token group SIDs contain S-1-11-96-* (MicrosoftAccount shadow) →
//      LookupAccountSidW to recover the email.
bool GetLinkedAccountUpn(std::wstring& outUpn);

// Convenience: true when the current session is a Microsoft account
// (direct or linked). Equivalent to GetLinkedAccountUpn() returning true.
bool IsMsaSession();

} // namespace facelogin
