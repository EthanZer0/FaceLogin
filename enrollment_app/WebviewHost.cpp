#include "WebviewHost.h"
#include "EnrollmentWizard.h"
#include "../common/logger.h"
#include "resource.h"
#include <shellapi.h>
#include <wtsapi32.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "wtsapi32.lib")

static const wchar_t* WND_CLASS = L"FaceloginWv2Wnd";

// ==========================================================================
// EnvCallback
// ==========================================================================

STDMETHODIMP EnvCallback::Invoke(HRESULT hr, ICoreWebView2Environment* env) {
    if (FAILED(hr) || !env) return hr;
    HWND hWnd = m_hWnd;
    WebviewHost* self = (WebviewHost*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (!self) return E_FAIL;
    env->AddRef();
    self->m_env = env;

    // Create controller
    CtrlCallback* cb = new CtrlCallback(hWnd);
    env->CreateCoreWebView2Controller(hWnd, cb);
    return S_OK;
}

// ==========================================================================
// CtrlCallback
// ==========================================================================

STDMETHODIMP CtrlCallback::Invoke(HRESULT hr, ICoreWebView2Controller* ctrl) {
    if (FAILED(hr) || !ctrl) return hr;
    HWND hWnd = m_hWnd;
    WebviewHost* self = (WebviewHost*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (!self) return E_FAIL;
    ctrl->AddRef();
    self->m_controller = ctrl;
    ctrl->get_CoreWebView2(&self->m_webview);

    // Register host object
    HostObject* host = new HostObject(self->m_wizard);
    VARIANT v; VariantInit(&v);
    v.vt = VT_DISPATCH;
    host->QueryInterface(IID_IDispatch, (void**)&v.pdispVal);
    self->m_webview->AddHostObjectToScript(L"host", &v);
    v.pdispVal->Release();
    host->Release();

    // Settings
    ICoreWebView2Settings* settings = nullptr;
    self->m_webview->get_Settings(&settings);
    if (settings) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->Release();
    }

    // Background: light theme
    COREWEBVIEW2_COLOR bg = {255, 248, 250, 252};
    ICoreWebView2Controller2* c2 = nullptr;
    if (SUCCEEDED(ctrl->QueryInterface(IID_PPV_ARGS(&c2)))) {
        c2->put_DefaultBackgroundColor(bg);
        c2->Release();
    }

    // Load HTML from embedded resource (compiled into EXE)
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_INDEX_HTML), RT_RCDATA);
    std::string htmlContent;
    if (hRes) {
        HGLOBAL hData = LoadResource(nullptr, hRes);
        DWORD size = SizeofResource(nullptr, hRes);
        if (hData && size > 0) {
            const char* p = static_cast<const char*>(LockResource(hData));
            htmlContent.assign(p, size);
            FACELOGIN_INFO(L"Loaded HTML from embedded resource (%lu bytes)", size);
        }
    }
    if (htmlContent.empty()) {
        htmlContent = self->m_html; // fallback
        FACELOGIN_WARN(L"Failed to load HTML from resource, using fallback");
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, htmlContent.c_str(), -1, nullptr, 0);
    std::wstring whtml(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, htmlContent.c_str(), -1, &whtml[0], wlen);
    self->m_webview->NavigateToString(whtml.c_str());

    self->ResizeWebView(hWnd);
    return S_OK;
}

// ==========================================================================
// WebviewHost
// ==========================================================================

WebviewHost::WebviewHost(HINSTANCE hInst, facelogin::EnrollmentWizard* w)
    : m_hInstance(hInst), m_wizard(w) {
    // Fallback HTML in case file can't be read
    m_html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><style>"
        "body{display:flex;align-items:center;justify-content:center;"
        "height:100vh;font-family:Segoe UI,sans-serif;background:#f8fafc;color:#0f172a}"
        "h1{font-size:18px;font-weight:500}"
        "</style></head><body><h1>index.html not found — please ensure it is next to the .exe</h1></body></html>";
}

WebviewHost::~WebviewHost() {
    if (m_controller) { m_controller->Close(); m_controller->Release(); }
    if (m_webview)    m_webview->Release();
    if (m_env)        m_env->Release();
}

int WebviewHost::Run() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = m_hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon   = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_ENROLLMENT_ICON));
    wc.hIconSm = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_ENROLLMENT_ICON));
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);

    // --- Compute window size with DPI-aware content fitting ---
    int scrW = GetSystemMetrics(SM_CXSCREEN), scrH = GetSystemMetrics(SM_CYSCREEN);

    // Get the monitor DPI so we can convert CSS pixels to physical pixels.
    // CSS layout needs ~600 CSS px vertically (viewport 360 + chrome ~240).
    HDC hdc = GetDC(nullptr);
    int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(nullptr, hdc);
    float dpiScale = dpiY / 96.0f;

    // Desired client area in CSS pixels:
    //   Width: just above content max-width (640px) for comfortable margin
    //   Height: ~620 CSS px covers viewport(360) + chrome(234) + breathing room
    int clientWCss = 680;

    int clientHCss = 660;  // covers viewport + chrome + progress bar without scrollbars

    // Convert to physical pixels for the window manager
    int clientW = static_cast<int>(clientWCss * dpiScale);
    int clientH = static_cast<int>(clientHCss * dpiScale);

    RECT rc = {0, 0, clientW, clientH};
    // Fixed-size window: no thick resize border (WS_THICKFRAME removed), no
    // maximize box (already stripped above) — only minimize and close remain.
    DWORD style = (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
    AdjustWindowRect(&rc, style, FALSE);
    int actualWndW = rc.right - rc.left;
    int actualWndH = rc.bottom - rc.top;
    // Capture the fixed size once; WM_GETMINMAXINFO always uses this, so a
    // minimize/restore cycle can never read back an icon-sized rect.
    m_fixedW = actualWndW;
    m_fixedH = actualWndH;

    m_hWnd = CreateWindowExW(0, WND_CLASS, L"FaceLogin Console",
        style,
        (scrW - actualWndW)/2, (scrH - actualWndH)/2, actualWndW, actualWndH,
        nullptr, nullptr, m_hInstance, this);
    if (!m_hWnd) return 1;

    ShowWindow(m_hWnd, SW_SHOW);
    UpdateWindow(m_hWnd);

    // Register for session notifications (lock/unlock) to handle camera contention
    WTSRegisterSessionNotification(m_hWnd, NOTIFY_FOR_THIS_SESSION);
    m_sessionNotifRegistered = true;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

LRESULT CALLBACK WebviewHost::WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    WebviewHost* self;
    if (msg == WM_CREATE) {
        self = (WebviewHost*)((CREATESTRUCT*)lp)->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (WebviewHost*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    }
    if (self) return self->HandleMessage(hWnd, msg, wp, lp);
    return DefWindowProcW(hWnd, msg, wp, lp);
}

LRESULT WebviewHost::HandleMessage(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // Set the window icon (large + small) from the embedded resource
        HICON hIcon = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_ENROLLMENT_ICON));
        if (hIcon) {
            SendMessageW(hWnd, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(hIcon));
            SendMessageW(hWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
        }

        // Prepare WebView2 user data folder in system temp to avoid
        // polluting the EXE directory with a .WebView2 folder.
        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        std::wstring wv2DataDir = std::wstring(tempPath) + L"FaceLoginConsole.WebView2";
        CreateDirectoryW(wv2DataDir.c_str(), nullptr);

        EnvCallback* cb = new EnvCallback(hWnd);
        CreateCoreWebView2EnvironmentWithOptions(nullptr, wv2DataDir.c_str(), nullptr, cb);
        return 0;
    }
    case WM_SIZE:
        // Skip resizing the WebView while minimized (SIZE_MINIMIZED reports a
        // tiny client area); the WM_SIZE(SIZE_RESTORED) on restore re-sizes it
        // back. Resizing the WebView into an icon-sized rect is harmless but
        // avoids a transient flash.
        if (wp != SIZE_MINIMIZED) {
            ResizeWebView(hWnd);
        }
        return 0;

    // Fixed-size window: clamp min/max tracking size to the fixed size captured
    // at creation, so neither dragging the edge (already disabled) nor the
    // system menu can resize the window. Using the stored m_fixedW/H — NOT
    // GetWindowRect — avoids the minimize bug where a restore would read back
    // an icon-sized rect and clamp the window to just the title bar.
    case WM_GETMINMAXINFO: {
        if (m_fixedW > 0 && m_fixedH > 0) {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMaxTrackSize.x = m_fixedW;
            mmi->ptMaxTrackSize.y = m_fixedH;
            mmi->ptMinTrackSize.x = m_fixedW;
            mmi->ptMinTrackSize.y = m_fixedH;
        }
        return 0;
    }

    case WM_WTSSESSION_CHANGE:
        if (m_wizard) {
            switch (wp) {
            case WTS_SESSION_LOCK:
                // Win+L pressed — release camera so login credential provider can use it
                FACELOGIN_INFO(L"WebviewHost: session locked — releasing camera");
                m_wizard->StopPreview();
                break;
            case WTS_SESSION_UNLOCK:
                // User returned — restart preview
                FACELOGIN_INFO(L"WebviewHost: session unlocked — restarting camera");
                m_wizard->StartPreview();
                break;
            }
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        if (m_sessionNotifRegistered) {
            WTSUnRegisterSessionNotification(hWnd);
            m_sessionNotifRegistered = false;
        }
        if (m_wizard) m_wizard->StopPreview();
        if (m_controller) { m_controller->Close(); m_controller->Release(); m_controller = nullptr; }
        if (m_webview)    { m_webview->Release(); m_webview = nullptr; }
        if (m_env)        { m_env->Release(); m_env = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

void WebviewHost::ResizeWebView(HWND hWnd) {
    if (m_controller) {
        RECT rc; GetClientRect(hWnd, &rc);
        m_controller->put_Bounds(rc);
    }
}

// ==========================================================================
// HostObject
// ==========================================================================

STDMETHODIMP HostObject::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDispatch) { *ppv = (IDispatch*)this; AddRef(); return S_OK; }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) HostObject::AddRef()  { return InterlockedIncrement(&m_refCount); }
STDMETHODIMP_(ULONG) HostObject::Release() {
    ULONG c = InterlockedDecrement(&m_refCount);
    if (c == 0) delete this;
    return c;
}

STDMETHODIMP HostObject::GetIDsOfNames(REFIID, LPOLESTR* names, UINT cNames, LCID, DISPID* ids) {
    if (cNames != 1) return E_INVALIDARG;
    std::wstring n(names[0]);
    if (n == L"StartPreview")           *ids = 1;
    else if (n == L"StopPreview")       *ids = 2;
    else if (n == L"GetSampleCount")    *ids = 3;
    else if (n == L"GetUsername")       *ids = 4;
    else if (n == L"CaptureFaceSamples") *ids = 5;
    else if (n == L"ValidatePassword")  *ids = 6;
    else if (n == L"SaveEnrollment")    *ids = 7;
    else if (n == L"GetLatestFrameBase64") *ids = 8;
    else if (n == L"GetLatestFacesJson")   *ids = 9;
    else if (n == L"IsRunning")         *ids = 10;
    else if (n == L"IsLivenessPassed")  *ids = 11;
    else if (n == L"IsLivenessChecking") *ids = 12;
    else if (n == L"GetConfig")    *ids = 13;
    else if (n == L"SetConfig")    *ids = 14;
    else if (n == L"GetLogLines")  *ids = 15;
    else if (n == L"GetServiceLogLines") *ids = 16;
    else if (n == L"ClearLog")     *ids = 17;
    else if (n == L"GetUserSid")   *ids = 18;
    else if (n == L"GetAccountType") *ids = 19;
    else if (n == L"GetLatestFrameAndFaces") *ids = 20;
    else if (n == L"GetCameraList") *ids = 21;
    else if (n == L"IsPasswordlessState") *ids = 22;
    else if (n == L"SaveEnrollmentNoPassword") *ids = 23;
    else if (n == L"GetFaceCount") *ids = 24;
    else if (n == L"GetFacesJson") *ids = 25;
    else if (n == L"SaveEnrollmentAppend") *ids = 26;
    else if (n == L"DeleteFace") *ids = 27;
    else if (n == L"ClearAllFaces") *ids = 28;
    else if (n == L"RenameFace") *ids = 29;
    else if (n == L"CheckAccountTypeChanged") *ids = 30;
    else if (n == L"RefreshAccountIdentity") *ids = 31;
    // NOTE: DISPID 32 is skipped on main — GetCaptureStatus() is a
    // feature/multi-angle-recognition-only method not present here. Keeping
    // ClearStaleAccountUpn at 33 matches that branch, so the future merge
    // slots GetCaptureStatus into 32 without renumbering.
    else if (n == L"ClearStaleAccountUpn") *ids = 33;
    else if (n == L"OpenExternal")        *ids = 34;
    else if (n == L"GetAboutSeen")        *ids = 35;
    else if (n == L"SetAboutSeen")        *ids = 36;
    else if (n == L"GetConsoleVersion")   *ids = 37;
    else if (n == L"NeedsReenrollment")   *ids = 38;
    else return DISP_E_UNKNOWNNAME;
    return S_OK;
}

// Read an optional (may be absent) string argument at position argIdx from the
// DISPPARAMS rgvarg array (0 = last JS argument). Returns empty when absent or
// not a BSTR. WebView2's host object passes arguments with VT_BSTR.
static std::wstring OptionalArg(DISPPARAMS* p, UINT argIdx) {
    if (!p || p->cArgs < argIdx + 1) return L"";
    if (p->rgvarg[argIdx].vt != VT_BSTR) return L"";
    return std::wstring(p->rgvarg[argIdx].bstrVal);
}

static VARIANT MakeStr(const std::string& s) {
    VARIANT v; VariantInit(&v);
    if (!s.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocStringLen(nullptr, wlen - 1);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, v.bstrVal, wlen);
    } else { v.vt = VT_BSTR; v.bstrVal = SysAllocString(L""); }
    return v;
}
static VARIANT MakeBool(bool v) { VARIANT var; VariantInit(&var); var.vt = VT_BOOL; var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE; return var; }
static VARIANT MakeInt(int v)  { VARIANT var; VariantInit(&var); var.vt = VT_I4; var.lVal = v; return var; }

STDMETHODIMP HostObject::Invoke(DISPID id, REFIID, LCID, WORD wFlags, DISPPARAMS* p, VARIANT* res, EXCEPINFO*, UINT*) {
    if (!m_wizard) return E_FAIL;
    if (res) VariantInit(res);

    // In sync mode, WebView2 calls DISPATCH_PROPERTYGET first to get the
    // method object, then DISPATCH_METHOD with DISPID_VALUE to invoke.
    if (wFlags & DISPATCH_PROPERTYGET) {
        m_lastDispId = id;
        if (res) {
            res->vt = VT_DISPATCH;
            res->pdispVal = this;
            AddRef();
        }
        return S_OK;
    }

    if (wFlags & DISPATCH_METHOD) {
        // DISPID_VALUE means "invoke the previously resolved method"
        if (id == DISPID_VALUE) id = m_lastDispId;

        switch (id) {
        case 1:  if (res) *res = MakeBool(m_wizard->StartPreview()); break;
        case 2:  m_wizard->StopPreview(); break;
        case 3:  if (res) *res = MakeInt(m_wizard->GetSampleCount()); break;
        case 4:  if (res) *res = MakeStr(m_wizard->GetUsername()); break;
        case 5:  if (res) *res = MakeBool(m_wizard->CaptureFaceSamples()); break;
        case 6: {
            if (p->cArgs < 1) return DISP_E_BADPARAMCOUNT;
            std::wstring pass(p->rgvarg[0].vt == VT_BSTR ? p->rgvarg[0].bstrVal : L"");
            if (res) *res = MakeBool(m_wizard->ValidatePassword(pass));
            break;
        }
        case 7: {
            // DISPPARAMS.rgvarg is in REVERSE order: rgvarg[0] is the LAST JS
            // argument. JS calls SaveEnrollment(password, label), so rgvarg[0]
            // is the label and rgvarg[1] is the password. (Reading them the
            // other way round stored the password as the face label — see the
            // first-enrollment password bug.)
            if (p->cArgs < 1) return DISP_E_BADPARAMCOUNT;
            std::wstring label = OptionalArg(p, 0);
            std::wstring pass;
            if (p->cArgs >= 2) {
                pass = (p->rgvarg[1].vt == VT_BSTR) ? std::wstring(p->rgvarg[1].bstrVal) : L"";
            } else {
                // Backward compat: a single-argument call is the password.
                pass = (p->rgvarg[0].vt == VT_BSTR) ? std::wstring(p->rgvarg[0].bstrVal) : L"";
            }
            if (res) *res = MakeBool(m_wizard->SaveEnrollment(pass, label));
            break;
        }
        case 8:  if (res) *res = MakeStr(m_wizard->GetLatestFrameBase64()); break;
        case 9:  if (res) *res = MakeStr(m_wizard->GetLatestFacesJson()); break;
        case 10: if (res) *res = MakeBool(m_wizard->IsPreviewRunning()); break;
        case 11: if (res) *res = MakeBool(m_wizard->IsLivenessPassed()); break;
        case 12: if (res) *res = MakeBool(m_wizard->IsLivenessChecking()); break;
        case 13: if (res) *res = MakeStr(m_wizard->GetConfig()); break;
        case 14: {
            if (p->cArgs < 1) return DISP_E_BADPARAMCOUNT;
            std::string json;
            if (p->rgvarg[0].vt == VT_BSTR) {
                int len = WideCharToMultiByte(CP_UTF8, 0, p->rgvarg[0].bstrVal, -1, nullptr, 0, nullptr, nullptr);
                json.resize(len > 0 ? len - 1 : 0);
                if (len > 0) WideCharToMultiByte(CP_UTF8, 0, p->rgvarg[0].bstrVal, -1, &json[0], len, nullptr, nullptr);
            }
            if (res) *res = MakeBool(m_wizard->SetConfig(json));
            break;
        }
        case 15: if (res) *res = MakeStr(m_wizard->GetLogLines()); break;
        case 16: if (res) *res = MakeStr(m_wizard->GetServiceLogLines()); break;
        case 17: m_wizard->ClearLog(); break;
        case 18: if (res) *res = MakeStr(m_wizard->GetUserSid()); break;
        case 19: if (res) *res = MakeStr(m_wizard->GetAccountType()); break;
        case 20: if (res) *res = MakeStr(m_wizard->GetLatestFrameAndFaces()); break;
        case 21: if (res) *res = MakeStr(m_wizard->GetCameraList()); break;
        case 22: if (res) *res = MakeInt(m_wizard->GetPasswordlessState()); break;
        case 23: {
            std::wstring label = OptionalArg(p, 0);  // first JS arg: face label
            if (res) *res = MakeBool(m_wizard->SaveEnrollmentNoPassword(label));
            break;
        }
        case 24: if (res) *res = MakeInt(m_wizard->GetFaceCount()); break;
        case 25: if (res) *res = MakeStr(m_wizard->GetFacesJson()); break;
        case 26: {
            std::wstring label = OptionalArg(p, 0);  // face label
            if (res) *res = MakeBool(m_wizard->SaveEnrollmentAppend(label));
            break;
        }
        case 27: {
            if (p->cArgs < 1) return DISP_E_BADPARAMCOUNT;
            if (p->rgvarg[0].vt != VT_I4) return DISP_E_TYPEMISMATCH;
            if (res) *res = MakeBool(m_wizard->DeleteFace(p->rgvarg[0].lVal));
            break;
        }
        case 28: if (res) *res = MakeBool(m_wizard->ClearAllFaces()); break;
        case 29: {
            if (p->cArgs < 2) return DISP_E_BADPARAMCOUNT;
            if (p->rgvarg[1].vt != VT_I4) return DISP_E_TYPEMISMATCH;
            std::wstring label = OptionalArg(p, 0);
            if (res) *res = MakeBool(m_wizard->RenameFace(p->rgvarg[1].lVal, label));
            break;
        }
        case 30: if (res) *res = MakeStr(m_wizard->CheckAccountTypeChanged()); break;
        case 31: {
            // Single argument (password), so rgvarg[0] is it — no reverse-order
            // ambiguity. Same access pattern as case 6.
            if (p->cArgs < 1) return DISP_E_BADPARAMCOUNT;
            std::wstring pass(p->rgvarg[0].vt == VT_BSTR ? p->rgvarg[0].bstrVal : L"");
            if (res) *res = MakeBool(m_wizard->RefreshAccountIdentity(pass));
            break;
        }
        case 33: if (res) *res = MakeBool(m_wizard->ClearStaleAccountUpn()); break;
        case 34: {
            // OpenExternal(url) — open a URL in the OS default browser
            // instead of a WebView2 popup window. Used by the About card's
            // GitHub link.
            std::wstring url = OptionalArg(p, 0);
            if (url.empty()) return DISP_E_BADPARAMCOUNT;
            HINSTANCE r = ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            // ShellExecute returns >32 on success (e.g. 42 = instance handle);
            // ERROR_FILE_NOT_FOUND or similar (<32) means it failed.
            if (res) *res = MakeBool(reinterpret_cast<INT_PTR>(r) > 32);
            break;
        }
        case 35: if (res) *res = MakeBool(m_wizard->GetAboutSeen()); break;
        case 36: {
            if (p->cArgs < 1) return DISP_E_BADPARAMCOUNT;
            if (p->rgvarg[0].vt != VT_BOOL) return DISP_E_TYPEMISMATCH;
            m_wizard->SetAboutSeen(p->rgvarg[0].boolVal == VARIANT_TRUE);
            break;
        }
        case 37: if (res) *res = MakeStr(m_wizard->GetConsoleVersion()); break;
        case 38: if (res) *res = MakeBool(m_wizard->NeedsReenrollment()); break;
        default: return DISP_E_MEMBERNOTFOUND;
        }
        return S_OK;
    }

    return DISP_E_MEMBERNOTFOUND;
}
