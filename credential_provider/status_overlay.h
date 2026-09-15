#pragma once

#include <windows.h>
#include <credentialprovider.h>
#include <string>

namespace facelogin {

enum class StatusOverlayTone {
    Neutral,
    Progress,
    Guidance,
    Success,
    Error
};

struct StatusOverlayPresentation {
    std::wstring text;
    StatusOverlayTone tone = StatusOverlayTone::Neutral;
    bool visible = false;
};

// A non-interactive status surface owned by LogonUI. The window uses the
// system STATIC class and UpdateLayeredWindow rather than a DLL-owned window
// procedure, so delayed window teardown cannot leave LogonUI calling code from
// an unloaded credential-provider module.
class StatusOverlay {
public:
    StatusOverlay();
    ~StatusOverlay();

    StatusOverlay(const StatusOverlay&) = delete;
    StatusOverlay& operator=(const StatusOverlay&) = delete;

    bool Create(ICredentialProviderCredentialEvents2* events,
                const StatusOverlayPresentation& presentation,
                const wchar_t* reason);
    bool Update(const StatusOverlayPresentation& presentation);
    void Hide();
    void Destroy(const wchar_t* reason);
    bool IsCreated() const;

private:
    bool RenderLocked(const StatusOverlayPresentation& presentation);
    UINT ResolveDpiLocked() const;
    void RecreateFontLocked(UINT dpi);

    mutable CRITICAL_SECTION m_cs;
    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    DWORD m_windowThreadId = 0;
    HFONT m_font = nullptr;
    UINT m_fontDpi = 0;
    bool m_ownsFont = false;
    bool m_creating = false;
    StatusOverlayPresentation m_presentation;
};

} // namespace facelogin
