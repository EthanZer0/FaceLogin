#pragma once

#include <windows.h>

namespace facelogin {

// Kernel-Boot event 27 records how the current kernel session was entered.
// It is OS-produced evidence, unlike WTS session flags which describe a
// LogonUI session that may already be transitioning while providers load.
enum class KernelBootKind {
    Unknown,
    FullStartup,
    FastStartup,
    HibernateResume,
};

struct KernelBootEvidence {
    bool valid = false;
    DWORD error = ERROR_SUCCESS;
    ULONGLONG recordId = 0;
    DWORD bootType = 0;
    KernelBootKind kind = KernelBootKind::Unknown;
};

// Reads only the newest Microsoft-Windows-Kernel-Boot/Event 27 entry from the
// System channel. This function never subscribes to, clears, or modifies the
// Windows event log.
KernelBootEvidence QueryLatestKernelBootEvidence();
const wchar_t* KernelBootKindName(KernelBootKind kind);

} // namespace facelogin
