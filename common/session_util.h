#pragma once

#include <windows.h>
namespace facelogin {

// A login-entry generation is an explicit, service-owned authorization for
// one automatic login attempt. It is intentionally independent from LogonUI's
// transient WTS state and is shared with the credential provider through the
// machine registry under a named mutex.
struct KernelBootRegistration {
    bool observed = false;
    bool seeded = false;
    bool createdLoginEntry = false;
    ULONGLONG generation = 0;
};

// Register the newest Kernel-Boot event record. The first observed record is
// used only as a baseline, so installing/updating the service during an
// already active desktop never starts an unsolicited camera session. If a
// BootType 0/1 event is seen before the console session exists, it remains
// pending for the service's subsequent WTS_SESSION_LOGON retry.
KernelBootRegistration RegisterKernelBootEvidence(
    ULONGLONG recordId, DWORD sessionId, bool createsLoginEntry);

// A console logoff is also a new no-user login entry. Its bootRecordId is 0.
ULONGLONG BeginLoginEntryGeneration(DWORD sessionId,
                                    ULONGLONG bootRecordId = 0);
void CompleteLoginEntryGeneration(DWORD sessionId);
ULONGLONG GetLoginEntryGeneration();
ULONGLONG GetAutoAttemptGeneration();
bool IsLoginEntryPending(ULONGLONG generation, DWORD sessionId);
bool TryClaimAutomaticLoginEntry(ULONGLONG generation, DWORD sessionId);

} // namespace facelogin
