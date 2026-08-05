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

    for (UINT32 row = 0; row < height; row++) {
        for (UINT32 col = 0; col < width; col++) {
            BYTE Y = yPlane[row * width + col];
            BYTE U = uvPlane[(row / 2) * width + (col & ~1)];
            BYTE V = uvPlane[(row / 2) * width + (col & ~1) + 1];

            int C = Y - 16;
            int D = U - 128;
            int E = V - 128;

            int R = (298 * C + 409 * E + 128) >> 8;
            int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int B = (298 * C + 516 * D + 128) >> 8;

            outFrame(row, col).red   = static_cast<unsigned char>(std::max(0, std::min(255, R)));
            outFrame(row, col).green = static_cast<unsigned char>(std::max(0, std::min(255, G)));
            outFrame(row, col).blue  = static_cast<unsigned char>(std::max(0, std::min(255, B)));
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
    if (m_pReader) {
        m_pReader->Release();
        m_pReader = nullptr;
    }
    if (m_pSource) {
        m_pSource->Release();
        m_pSource = nullptr;
    }
    m_initialized = false;
}

} // namespace facelogin
