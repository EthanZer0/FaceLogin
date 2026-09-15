#include "status_overlay.h"

#include "../common/logger.h"

#include <algorithm>
#include <cstdint>
#include <vector>

extern HMODULE g_hModule;

namespace facelogin {
namespace {

constexpr COLORREF kText = RGB(248, 250, 252);
constexpr BYTE kWindowOpacity = 226;
constexpr BYTE kBackgroundR = 31;
constexpr BYTE kBackgroundG = 36;
constexpr BYTE kBackgroundB = 44;

int ScaleDip(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

COLORREF ToneColor(StatusOverlayTone tone) {
    switch (tone) {
    case StatusOverlayTone::Progress:
    case StatusOverlayTone::Success:
        return RGB(16, 169, 121);
    case StatusOverlayTone::Guidance:
        return RGB(231, 174, 67);
    case StatusOverlayTone::Error:
        return RGB(225, 93, 93);
    case StatusOverlayTone::Neutral:
    default:
        return RGB(132, 145, 166);
    }
}

BYTE RoundedRectangleCoverage(
    int x, int y, int width, int height, int radius) {
    if (x >= radius && x < width - radius) return 255;
    if (y >= radius && y < height - radius) return 255;

    constexpr int kSamplesPerAxis = 4;
    constexpr int kSampleCount = kSamplesPerAxis * kSamplesPerAxis;
    const double centerX = x < radius
        ? static_cast<double>(radius)
        : static_cast<double>(width - radius);
    const double centerY = y < radius
        ? static_cast<double>(radius)
        : static_cast<double>(height - radius);
    const double radiusSquared = static_cast<double>(radius) * radius;
    int coveredSamples = 0;
    for (int sampleY = 0; sampleY < kSamplesPerAxis; ++sampleY) {
        const double pointY = static_cast<double>(y) +
            (static_cast<double>(sampleY) + 0.5) / kSamplesPerAxis;
        for (int sampleX = 0; sampleX < kSamplesPerAxis; ++sampleX) {
            const double pointX = static_cast<double>(x) +
                (static_cast<double>(sampleX) + 0.5) / kSamplesPerAxis;
            const double dx = pointX - centerX;
            const double dy = pointY - centerY;
            if (dx * dx + dy * dy <= radiusSquared) ++coveredSamples;
        }
    }
    return static_cast<BYTE>(
        (coveredSamples * 255 + kSampleCount / 2) / kSampleCount);
}

BYTE Premultiply(BYTE channel, BYTE alpha) {
    return static_cast<BYTE>(
        (static_cast<unsigned>(channel) * alpha + 127u) / 255u);
}

} // namespace

StatusOverlay::StatusOverlay() {
    InitializeCriticalSection(&m_cs);
}

StatusOverlay::~StatusOverlay() {
    Destroy(L"destructor");
    EnterCriticalSection(&m_cs);
    if (m_font && m_ownsFont) {
        DeleteObject(m_font);
    }
    m_font = nullptr;
    m_ownsFont = false;
    LeaveCriticalSection(&m_cs);
    DeleteCriticalSection(&m_cs);
}

bool StatusOverlay::Create(
    ICredentialProviderCredentialEvents2* events,
    const StatusOverlayPresentation& presentation,
    const wchar_t* reason) {
    if (!events || !presentation.visible || presentation.text.empty()) return false;

    EnterCriticalSection(&m_cs);
    if (m_hwnd && IsWindow(m_hwnd)) {
        LeaveCriticalSection(&m_cs);
        return Update(presentation);
    }
    if (m_creating) {
        LeaveCriticalSection(&m_cs);
        return false;
    }
    m_creating = true;
    LeaveCriticalSection(&m_cs);

    HWND owner = nullptr;
    const HRESULT ownerHr = events->OnCreatingWindow(&owner);
    if (FAILED(ownerHr) || !owner || !IsWindow(owner)) {
        EnterCriticalSection(&m_cs);
        m_creating = false;
        LeaveCriticalSection(&m_cs);
        FACELOGIN_WARN(L"StatusOverlay: action=create reason=%s result=owner_failed hr=0x%08X owner=%p",
                       reason ? reason : L"unknown", ownerHr, owner);
        return false;
    }

    const HWND overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_LAYERED | WS_EX_TRANSPARENT,
        L"STATIC", L"",
        WS_POPUP | WS_DISABLED,
        0, 0, 1, 1,
        owner, nullptr, g_hModule, nullptr);
    if (!overlay) {
        const DWORD error = GetLastError();
        EnterCriticalSection(&m_cs);
        m_creating = false;
        LeaveCriticalSection(&m_cs);
        FACELOGIN_WARN(L"StatusOverlay: action=create reason=%s result=window_failed error=%lu owner=%p",
                       reason ? reason : L"unknown", error, owner);
        return false;
    }

    wchar_t desktopName[128] = {};
    wchar_t stationName[128] = {};
    DWORD needed = 0;
    const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    const HWINSTA station = GetProcessWindowStation();
    if (desktop) {
        GetUserObjectInformationW(desktop, UOI_NAME, desktopName,
                                  sizeof(desktopName), &needed);
    }
    if (station) {
        GetUserObjectInformationW(station, UOI_NAME, stationName,
                                  sizeof(stationName), &needed);
    }

    EnterCriticalSection(&m_cs);
    m_hwnd = overlay;
    m_owner = owner;
    m_windowThreadId = GetCurrentThreadId();
    m_creating = false;
    m_presentation = {};
    const bool rendered = RenderLocked(presentation);
    LeaveCriticalSection(&m_cs);

    if (!rendered) {
        Destroy(L"initial_render_failed");
        return false;
    }

    FACELOGIN_INFO(L"StatusOverlay: action=create reason=%s result=success hwnd=%p owner=%p station='%s' desktop='%s'",
                   reason ? reason : L"unknown", overlay, owner,
                   stationName[0] ? stationName : L"<unknown>",
                   desktopName[0] ? desktopName : L"<unknown>");
    return true;
}

bool StatusOverlay::Update(const StatusOverlayPresentation& presentation) {
    EnterCriticalSection(&m_cs);
    if (!m_hwnd || !IsWindow(m_hwnd)) {
        LeaveCriticalSection(&m_cs);
        return false;
    }
    if (!presentation.visible || presentation.text.empty()) {
        ShowWindow(m_hwnd, SW_HIDE);
        m_presentation = presentation;
        LeaveCriticalSection(&m_cs);
        return true;
    }
    if (m_presentation.visible &&
        m_presentation.tone == presentation.tone &&
        m_presentation.text == presentation.text) {
        LeaveCriticalSection(&m_cs);
        return true;
    }
    const bool rendered = RenderLocked(presentation);
    LeaveCriticalSection(&m_cs);
    return rendered;
}

void StatusOverlay::Hide() {
    EnterCriticalSection(&m_cs);
    if (m_hwnd && IsWindow(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE);
    m_presentation.visible = false;
    LeaveCriticalSection(&m_cs);
}

void StatusOverlay::Destroy(const wchar_t* reason) {
    HWND overlay = nullptr;
    DWORD windowThreadId = 0;
    EnterCriticalSection(&m_cs);
    overlay = m_hwnd;
    windowThreadId = m_windowThreadId;
    m_hwnd = nullptr;
    m_owner = nullptr;
    m_windowThreadId = 0;
    m_creating = false;
    m_presentation = {};
    LeaveCriticalSection(&m_cs);

    if (!overlay || !IsWindow(overlay)) return;
    ShowWindow(overlay, SW_HIDE);
    if (windowThreadId == GetCurrentThreadId()) {
        DestroyWindow(overlay);
    } else {
        PostMessageW(overlay, WM_CLOSE, 0, 0);
    }
    FACELOGIN_INFO(L"StatusOverlay: action=destroy reason=%s hwnd=%p",
                   reason ? reason : L"unknown", overlay);
}

bool StatusOverlay::IsCreated() const {
    EnterCriticalSection(&m_cs);
    const bool created = m_hwnd && IsWindow(m_hwnd);
    LeaveCriticalSection(&m_cs);
    return created;
}

UINT StatusOverlay::ResolveDpiLocked() const {
    if (m_owner && IsWindow(m_owner)) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        static const auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (getDpiForWindow) {
            const UINT dpi = getDpiForWindow(m_owner);
            if (dpi) return dpi;
        }
    }

    HWND dcOwner = m_owner;
    HDC dc = GetDC(dcOwner);
    if (!dc) {
        dcOwner = nullptr;
        dc = GetDC(nullptr);
    }
    if (!dc) return 96;
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(dcOwner, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

void StatusOverlay::RecreateFontLocked(UINT dpi) {
    if (m_font && m_fontDpi == dpi) return;
    if (m_font && m_ownsFont) DeleteObject(m_font);
    m_font = CreateFontW(
        -MulDiv(14, static_cast<int>(dpi), 72),
        0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI Variable Text");
    m_ownsFont = m_font != nullptr;
    if (!m_font) {
        m_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        m_ownsFont = false;
    }
    m_fontDpi = dpi;
}

bool StatusOverlay::RenderLocked(
    const StatusOverlayPresentation& presentation) {
    if (!m_hwnd || !IsWindow(m_hwnd) ||
        !m_owner || !IsWindow(m_owner)) {
        return false;
    }

    const UINT dpi = ResolveDpiLocked();
    RecreateFontLocked(dpi);

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) return false;
    HDC memoryDc = CreateCompatibleDC(screenDc);
    if (!memoryDc) {
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    const HGDIOBJ previousFont = SelectObject(memoryDc, m_font);
    RECT measured = {0, 0, 0, 0};
    DrawTextW(memoryDc, presentation.text.c_str(), -1, &measured,
              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);

    const HMONITOR monitor = MonitorFromWindow(m_owner, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo = {sizeof(monitorInfo)};
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        monitorInfo.rcMonitor = {0, 0,
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const int monitorWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
    const int monitorHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
    const int availableWidth =
        (std::max)(ScaleDip(160, dpi), monitorWidth - ScaleDip(48, dpi));
    const int maxWidth = (std::min)(ScaleDip(720, dpi), availableWidth);
    const int minWidth = (std::min)(ScaleDip(260, dpi), maxWidth);
    const int measuredWidth =
        static_cast<int>(measured.right - measured.left);
    const int width = (std::min)(maxWidth,
        (std::max)(minWidth,
                   measuredWidth + ScaleDip(74, dpi)));
    const int height = ScaleDip(52, dpi);
    const int x = monitorInfo.rcMonitor.left + (monitorWidth - width) / 2;
    const int y = monitorInfo.rcMonitor.top + (monitorHeight - height) / 2;

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        memoryDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        SelectObject(memoryDc, previousFont);
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        if (bitmap) DeleteObject(bitmap);
        return false;
    }

    const HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
    auto* argb = static_cast<std::uint32_t*>(pixels);
    std::fill(argb, argb + static_cast<size_t>(width) * height, 0u);

    const int radius = ScaleDip(10, dpi);
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            const BYTE coverage =
                RoundedRectangleCoverage(px, py, width, height, radius);
            if (coverage != 0) {
                argb[static_cast<size_t>(py) * width + px] =
                    (static_cast<std::uint32_t>(coverage) << 24) |
                    (static_cast<std::uint32_t>(kBackgroundR) << 16) |
                    (static_cast<std::uint32_t>(kBackgroundG) << 8) |
                    kBackgroundB;
            }
        }
    }

    const COLORREF accentColor = ToneColor(presentation.tone);
    const HBRUSH accentBrush = CreateSolidBrush(accentColor);
    RECT accentRect = {
        ScaleDip(18, dpi), ScaleDip(15, dpi),
        ScaleDip(22, dpi), height - ScaleDip(15, dpi)};
    FillRect(memoryDc, &accentRect, accentBrush);
    DeleteObject(accentBrush);

    SetBkMode(memoryDc, TRANSPARENT);
    SetTextColor(memoryDc, kText);
    RECT textRect = {
        ScaleDip(38, dpi), 0,
        width - ScaleDip(20, dpi), height};
    DrawTextW(memoryDc, presentation.text.c_str(), -1, &textRect,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    // GDI does not preserve the alpha byte of every glyph pixel. Restore the
    // supersampled mask and premultiply edge pixels for UpdateLayeredWindow;
    // text and the accent stay in the fully opaque interior.
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            auto& pixel = argb[static_cast<size_t>(py) * width + px];
            const BYTE coverage =
                RoundedRectangleCoverage(px, py, width, height, radius);
            if (coverage == 0) {
                pixel = 0;
                continue;
            }
            const BYTE red = static_cast<BYTE>((pixel >> 16) & 0xFFu);
            const BYTE green = static_cast<BYTE>((pixel >> 8) & 0xFFu);
            const BYTE blue = static_cast<BYTE>(pixel & 0xFFu);
            pixel = (static_cast<std::uint32_t>(coverage) << 24) |
                    (static_cast<std::uint32_t>(Premultiply(red, coverage)) << 16) |
                    (static_cast<std::uint32_t>(Premultiply(green, coverage)) << 8) |
                    Premultiply(blue, coverage);
        }
    }

    POINT destination = {x, y};
    SIZE size = {width, height};
    POINT source = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, kWindowOpacity, AC_SRC_ALPHA};
    const BOOL updated = UpdateLayeredWindow(
        m_hwnd, screenDc, &destination, &size,
        memoryDc, &source, 0, &blend, ULW_ALPHA);

    SelectObject(memoryDc, previousBitmap);
    SelectObject(memoryDc, previousFont);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);

    if (!updated) {
        FACELOGIN_WARN(L"StatusOverlay: action=render result=failed error=%lu",
                       GetLastError());
        return false;
    }

    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    m_presentation = presentation;
    return true;
}

} // namespace facelogin
