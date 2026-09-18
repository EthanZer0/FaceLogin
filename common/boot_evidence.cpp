#include "boot_evidence.h"

#include <winevt.h>

#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "wevtapi.lib")

namespace facelogin {
namespace {

class ScopedEvtHandle {
public:
    explicit ScopedEvtHandle(EVT_HANDLE handle = nullptr) : handle_(handle) {}
    ~ScopedEvtHandle() { if (handle_) EvtClose(handle_); }

    EVT_HANDLE get() const { return handle_; }
    EVT_HANDLE* put() { return &handle_; }

private:
    EVT_HANDLE handle_ = nullptr;
};

bool ExtractElementValue(const std::wstring& xml, const wchar_t* element,
                         std::wstring& value) {
    const std::wstring open = std::wstring(L"<") + element + L">";
    const std::wstring close = std::wstring(L"</") + element + L">";
    const size_t begin = xml.find(open);
    if (begin == std::wstring::npos) return false;
    const size_t valueBegin = begin + open.size();
    const size_t end = xml.find(close, valueBegin);
    if (end == std::wstring::npos) return false;
    value.assign(xml, valueBegin, end - valueBegin);
    return true;
}

bool ExtractNamedDataValue(const std::wstring& xml, const wchar_t* name,
                           std::wstring& value) {
    const std::wstring single = std::wstring(L"Name='") + name + L"'";
    const std::wstring quoted = std::wstring(L"Name=\"") + name + L"\"";
    size_t attribute = xml.find(single);
    if (attribute == std::wstring::npos) attribute = xml.find(quoted);
    if (attribute == std::wstring::npos) return false;
    const size_t valueBegin = xml.find(L'>', attribute);
    if (valueBegin == std::wstring::npos) return false;
    const size_t end = xml.find(L"</Data>", valueBegin + 1);
    if (end == std::wstring::npos) return false;
    value.assign(xml, valueBegin + 1, end - valueBegin - 1);
    return true;
}

bool ParseUnsigned(const std::wstring& text, ULONGLONG& value) {
    wchar_t* end = nullptr;
    const unsigned long long parsed = wcstoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != L'\0') return false;
    value = static_cast<ULONGLONG>(parsed);
    return true;
}

KernelBootKind ToKernelBootKind(DWORD bootType) {
    switch (bootType) {
    case 0: return KernelBootKind::FullStartup;
    case 1: return KernelBootKind::FastStartup;
    case 2: return KernelBootKind::HibernateResume;
    default: return KernelBootKind::Unknown;
    }
}

} // namespace

KernelBootEvidence QueryLatestKernelBootEvidence() {
    constexpr wchar_t kChannel[] = L"System";
    constexpr wchar_t kQuery[] =
        L"*[System[Provider[@Name='Microsoft-Windows-Kernel-Boot'] and EventID=27]]";

    KernelBootEvidence result;
    ScopedEvtHandle query(EvtQuery(nullptr, kChannel, kQuery,
                                   EvtQueryChannelPath | EvtQueryReverseDirection));
    if (!query.get()) {
        result.error = GetLastError();
        return result;
    }

    ScopedEvtHandle event;
    DWORD returned = 0;
    if (!EvtNext(query.get(), 1, event.put(), 0, 0, &returned) || returned != 1) {
        result.error = GetLastError();
        return result;
    }

    DWORD bytes = 0;
    DWORD properties = 0;
    EvtRender(nullptr, event.get(), EvtRenderEventXml, 0, nullptr, &bytes, &properties);
    const DWORD renderError = GetLastError();
    if (renderError != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        result.error = renderError;
        return result;
    }

    std::vector<wchar_t> xml(bytes / sizeof(wchar_t));
    if (!EvtRender(nullptr, event.get(), EvtRenderEventXml, bytes, xml.data(),
                   &bytes, &properties)) {
        result.error = GetLastError();
        return result;
    }

    const std::wstring eventXml(xml.data());
    std::wstring recordIdText;
    std::wstring bootTypeText;
    ULONGLONG recordId = 0;
    ULONGLONG bootType = 0;
    if (!ExtractElementValue(eventXml, L"EventRecordID", recordIdText) ||
        !ExtractNamedDataValue(eventXml, L"BootType", bootTypeText) ||
        !ParseUnsigned(recordIdText, recordId) ||
        !ParseUnsigned(bootTypeText, bootType) || bootType > MAXDWORD) {
        result.error = ERROR_INVALID_DATA;
        return result;
    }

    result.valid = true;
    result.recordId = recordId;
    result.bootType = static_cast<DWORD>(bootType);
    result.kind = ToKernelBootKind(result.bootType);
    return result;
}

const wchar_t* KernelBootKindName(KernelBootKind kind) {
    switch (kind) {
    case KernelBootKind::FullStartup: return L"full";
    case KernelBootKind::FastStartup: return L"fast-startup";
    case KernelBootKind::HibernateResume: return L"hibernate-resume";
    default: return L"unknown";
    }
}

} // namespace facelogin
