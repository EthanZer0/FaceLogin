#pragma once

// DirectShow webcam capture — Session 0 compatible.
// Mirrors the WebcamCapture API so FaceService can use either backend.
//
// Filter graph:  [Capture] -> [SampleGrabber (RGB24)] -> [Null Renderer]
// ISampleGrabberCB::BufferCB copies frames into a shared buffer protected
// by a CRITICAL_SECTION.  GrabFrame() copies out.

#define WINVER       0x0602
#define _WIN32_WINNT 0x0602
#define _CRT_SECURE_NO_WARNINGS

#include <dlib/matrix.h>
#include <dlib/pixel.h>
#include <windows.h>
#include <dshow.h>
#include <strmif.h>
#include <uuids.h>

#include "camera_types.h"

// ISampleGrabberCB was removed from Win10+ SDK, but CLSIDs are still
// in strmiids.lib.  Declare minimal interface.
#ifndef __ISampleGrabberCB_INTERFACE_DEFINED__
#define __ISampleGrabberCB_INTERFACE_DEFINED__
struct ISampleGrabberCB : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(
        double SampleTime, IMediaSample* pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(
        double SampleTime, BYTE* pBuffer, long BufferLen) = 0;
};
static const IID IID_ISampleGrabberCB = {
    0x0579154A, 0x2B53, 0x4994, {0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85}};
#endif

// CLSID_SampleGrabber and CLSID_NullRenderer
#ifndef CLSID_SampleGrabber
static const CLSID CLSID_SampleGrabber = {
    0xC1F400A0, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
#endif
#ifndef CLSID_NullRenderer
static const CLSID CLSID_NullRenderer = {
    0xC1F400A4, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
#endif

// ISampleGrabber interface
#ifndef __ISampleGrabber_INTERFACE_DEFINED__
#define __ISampleGrabber_INTERFACE_DEFINED__
interface ISampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(
        ISampleGrabberCB* pCallback, long WhichMethodToCallback) = 0;
};
static const IID IID_ISampleGrabber = {
    0x6B652FFF, 0x11FE, 0x4fce, {0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F}};
#endif

// Helper to free an AM_MEDIA_TYPE allocated by DirectShow
inline void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat != 0) {
        CoTaskMemFree(mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

namespace facelogin {

class WebcamCaptureDS {
public:
    WebcamCaptureDS();
    ~WebcamCaptureDS();

    WebcamCaptureDS(const WebcamCaptureDS&) = delete;
    WebcamCaptureDS& operator=(const WebcamCaptureDS&) = delete;

    // devicePath: optional stable symbolic link of the camera to use.
    // Empty (default) = first enumerated device.
    bool Initialize(int preferredWidth = 1280, int preferredHeight = 720,
                    const std::wstring& devicePath = L"");
    bool IsInitialized() const { return m_initialized; }
    bool GrabFrame(dlib::matrix<dlib::rgb_pixel>& outFrame);
    bool IsFrameReady();
    void Pause();    // stop graph → camera LED off
    void Resume();   // restart graph for next auth session
    void Shutdown();

    // Camera control interfaces for the face-exposure controller (1.9.0).
    // QI'd off the capture filter during Initialize; null when the device
    // does not expose them. Borrowed pointers — valid until Shutdown().
    IAMVideoProcAmp* GetVideoProcAmp() const { return m_vpa; }
    IAMCameraControl* GetCameraControl() const { return m_cc; }

    static bool InitializeCOM();
    static void ShutdownCOM();

    // Enumerate all video capture devices via DirectShow. Requires
    // CoInitializeEx (InitializeCOM) to have been called first.
    static std::vector<CameraDeviceInfo> ListCameras();

private:
    static bool   s_comInitialized;
    static int    s_comRefCount;
    static CRITICAL_SECTION s_comCs;

    // ISampleGrabberCB nested implementation
    class GrabberCB : public ISampleGrabberCB {
    public:
        GrabberCB(WebcamCaptureDS* parent) : m_parent(parent) {}
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
        STDMETHODIMP_(ULONG) AddRef() override;
        STDMETHODIMP_(ULONG) Release() override;
        STDMETHODIMP SampleCB(double time, IMediaSample* sample) override;
        STDMETHODIMP BufferCB(double time, BYTE* buffer, long len) override;
    private:
        WebcamCaptureDS* m_parent;
        LONG m_refCount = 1;
    };

    bool FindCamera(const std::wstring& devicePath, IBaseFilter** ppFilter);
    bool BuildGraph(IBaseFilter* pCapture, int width, int height);

    IGraphBuilder*   m_pGraph        = nullptr;
    IMediaControl*   m_pControl      = nullptr;
    IBaseFilter*     m_pCapture      = nullptr;
    ISampleGrabber*  m_pGrabber      = nullptr;
    IBaseFilter*     m_pNullRenderer = nullptr;
    IAMVideoProcAmp* m_vpa           = nullptr;   // camera controls, QI'd off the filter
    IAMCameraControl* m_cc           = nullptr;

    GrabberCB m_callback;

    CRITICAL_SECTION m_frameCs;
    BYTE*  m_frameBuffer = nullptr;
    long   m_frameSize   = 0;
    bool   m_frameReady  = false;
    int    m_width       = 1280;
    int    m_height      = 720;
    bool   m_initialized = false;
};

} // namespace facelogin
