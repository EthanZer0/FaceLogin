#include "session_util.h"

#include "registry_util.h"

#include <limits>

namespace facelogin {
namespace {

constexpr wchar_t kLoginEntryMutexName[] =
    L"Global\\FaceLogin.LoginEntryState.v2";

class ScopedLoginEntryMutex {
public:
    ScopedLoginEntryMutex()
        : handle_(CreateMutexW(nullptr, FALSE, kLoginEntryMutexName)) {
        if (handle_) {
            const DWORD wait = WaitForSingleObject(handle_, 5000);
            acquired_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        }
    }

    ~ScopedLoginEntryMutex() {
        if (acquired_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }

    bool acquired() const { return acquired_; }

private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
};

ULONGLONG NextGeneration() {
    const ULONGLONG previous = ReadRegQword(REGVAL_LOGIN_ENTRY_GENERATION, 0);
    return previous == std::numeric_limits<ULONGLONG>::max() ? 1 : previous + 1;
}

ULONGLONG BeginLoginEntryGenerationLocked(DWORD sessionId,
                                          ULONGLONG bootRecordId) {
    if (sessionId == 0xFFFFFFFF) return 0;

    const bool active = ReadRegDword(REGVAL_LOGIN_ENTRY_ACTIVE, 0) != 0;
    const DWORD activeSession = static_cast<DWORD>(
        ReadRegQword(REGVAL_LOGIN_ENTRY_SESSION, 0xFFFFFFFF));
    const ULONGLONG activeBootRecord =
        ReadRegQword(REGVAL_LOGIN_ENTRY_BOOT_RECORD, 0);
    const bool sameEntry = active && activeSession == sessionId &&
        (bootRecordId == 0 || activeBootRecord == 0 ||
         activeBootRecord == bootRecordId);
    if (sameEntry) return ReadRegQword(REGVAL_LOGIN_ENTRY_GENERATION, 0);

    const ULONGLONG generation = NextGeneration();
    if (!WriteRegQword(REGVAL_LOGIN_ENTRY_GENERATION, generation) ||
        !WriteRegQword(REGVAL_LOGIN_ENTRY_SESSION, sessionId) ||
        !WriteRegQword(REGVAL_LOGIN_ENTRY_BOOT_RECORD, bootRecordId) ||
        !WriteRegDword(REGVAL_LOGIN_ENTRY_ACTIVE, 1)) {
        return 0;
    }

    return generation;
}

} // namespace

KernelBootRegistration RegisterKernelBootEvidence(
    ULONGLONG recordId, DWORD sessionId, bool createsLoginEntry) {
    KernelBootRegistration result;
    if (recordId == 0) return result;

    ScopedLoginEntryMutex lock;
    if (!lock.acquired()) return result;

    const ULONGLONG lastRecord = ReadRegQword(REGVAL_LAST_KERNEL_BOOT_RECORD, 0);
    if (lastRecord == recordId) {
        result.observed = true;
        return result;
    }

    // Auto-start evidence needs a concrete console session. Services can start
    // before Winlogon has created it; leave the record unconsumed so the later
    // WTS_SESSION_LOGON notification can register the same Kernel-Boot event.
    if (createsLoginEntry && sessionId == 0xFFFFFFFF) return result;

    if (!WriteRegQword(REGVAL_LAST_KERNEL_BOOT_RECORD, recordId)) return result;
    result.observed = true;

    // A service first installed while Windows is already at the desktop has no
    // prior evidence record. Seed it without auto-starting recognition; the
    // next actual boot/resume will have a different record id.
    if (lastRecord == 0) {
        result.seeded = true;
        return result;
    }
    if (!createsLoginEntry) return result;

    result.generation = BeginLoginEntryGenerationLocked(sessionId, recordId);
    result.createdLoginEntry = result.generation != 0;
    return result;
}

ULONGLONG BeginLoginEntryGeneration(DWORD sessionId, ULONGLONG bootRecordId) {
    ScopedLoginEntryMutex lock;
    if (!lock.acquired()) return 0;
    return BeginLoginEntryGenerationLocked(sessionId, bootRecordId);
}

void CompleteLoginEntryGeneration(DWORD sessionId) {
    if (sessionId == 0xFFFFFFFF) return;

    ScopedLoginEntryMutex lock;
    if (!lock.acquired()) return;
    if (ReadRegDword(REGVAL_LOGIN_ENTRY_ACTIVE, 0) != 0 &&
        ReadRegQword(REGVAL_LOGIN_ENTRY_SESSION, 0xFFFFFFFF) == sessionId) {
        WriteRegDword(REGVAL_LOGIN_ENTRY_ACTIVE, 0);
    }
}

ULONGLONG GetLoginEntryGeneration() {
    return ReadRegQword(REGVAL_LOGIN_ENTRY_GENERATION, 0);
}

ULONGLONG GetAutoAttemptGeneration() {
    return ReadRegQword(REGVAL_AUTO_ATTEMPT_GENERATION, 0);
}

bool IsLoginEntryPending(ULONGLONG generation, DWORD sessionId) {
    if (generation == 0 || sessionId == 0xFFFFFFFF) return false;
    ScopedLoginEntryMutex lock;
    if (!lock.acquired()) return false;
    return ReadRegDword(REGVAL_LOGIN_ENTRY_ACTIVE, 0) != 0 &&
           ReadRegQword(REGVAL_LOGIN_ENTRY_GENERATION, 0) == generation &&
           ReadRegQword(REGVAL_LOGIN_ENTRY_SESSION, 0xFFFFFFFF) == sessionId;
}

bool TryClaimAutomaticLoginEntry(ULONGLONG generation, DWORD sessionId) {
    if (generation == 0 || sessionId == 0xFFFFFFFF) return false;

    ScopedLoginEntryMutex lock;
    if (!lock.acquired()) return false;

    if (ReadRegDword(REGVAL_LOGIN_ENTRY_ACTIVE, 0) == 0 ||
        ReadRegQword(REGVAL_LOGIN_ENTRY_GENERATION, 0) != generation ||
        ReadRegQword(REGVAL_LOGIN_ENTRY_SESSION, 0xFFFFFFFF) != sessionId ||
        ReadRegQword(REGVAL_AUTO_ATTEMPT_GENERATION, 0) == generation) {
        return false;
    }
    return WriteRegQword(REGVAL_AUTO_ATTEMPT_GENERATION, generation);
}

} // namespace facelogin
