#include "webcam_capture.h"
#include "../common/logger.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfobjects.h>
#include <Mferror.h>
#include <shlwapi.h>
#include <comdef.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")

namespace facelogin {

bool WebcamCapture::s_mfInitialized = false;
int WebcamCapture::s_mfRefCount = 0;

WebcamCapture::~WebcamCapture() {
    Shutdown();
}

bool WebcamCapture::InitializeMF() {
    if (!s_mfInitialized) {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (SUCCEEDED(hr)) {
            s_mfInitialized = true;
        }
    }
    if (s_mfInitialized) {
        s_mfRefCount++;
    }
    return s_mfInitialized;
}

void WebcamCapture::ShutdownMF() {
    if (s_mfRefCount > 0) {
        s_mfRefCount--;
        if (s_mfRefCount == 0 && s_mfInitialized) {
            MFShutdown();
            s_mfInitialized = false;
        }
    }
}

bool WebcamCapture::Initialize(int preferredWidth, int preferredHeight,
                               const std::wstring& devicePath) {
    if (m_initialized) return true;

    m_width = preferredWidth;
    m_height = preferredHeight;

    if (!InitializeMF()) {
        FACELOGIN_ERROR(L"Failed to initialize Media Foundation");
        return false;
    }

    if (!FindCamera(devicePath, &m_pSource)) {
        FACELOGIN_ERROR(L"No webcam found%s", devicePath.empty() ? L"" : L" for configured device");
        ShutdownMF();
        return false;
    }

    HRESULT hr = MFCreateSourceReaderFromMediaSource(
        m_pSource, nullptr, &m_pReader);

    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"Failed to create source reader: 0x%08X", hr);
        m_pSource->Release();
        m_pSource = nullptr;
        ShutdownMF();
        return false;
    }

    if (!ConfigureReader(m_width, m_height)) {
        m_pReader->Release();
        m_pReader = nullptr;
        m_pSource->Release();
        m_pSource = nullptr;
        ShutdownMF();
        return false;
    }

    m_initialized = true;
    FACELOGIN_INFO(L"Webcam initialized: %dx%d  format=%s",
                   m_width, m_height, m_isNV12 ? L"NV12" : L"YUY2");
    return true;
}

// Enumerate all video capture devices via Media Foundation. Each entry carries
// the stable symbolic link (devicePath) and the friendly display name.
std::vector<CameraDeviceInfo> WebcamCapture::ListCameras() {
    std::vector<CameraDeviceInfo> devices;

    if (!InitializeMF()) {
        FACELOGIN_ERROR(L"MF: failed to initialize for camera enumeration");
        return devices;
    }

    IMFAttributes* pAttributes = nullptr;
    HRESULT hr = MFCreateAttributes(&pAttributes, 1);
    if (SUCCEEDED(hr)) {
        pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                             MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

        IMFActivate** ppDevices = nullptr;
        UINT32 count = 0;
        hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count);
        if (SUCCEEDED(hr)) {
            for (UINT32 i = 0; i < count; i++) {
                CameraDeviceInfo info;
                LPWSTR path = nullptr, name = nullptr;
                if (SUCCEEDED(ppDevices[i]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &path, nullptr))) {
                    info.devicePath = path;
                    CoTaskMemFree(path);
                }
                if (SUCCEEDED(ppDevices[i]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, nullptr))) {
                    info.friendlyName = name;
                    CoTaskMemFree(name);
                }
                devices.push_back(std::move(info));
                ppDevices[i]->Release();
            }
            CoTaskMemFree(ppDevices);
        }
        pAttributes->Release();
    }

    ShutdownMF();
    return devices;
}

bool WebcamCapture::FindCamera(const std::wstring& devicePath,
                               IMFMediaSource** ppSource) {
    *ppSource = nullptr;

    IMFAttributes* pAttributes = nullptr;
    HRESULT hr = MFCreateAttributes(&pAttributes, 1);
    if (FAILED(hr)) return false;

    pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                         MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;

    hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count);
    pAttributes->Release();

    if (FAILED(hr) || count == 0) {
        FACELOGIN_WARN(L"No video capture devices found");
        return false;
    }

    FACELOGIN_INFO(L"Found %u video device(s), looking for %s",
                   count, devicePath.empty() ? L"first" : devicePath.c_str());

    // First pass: match the configured device by symbolic link.
    int selected = -1;
    if (!devicePath.empty()) {
        for (UINT32 i = 0; i < count; i++) {
            LPWSTR path = nullptr;
            if (SUCCEEDED(ppDevices[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &path, nullptr))) {
                bool match = (devicePath == path);
                CoTaskMemFree(path);
                if (match) { selected = static_cast<int>(i); break; }
            }
        }
        if (selected < 0) {
            FACELOGIN_WARN(L"Configured camera not found in device list — falling back to first");
        }
    }

    // Fallback: first device (existing behavior when devicePath is empty).
    if (selected < 0) selected = 0;

    hr = ppDevices[selected]->ActivateObject(IID_PPV_ARGS(ppSource));

    // Diagnostics: log which camera was actually selected (friendly name +
    // symbolic link). Helps identify a mis-picked / virtual camera when
    // enrollment or the lock-screen behaves oddly (卡90% 排查).
    {
        LPWSTR selPath = nullptr, selName = nullptr;
        if (SUCCEEDED(ppDevices[selected]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &selPath, nullptr))) {
            if (SUCCEEDED(ppDevices[selected]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &selName, nullptr))) {
                FACELOGIN_INFO(L"Selected camera: %s (%s)",
                               selName, selPath);
            } else {
                FACELOGIN_INFO(L"Selected camera: %s", selPath);
            }
            CoTaskMemFree(selPath);
            if (selName) CoTaskMemFree(selName);
        }
    }

    for (UINT32 i = 0; i < count; i++) {
        ppDevices[i]->Release();
    }
    CoTaskMemFree(ppDevices);

    return SUCCEEDED(hr);
}

bool WebcamCapture::ConfigureReader(int width, int height) {
    IMFMediaType* pType = nullptr;
    HRESULT hr = MFCreateMediaType(&pType);
    if (FAILED(hr)) return false;

    pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, 30, 1);

    hr = m_pReader->SetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
    pType->Release();

    if (SUCCEEDED(hr)) {
        m_isNV12 = true;
        FACELOGIN_INFO(L"Camera configured for NV12");
        return true;
    }

    // NV12 was rejected. There is no "reset to native format" form of
    // SetCurrentMediaType: pMediaType must be a valid IMFMediaType, and passing
    // NULL fails with E_INVALIDARG (0x80070057). A failed SetCurrentMediaType
    // leaves the camera's native type in effect, so the robust fallback is to
    // pick a *native* type we know how to convert (NV12/YUY2) and set it
    // directly — a native type is always accepted.
    FACELOGIN_WARN(L"NV12 not supported by camera (hr=0x%08X), falling back", hr);

    IMFMediaType* pBestNative = nullptr;
    UINT32 bestW = 0, bestH = 0;
    bool bestIsNV12 = false;
    UINT32 mjpgW = 0, mjpgH = 0;   // last-resort MJPEG size

    for (DWORD i = 0; ; ++i) {
        IMFMediaType* pNative = nullptr;
        hr = m_pReader->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &pNative);
        if (FAILED(hr)) break;   // MF_E_NO_MORE_TYPES ends the list

        GUID subtype = GUID_NULL;
        UINT32 w = 0, h = 0;
        pNative->GetGUID(MF_MT_SUBTYPE, &subtype);
        MFGetAttributeSize(pNative, MF_MT_FRAME_SIZE, &w, &h);

        if (subtype == MFVideoFormat_NV12) {
            if (pBestNative) pBestNative->Release();
            pBestNative = pNative;   // transfer ownership
            bestW = w; bestH = h; bestIsNV12 = true;
            break;                   // NV12 is the best possible outcome
        }
        if (subtype == MFVideoFormat_YUY2 && !pBestNative) {
            pBestNative = pNative;   // transfer ownership; keep scanning for NV12
            bestW = w; bestH = h; bestIsNV12 = false;
            continue;
        }
        if (subtype == MFVideoFormat_MJPG && !mjpgW) {
            mjpgW = w; mjpgH = h;
        }
        pNative->Release();
    }

    if (pBestNative) {
        hr = m_pReader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pBestNative);
        if (SUCCEEDED(hr)) {
            m_isNV12 = bestIsNV12;
            m_width = static_cast<int>(bestW);
            m_height = static_cast<int>(bestH);
            pBestNative->Release();
            FACELOGIN_INFO(L"Camera configured for %s %dx%d",
                           m_isNV12 ? L"NV12" : L"YUY2", m_width, m_height);
            return true;
        }
        pBestNative->Release();
    }

    // Camera only offers compressed MJPEG: ask the reader to decode it to YUY2.
    if (mjpgW) {
        IMFMediaType* pYuy2 = nullptr;
        hr = MFCreateMediaType(&pYuy2);
        if (SUCCEEDED(hr)) {
            pYuy2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            pYuy2->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2);
            MFSetAttributeSize(pYuy2, MF_MT_FRAME_SIZE, mjpgW, mjpgH);
            hr = m_pReader->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pYuy2);
            pYuy2->Release();
        }
        if (SUCCEEDED(hr)) {
            m_isNV12 = false;
            m_width = static_cast<int>(mjpgW);
            m_height = static_cast<int>(mjpgH);
            FACELOGIN_INFO(L"MJPEG camera decoding to YUY2 %dx%d", m_width, m_height);
            return true;
        }
        FACELOGIN_ERROR(L"MJPEG camera: failed to configure YUY2 decode output (0x%08X)", hr);
        return false;
    }

    FACELOGIN_ERROR(L"No supported camera format (NV12/YUY2/MJPEG)");
    return false;
}

bool WebcamCapture::GrabFrame(dlib::matrix<dlib::rgb_pixel>& outFrame) {
    if (!m_initialized || !m_pReader) return false;

    DWORD streamIndex, flags;
    LONGLONG timestamp;
    IMFSample* pSample = nullptr;

    HRESULT hr = m_pReader->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
        &streamIndex, &flags, &timestamp, &pSample);

    if (FAILED(hr) || !pSample) {
        // A device stall after resume makes ReadSample fail repeatedly on a
        // stale SourceReader. A few consecutive failures means it's dead, not
        // a transient glitch — self-shutdown so the caller re-inits fresh.
        if (++m_consecutiveFailures >= kMaxConsecutiveGrabFailures) {
            FACELOGIN_WARN(L"GrabFrame failed %d times — camera stalled, releasing for re-init",
                           m_consecutiveFailures);
            m_consecutiveFailures = 0;
            Shutdown();
        }
        return false;
    }
    m_consecutiveFailures = 0;

    bool result = false;
    if (m_isNV12) {
        result = ConvertNV12toRGB(pSample, outFrame);
    } else {
        result = ConvertYUY2toRGB(pSample, outFrame);
    }
    pSample->Release();
    return result;
}

bool WebcamCapture::IsFrameReady() {
    if (!m_initialized || !m_pReader) return false;

    DWORD streamIndex, flags;
    LONGLONG timestamp;
    IMFSample* pSample = nullptr;

    HRESULT hr = m_pReader->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
        &streamIndex, &flags, &timestamp, &pSample);

    if (pSample) pSample->Release();
    return SUCCEEDED(hr) && (flags & static_cast<DWORD>(MF_SOURCE_READERF_STREAMTICK)) == 0;
}

bool WebcamCapture::ConvertNV12toRGB(IMFSample* pSample,
                                      dlib::matrix<dlib::rgb_pixel>& outFrame) {
    IMFMediaBuffer* pBuffer = nullptr;
    HRESULT hr = pSample->ConvertToContiguousBuffer(&pBuffer);
    if (FAILED(hr)) return false;

    BYTE* pData = nullptr;
    DWORD maxLen = 0, curLen = 0;
    hr = pBuffer->Lock(&pData, &maxLen, &curLen);
    if (FAILED(hr)) {
        pBuffer->Release();
        return false;
    }

    IMFMediaType* pType = nullptr;
    UINT32 width = 1280, height = 720;
    hr = m_pReader->GetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pType);
    if (SUCCEEDED(hr)) {
        MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &width, &height);
        pType->Release();
    }

    m_width = static_cast<int>(width);
    m_height = static_cast<int>(height);

    const BYTE* yPlane = pData;
    const BYTE* uvPlane = pData + (width * height);

    outFrame.set_size(height, width);

    // YUV→RGB is a fixed linear combination of Y/U/V. Precompute the
    // chroma-only contribution (NOT shifted — the >>8 must stay together
    // with the luma term so the result is bit-identical to the original
    // scalar formula) for every possible (U,V) pair. This removes the
    // per-pixel chroma multiplications and max/min clamps from the hot loop
    // — for 1280×720 that is ~920k pixels × 3 channels every frame.
    //   R = (298·C + 409·E + 128)>>8
    //   G = (298·C −100·D −208·E + 128)>>8
    //   B = (298·C + 516·D + 128)>>8     with C=Y−16, D=U−128, E=V−128
    static int rUV[256][256], gUV[256][256], bUV[256][256];
    static const bool sUVBuilt = [] {
        for (int u = 0; u < 256; u++) {
            int D = u - 128;
            for (int v = 0; v < 256; v++) {
                int E = v - 128;
                rUV[u][v] = 409 * E + 128;
                gUV[u][v] = -100 * D - 208 * E + 128;
                bUV[u][v] = 516 * D + 128;
            }
        }
        return true;
    }();

    for (UINT32 row = 0; row < height; row++) {
        dlib::rgb_pixel* dstRow = &outFrame(row, 0);
        const BYTE* yRow = yPlane + row * width;
        const BYTE* uvRow = uvPlane + (row >> 1) * width;

        for (UINT32 col = 0; col < width; col++) {
            BYTE Y = yRow[col];
            int C = static_cast<int>(Y) - 16;
            // NV12 chroma is subsampled 2x2: for luma (row,col) the chroma
            // position is (row/2, col/2). Each (U,V) covers a 2x1 luma pair
            // at the same row, so even col shares the sample with col+1.
            BYTE U = uvRow[col & ~1];
            BYTE V = uvRow[(col & ~1) + 1];

            const int rv = (298 * C + rUV[U][V]) >> 8;
            const int gv = (298 * C + gUV[U][V]) >> 8;
            const int bv = (298 * C + bUV[U][V]) >> 8;

            dstRow[col].red   = static_cast<unsigned char>(rv < 0 ? 0 : (rv > 255 ? 255 : rv));
            dstRow[col].green = static_cast<unsigned char>(gv < 0 ? 0 : (gv > 255 ? 255 : gv));
            dstRow[col].blue  = static_cast<unsigned char>(bv < 0 ? 0 : (bv > 255 ? 255 : bv));
        }
    }

    pBuffer->Unlock();
    pBuffer->Release();
    return true;
}

bool WebcamCapture::ConvertYUY2toRGB(IMFSample* pSample,
                                      dlib::matrix<dlib::rgb_pixel>& outFrame) {
    IMFMediaBuffer* pBuffer = nullptr;
    HRESULT hr = pSample->ConvertToContiguousBuffer(&pBuffer);
    if (FAILED(hr)) return false;

    BYTE* pData = nullptr;
    DWORD maxLen = 0, curLen = 0;
    hr = pBuffer->Lock(&pData, &maxLen, &curLen);
    if (FAILED(hr)) {
        pBuffer->Release();
        return false;
    }

    IMFMediaType* pType = nullptr;
    UINT32 width = 1280, height = 720;
    hr = m_pReader->GetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pType);
    if (SUCCEEDED(hr)) {
        MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &width, &height);
        pType->Release();
    }

    m_width = static_cast<int>(width);
    m_height = static_cast<int>(height);

    outFrame.set_size(height, width);

    for (UINT32 row = 0; row < height; row++) {
        for (UINT32 col = 0; col < width; col += 2) {
            int offset = row * width * 2 + col * 2;

            BYTE Y0 = pData[offset];
            BYTE U  = pData[offset + 1];
            BYTE Y1 = pData[offset + 2];
            BYTE V  = pData[offset + 3];

            int C = Y0 - 16;
            int D = U - 128;
            int E = V - 128;
            outFrame(row, col).red   = static_cast<unsigned char>(std::clamp((298 * C + 409 * E + 128) >> 8, 0, 255));
            outFrame(row, col).green = static_cast<unsigned char>(std::clamp((298 * C - 100 * D - 208 * E + 128) >> 8, 0, 255));
            outFrame(row, col).blue  = static_cast<unsigned char>(std::clamp((298 * C + 516 * D + 128) >> 8, 0, 255));

            if (col + 1 < static_cast<UINT32>(width)) {
                C = Y1 - 16;
                outFrame(row, col + 1).red   = static_cast<unsigned char>(std::clamp((298 * C + 409 * E + 128) >> 8, 0, 255));
                outFrame(row, col + 1).green = static_cast<unsigned char>(std::clamp((298 * C - 100 * D - 208 * E + 128) >> 8, 0, 255));
                outFrame(row, col + 1).blue  = static_cast<unsigned char>(std::clamp((298 * C + 516 * D + 128) >> 8, 0, 255));
            }
        }
    }

    pBuffer->Unlock();
    pBuffer->Release();
    return true;
}

void WebcamCapture::Shutdown() {
    // Force-close the media source BEFORE releasing our references. A blocked
    // synchronous ReadSample (e.g. the camera was just taken over by the
    // credential provider at lock) would otherwise keep the source referenced
    // and the caller's thread join would hang forever. IMFMediaSource::Shutdown
    // makes an in-flight ReadSample return MF_E_SHUTDOWN so the frame thread
    // can exit cleanly.
    //
    // The teardown itself is BOUNDED: a wedged camera driver can make
    // IMFMediaSource::Shutdown() block forever, and StopPreview() runs on the
    // UI thread — an unbounded wait here freezes the whole console
    // (卡90%无响应 on machines with such drivers). The actual MF calls run on a
    // detached worker that captures the interfaces by value, and we wait at
    // most kShutdownBudgetMs. If the driver is unresponsive the worker keeps
    // running in the background (worst case it leaks until process exit) while
    // the caller — and with it the UI thread — moves on.
    //
    // The pointers are cleared synchronously so GrabFrame() bails immediately
    // and the frame thread's re-init logic never touches a torn-down camera.
    IMFMediaSource* src = m_pSource;
    IMFSourceReader* rdr = m_pReader;
    m_pSource = nullptr;
    m_pReader = nullptr;
    m_initialized = false;
    m_consecutiveFailures = 0;

    if (!src) return;

    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([src, rdr, done]() {
        if (src) src->Shutdown();   // wakes an in-flight ReadSample (MF_E_SHUTDOWN)
        if (rdr) rdr->Release();
        if (src) src->Release();
        done->store(true, std::memory_order_release);
    });
    worker.detach();

    constexpr int kShutdownBudgetMs = 1500;
    auto start = std::chrono::steady_clock::now();
    while (!done->load(std::memory_order_acquire)) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= kShutdownBudgetMs) {
            FACELOGIN_WARN(L"Webcam MF teardown did not finish within %d ms — "
                           L"driver unresponsive, completing in background",
                           kShutdownBudgetMs);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace facelogin
