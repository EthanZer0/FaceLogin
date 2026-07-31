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

bool WebcamCapture::Initialize(int preferredWidth, int preferredHeight) {
    if (m_initialized) return true;

    m_width = preferredWidth;
    m_height = preferredHeight;

    if (!InitializeMF()) {
        FACELOGIN_ERROR(L"Failed to initialize Media Foundation");
        return false;
    }

    if (!FindFirstCamera(&m_pSource)) {
        FACELOGIN_ERROR(L"No webcam found");
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

bool WebcamCapture::FindFirstCamera(IMFMediaSource** ppSource) {
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

    FACELOGIN_INFO(L"Found %u video device(s)", count);

    hr = ppDevices[0]->ActivateObject(IID_PPV_ARGS(ppSource));

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

    FACELOGIN_WARN(L"NV12 not supported by camera, using native format");

    hr = m_pReader->SetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nullptr);
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"Failed to reset to native format: 0x%08X", hr);
        return false;
    }

    IMFMediaType* pActualType = nullptr;
    hr = m_pReader->GetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pActualType);
    if (SUCCEEDED(hr)) {
        GUID subtype;
        pActualType->GetGUID(MF_MT_SUBTYPE, &subtype);
        MFGetAttributeSize(pActualType, MF_MT_FRAME_SIZE,
                           reinterpret_cast<UINT32*>(&m_width),
                           reinterpret_cast<UINT32*>(&m_height));

        if (subtype == MFVideoFormat_YUY2) {
            m_isNV12 = false;
            FACELOGIN_INFO(L"Native format: YUY2  %dx%d", m_width, m_height);
        } else if (subtype == MFVideoFormat_MJPG) {
            FACELOGIN_WARN(L"Native format is MJPEG, requesting YUY2");
            pType = nullptr;
            MFCreateMediaType(&pType);
            pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2);
            MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, m_width, m_height);
            hr = m_pReader->SetCurrentMediaType(
                (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
            if (SUCCEEDED(hr)) {
                m_isNV12 = false;
                IMFMediaType* pYUY2Type = nullptr;
                m_pReader->GetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pYUY2Type);
                if (pYUY2Type) {
                    UINT32 yw = 0, yh = 0;
                    MFGetAttributeSize(pYUY2Type, MF_MT_FRAME_SIZE, &yw, &yh);
                    m_width = static_cast<int>(yw);
                    m_height = static_cast<int>(yh);
                    pYUY2Type->Release();
                }
            } else {
                m_isNV12 = false;
            }
            pType->Release();
        } else {
            m_isNV12 = false;
        }
        pActualType->Release();
    } else {
        m_isNV12 = false;
    }

    return true;
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
        return false;
    }

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
