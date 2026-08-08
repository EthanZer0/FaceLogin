#pragma once

#include <windows.h>
#include <objbase.h>
#include <oaidl.h>
#include <string>

// WebView2 uses MIDL `interface` keyword
#ifndef interface
#define interface struct
#endif
#include "webview2/include/WebView2.h"

namespace facelogin {
class EnrollmentWizard;
}

// COM callback implementations for WebView2 async init
class EnvCallback : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    EnvCallback(HWND hWnd) : m_hWnd(hWnd), m_ref(1) {}
    HWND m_hWnd;

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) {
        if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)()  { return InterlockedIncrement(&m_ref); }
    STDMETHOD_(ULONG, Release)() {
        ULONG c = InterlockedDecrement(&m_ref);
        if (c == 0) delete this;
        return c;
    }
    STDMETHOD(Invoke)(HRESULT hr, ICoreWebView2Environment* env) override;
private:
    ULONG m_ref;
};

class CtrlCallback : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    CtrlCallback(HWND hWnd) : m_hWnd(hWnd), m_ref(1) {}
    HWND m_hWnd;

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) {
        if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)()  { return InterlockedIncrement(&m_ref); }
    STDMETHOD_(ULONG, Release)() {
        ULONG c = InterlockedDecrement(&m_ref);
        if (c == 0) delete this;
        return c;
    }
    STDMETHOD(Invoke)(HRESULT hr, ICoreWebView2Controller* ctrl) override;
private:
    ULONG m_ref;
};

// Main window + WebView2 host
class WebviewHost {
public:
    WebviewHost(HINSTANCE hInstance, facelogin::EnrollmentWizard* wizard);
    ~WebviewHost();
    int Run();
    void ResizeWebView(HWND hWnd);

    HINSTANCE m_hInstance;
    HWND     m_hWnd = nullptr;
    facelogin::EnrollmentWizard* m_wizard;
    LONG m_fixedW = 0;   // fixed window size (pixels), captured at creation
    LONG m_fixedH = 0;

    ICoreWebView2Controller*  m_controller = nullptr;
    ICoreWebView2*            m_webview    = nullptr;
    ICoreWebView2Environment* m_env        = nullptr;
    bool m_sessionNotifRegistered = false;

    std::string m_html;

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);
};

class HostObject : public IDispatch {
public:
    HostObject(facelogin::EnrollmentWizard* w) : m_wizard(w), m_refCount(1), m_lastDispId(0) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv);
    STDMETHOD_(ULONG, AddRef)();
    STDMETHOD_(ULONG, Release)();
    STDMETHOD(GetTypeInfoCount)(UINT*) override { return E_NOTIMPL; }
    STDMETHOD(GetTypeInfo)(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    STDMETHOD(GetIDsOfNames)(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override;
    STDMETHOD(Invoke)(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override;

private:
    facelogin::EnrollmentWizard* m_wizard;
    ULONG m_refCount;
    DISPID m_lastDispId;
};
