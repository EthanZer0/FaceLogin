#include "webcam_capture_dshow.h"
#include "../common/logger.h"
#include <new>

#pragma comment(lib, "strmiids.lib")

namespace facelogin {

// ============================================================================
// Static COM helpers
// ============================================================================

bool   WebcamCaptureDS::s_comInitialized = false;
int    WebcamCaptureDS::s_comRefCount    = 0;
CRITICAL_SECTION WebcamCaptureDS::s_comCs;

bool WebcamCaptureDS::InitializeCOM() {
    EnterCriticalSection(&s_comCs);
    if (!s_comInitialized) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) {
            LeaveCriticalSection(&s_comCs);
            FACELOGIN_ERROR(L"DS CoInitializeEx(COINIT_MULTITHREADED) failed: 0x%08X", hr);
            return false;
        }
        s_comInitialized = true;
    }
    s_comRefCount++;
    LeaveCriticalSection(&s_comCs);
    return true;
}

void WebcamCaptureDS::ShutdownCOM() {
    EnterCriticalSection(&s_comCs);
    if (s_comRefCount > 0) {
        s_comRefCount--;
        if (s_comRefCount == 0 && s_comInitialized) {
            CoUninitialize();
            s_comInitialized = false;
        }
    }
    LeaveCriticalSection(&s_comCs);
}

// ============================================================================
// Construction / destruction
// ============================================================================

WebcamCaptureDS::WebcamCaptureDS()
    : m_callback(this)
{
    InitializeCriticalSection(&m_frameCs);
    InitializeCriticalSection(&s_comCs);
}

WebcamCaptureDS::~WebcamCaptureDS() {
    Shutdown();
    DeleteCriticalSection(&m_frameCs);
    // s_comCs is intentionally leaked (process-lifetime singleton)
}

// ============================================================================
// SampleCB — ISampleGrabberCB
// ============================================================================

STDMETHODIMP WebcamCaptureDS::GrabberCB::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB) {
        *ppv = static_cast<ISampleGrabberCB*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) WebcamCaptureDS::GrabberCB::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) WebcamCaptureDS::GrabberCB::Release() {
    return InterlockedDecrement(&m_refCount);
}

STDMETHODIMP WebcamCaptureDS::GrabberCB::SampleCB(double, IMediaSample*) {
    return S_OK;
}

STDMETHODIMP WebcamCaptureDS::GrabberCB::BufferCB(double, BYTE* buffer, long len) {
    if (!buffer || len <= 0)
        return S_OK;

    EnterCriticalSection(&m_parent->m_frameCs);

    if (m_parent->m_frameSize < len) {
        delete[] m_parent->m_frameBuffer;
        m_parent->m_frameBuffer = new (std::nothrow) BYTE[len];
        if (!m_parent->m_frameBuffer) {
            LeaveCriticalSection(&m_parent->m_frameCs);
            return E_OUTOFMEMORY;
        }
        m_parent->m_frameSize = len;
    }

    memcpy(m_parent->m_frameBuffer, buffer, len);
    m_parent->m_frameReady = true;

    LeaveCriticalSection(&m_parent->m_frameCs);
    return S_OK;
}

// ============================================================================
// Camera enumeration
// ============================================================================

static bool GetPin(IBaseFilter* pFilter, PIN_DIRECTION dir, IPin** ppPin) {
    *ppPin = nullptr;
    IEnumPins* pEnum = nullptr;
    if (FAILED(pFilter->EnumPins(&pEnum))) return false;

    IPin* pPin = nullptr;
    while (pEnum->Next(1, &pPin, nullptr) == S_OK) {
        PIN_DIRECTION pinDir;
        if (SUCCEEDED(pPin->QueryDirection(&pinDir)) && pinDir == dir) {
            *ppPin = pPin;
            pEnum->Release();
            return true;
        }
        pPin->Release();
    }
    pEnum->Release();
    return false;
}

bool WebcamCaptureDS::FindFirstCamera(IBaseFilter** ppFilter) {
    *ppFilter = nullptr;

    ICreateDevEnum* pDevEnum = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pDevEnum));
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS CoCreateInstance(SystemDeviceEnum) failed: 0x%08X", hr);
        return false;
    }

    IEnumMoniker* pEnum = nullptr;
    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    pDevEnum->Release();

    if (FAILED(hr) || pEnum == nullptr) {
        FACELOGIN_WARN(L"DS: no video capture devices found (pEnum=%p, hr=0x%08X)",
                       (void*)pEnum, hr);
        return false;
    }

    IMoniker* pMoniker = nullptr;
    ULONG fetched = 0;
    hr = pEnum->Next(1, &pMoniker, &fetched);
    pEnum->Release();

    if (hr != S_OK || fetched == 0) {
        FACELOGIN_WARN(L"DS camera enumeration returned no monikers");
        return false;
    }

    hr = pMoniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)ppFilter);
    pMoniker->Release();

    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS BindToObject (camera) failed: 0x%08X", hr);
        return false;
    }

    FACELOGIN_INFO(L"DS: found first video capture device");
    return true;
}

// ============================================================================
// Filter graph construction
// ============================================================================

bool WebcamCaptureDS::BuildGraph(IBaseFilter* pCapture, int width, int height) {
    HRESULT hr;

    // 1. Create Filter Graph
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&m_pGraph));
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS CoCreateInstance(FilterGraph) failed: 0x%08X", hr);
        return false;
    }

    // 2. Add capture filter
    hr = m_pGraph->AddFilter(pCapture, L"Video Capture");
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS AddFilter(capture) failed: 0x%08X", hr);
        return false;
    }

    // 3. Create Sample Grabber
    IBaseFilter* pGrabberFilter = nullptr;
    hr = CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&pGrabberFilter));
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS CoCreateInstance(SampleGrabber) failed: 0x%08X", hr);
        return false;
    }

    hr = m_pGraph->AddFilter(pGrabberFilter, L"Sample Grabber");
    if (FAILED(hr)) {
        pGrabberFilter->Release();
        return false;
    }

    hr = pGrabberFilter->QueryInterface(IID_ISampleGrabber, (void**)&m_pGrabber);
    pGrabberFilter->Release();  // graph owns the ref now
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS QueryInterface(ISampleGrabber) failed: 0x%08X", hr);
        return false;
    }

    // 4. Configure Sample Grabber: RGB24, BufferCB (not buffered)
    {
        AM_MEDIA_TYPE mt = {};
        mt.majortype  = MEDIATYPE_Video;
        mt.subtype    = MEDIASUBTYPE_RGB24;
        mt.formattype = FORMAT_VideoInfo;
        hr = m_pGrabber->SetMediaType(&mt);
        if (FAILED(hr)) {
            FACELOGIN_ERROR(L"DS SetMediaType(RGB24) failed: 0x%08X", hr);
            return false;
        }

        hr = m_pGrabber->SetBufferSamples(FALSE);
        if (FAILED(hr)) {
            FACELOGIN_ERROR(L"DS SetBufferSamples(FALSE) failed: 0x%08X", hr);
            return false;
        }

        hr = m_pGrabber->SetCallback(&m_callback, 1);  // 1 = BufferCB
        if (FAILED(hr)) {
            FACELOGIN_ERROR(L"DS SetCallback failed: 0x%08X", hr);
            return false;
        }
    }

    // 5. Create Null Renderer
    hr = CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&m_pNullRenderer));
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS CoCreateInstance(NullRenderer) failed: 0x%08X", hr);
        return false;
    }

    hr = m_pGraph->AddFilter(m_pNullRenderer, L"Null Renderer");
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS AddFilter(NullRenderer) failed: 0x%08X", hr);
        return false;
    }

    // 6. Try to set resolution via IAMStreamConfig
    {
        IPin* pCaptureOut = nullptr;
        if (GetPin(pCapture, PINDIR_OUTPUT, &pCaptureOut)) {
            IAMStreamConfig* pConfig = nullptr;
            hr = pCaptureOut->QueryInterface(IID_PPV_ARGS(&pConfig));
            if (SUCCEEDED(hr)) {
                AM_MEDIA_TYPE* pmt = nullptr;
                hr = pConfig->GetFormat(&pmt);
                if (SUCCEEDED(hr) && pmt) {
                    if (pmt->formattype == FORMAT_VideoInfo) {
                        auto* pVih = (VIDEOINFOHEADER*)pmt->pbFormat;
                        pVih->bmiHeader.biWidth  = width;
                        pVih->bmiHeader.biHeight = height;
                        pVih->bmiHeader.biSizeImage = width * height * 3;
                        hr = pConfig->SetFormat(pmt);
                        if (SUCCEEDED(hr))
                            FACELOGIN_INFO(L"DS: set capture format %dx%d", width, height);
                        else
                            FACELOGIN_WARN(L"DS SetFormat failed: 0x%08X, using default", hr);
                    }
                    FreeMediaType(*pmt);
                    CoTaskMemFree(pmt);
                }
                pConfig->Release();
            }
            pCaptureOut->Release();
        }
    }

    // 7. Manually connect the graph:
    //    Capture output → SampleGrabber input
    //    SampleGrabber output → Null Renderer input
    // This ensures data flows through the SampleGrabber so BufferCB fires.
    {
        IPin* pCaptureOut = nullptr;
        IPin* pGrabberIn  = nullptr;
        IPin* pGrabberOut = nullptr;
        IPin* pNullIn     = nullptr;

        if (!GetPin(pCapture,        PINDIR_OUTPUT, &pCaptureOut) ||
            !GetPin(pGrabberFilter,  PINDIR_INPUT,  &pGrabberIn)  ||
            !GetPin(pGrabberFilter,  PINDIR_OUTPUT, &pGrabberOut) ||
            !GetPin(m_pNullRenderer, PINDIR_INPUT,  &pNullIn)) {
            FACELOGIN_ERROR(L"DS: failed to get pins for graph connection");
            return false;
        }

        // Connect capture → grabber
        hr = m_pGraph->Connect(pCaptureOut, pGrabberIn);
        if (FAILED(hr)) {
            FACELOGIN_ERROR(L"DS Connect(capture→grabber) failed: 0x%08X", hr);
            pCaptureOut->Release(); pGrabberIn->Release();
            pGrabberOut->Release(); pNullIn->Release();
            return false;
        }
        pCaptureOut->Release();
        pGrabberIn->Release();

        // Connect grabber → null renderer
        hr = m_pGraph->Connect(pGrabberOut, pNullIn);
        if (FAILED(hr)) {
            FACELOGIN_ERROR(L"DS Connect(grabber→null) failed: 0x%08X", hr);
            pGrabberOut->Release(); pNullIn->Release();
            return false;
        }
        pGrabberOut->Release();
        pNullIn->Release();

        FACELOGIN_INFO(L"DS: graph connected capture→grabber→null");
    }

    // 8. Query IMediaControl
    hr = m_pGraph->QueryInterface(IID_PPV_ARGS(&m_pControl));
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS QueryInterface(IMediaControl) failed: 0x%08X", hr);
        return false;
    }

    return true;
}

// ============================================================================
// Public API
// ============================================================================

bool WebcamCaptureDS::Initialize(int preferredWidth, int preferredHeight) {
    if (m_initialized) return true;

    m_width  = preferredWidth;
    m_height = preferredHeight;

    if (!InitializeCOM()) {
        FACELOGIN_ERROR(L"DS: COM init failed");
        return false;
    }

    if (!FindFirstCamera(&m_pCapture)) {
        FACELOGIN_ERROR(L"DS: no camera found — check if camera is connected and "
                         "drivers are installed.");
        ShutdownCOM();
        return false;
    }

    if (!BuildGraph(m_pCapture, m_width, m_height)) {
        FACELOGIN_ERROR(L"DS: failed to build capture graph");
        if (m_pCapture) { m_pCapture->Release(); m_pCapture = nullptr; }
        ShutdownCOM();
        return false;
    }

    HRESULT hr = m_pControl->Run();
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS IMediaControl::Run failed: 0x%08X", hr);
        Shutdown();
        return false;
    }

    m_initialized = true;
    FACELOGIN_INFO(L"DirectShow webcam initialized: %dx%d RGB24", m_width, m_height);
    return true;
}

bool WebcamCaptureDS::IsFrameReady() {
    // Stub for API parity.  GrabFrame handles readiness internally.
    return m_initialized;
}

void WebcamCaptureDS::Pause() {
    if (m_pControl && m_initialized) {
        m_pControl->Stop();
        // Discard any stale frame so the next resume starts fresh
        EnterCriticalSection(&m_frameCs);
        m_frameReady = false;
        LeaveCriticalSection(&m_frameCs);
        FACELOGIN_INFO(L"DS: graph stopped (camera LED off)");
    }
}

void WebcamCaptureDS::Resume() {
    if (m_pControl && m_initialized) {
        m_pControl->Run();
        FACELOGIN_INFO(L"DS: graph resumed (camera LED on)");
    }
}

bool WebcamCaptureDS::GrabFrame(dlib::matrix<dlib::rgb_pixel>& outFrame) {
    if (!m_initialized) return false;

    EnterCriticalSection(&m_frameCs);

    if (!m_frameReady || !m_frameBuffer || m_frameSize == 0) {
        LeaveCriticalSection(&m_frameCs);
        return false;
    }

    long stride = m_width * 3;
    outFrame.set_size(m_height, m_width);

    // DirectShow RGB24 is bottom-up (biHeight > 0 in VIDEOINFOHEADER
    // means the first scan line is the bottom of the image).
    // dlib::matrix uses top-down indexing, so we flip vertically.
    //
    // DShow RGB24 byte order: B, G, R
    // dlib rgb_pixel struct order: red, green, blue → mem layout = R, G, B
    // → need to swap R↔B

    const BYTE* src = m_frameBuffer;
    for (int row = 0; row < m_height; row++) {
        int srcRow = m_height - 1 - row;  // flip bottom-up → top-down
        const BYTE* srcRowPtr = src + srcRow * stride;
        for (int col = 0; col < m_width; col++) {
            const BYTE* pixel = srcRowPtr + col * 3;
            dlib::rgb_pixel& dst = outFrame(row, col);
            dst.red   = pixel[2];   // byte 2 of BGR = R
            dst.green = pixel[1];   // byte 1 of BGR = G
            dst.blue  = pixel[0];   // byte 0 of BGR = B
        }
    }

    LeaveCriticalSection(&m_frameCs);
    return true;
}

void WebcamCaptureDS::Shutdown() {
    // Stop streaming first so BufferCB stops firing
    if (m_pControl) {
        m_pControl->Stop();
        m_pControl->Release();
        m_pControl = nullptr;
    }

    // Release grabber before the graph
    if (m_pGrabber) {
        m_pGrabber->Release();
        m_pGrabber = nullptr;
    }
    if (m_pNullRenderer) {
        m_pNullRenderer->Release();
        m_pNullRenderer = nullptr;
    }
    // m_pCapture is owned by the graph, no need to Release separately
    // unless Shutdown is called before BuildGraph finished
    if (m_pCapture) {
        m_pCapture->Release();
        m_pCapture = nullptr;
    }
    if (m_pGraph) {
        m_pGraph->Release();
        m_pGraph = nullptr;
    }

    ShutdownCOM();

    EnterCriticalSection(&m_frameCs);
    delete[] m_frameBuffer;
    m_frameBuffer = nullptr;
    m_frameSize = 0;
    m_frameReady = false;
    m_initialized = false;
    LeaveCriticalSection(&m_frameCs);
}

} // namespace facelogin
