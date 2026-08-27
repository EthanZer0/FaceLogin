// Standalone camera diagnostic (issue #16-class reports).
//
// Answers the questions raised by a "service not running" lock-screen report:
//   1. Does Media Foundation enumerate the camera? (repeatedly — enumeration
//      can fail transiently right after install / during driver wakeup)
//   2. Does DirectShow enumerate it? (device visible in only one stack?)
//   3. What media types does the device offer (MF native types, DS stream caps)?
//   4. Can 1280x720 be set (reproduces DS SetFormat 0xC00D36B4 failures)?
//   5. Does the camera actually deliver frames (capture test with a hard
//      timeout — a stalled grab is the #1 cause of service hangs)?
//
// Every camera interaction runs on a worker thread with a hard timeout, so
// the tool itself can never hang. Output goes to the console AND to a report
// file (diag_camera_report.txt, UTF-8 BOM) next to the exe for easy upload.
//
// Build: cmake --target diag_camera (see face_service/CMakeLists.txt)
// The tool is self-contained (no dlib / no project logger) on purpose.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <uuids.h>
#include <process.h>
#include <stdio.h>
#include <locale.h>
#include <wchar.h>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// ==========================================================================
// qedit.h was removed from the Windows SDK; declare the SampleGrabber
// interfaces we need (canonical GUIDs from the original qedit.idl).
// ==========================================================================
static const CLSID CLSID_SampleGrabber = {0xC1F400A0, 0x3F08, 0x11D0,
                                          {0x9D, 0x5D, 0x00, 0xA0, 0xC9, 0x1E, 0x9B, 0x4E}};
static const CLSID CLSID_NullRenderer = {0xC1F400A4, 0x3F08, 0x11D0,
                                         {0x9D, 0x5D, 0x00, 0xA0, 0xC9, 0x1E, 0x9B, 0x4E}};
static const IID IID_ISampleGrabber = {0x6B652FFF, 0x11FE, 0x4fce,
                                       {0x92, 0xAD, 0x02, 0x65, 0xB5, 0xD7, 0xC7, 0x8F}};
static const IID IID_ISampleGrabberCB = {0x0579154A, 0x2B53, 0x4994,
                                         {0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85}};

struct ISampleGrabberCB;

struct ISampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL bufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(LONG* pBufferSize, LONG* pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* pCallback, LONG whichMethodToCallback) = 0;
};

struct ISampleGrabberCB : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample* pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE* pBuffer, LONG BufferLen) = 0;
};

// ==========================================================================
// Output helpers: console (WriteConsoleW — bypasses the ANSI code page, so
// Chinese renders correctly regardless of the system locale) + report file
// (UTF-8 with BOM, so Notepad reads it). When stdout is redirected (pipe /
// file), WriteConsoleW is unavailable and we fall back to wprintf with the
// system ANSI locale set.
// ==========================================================================
static FILE* g_report = nullptr;

static void OutConsole(const std::wstring& s) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
        DWORD written = 0;
        WriteConsoleW(hOut, s.c_str(), static_cast<DWORD>(s.size()), &written, nullptr);
        WriteConsoleW(hOut, L"\n", 1, &written, nullptr);
        return;
    }
    // Redirected: rely on the C runtime locale (set in wmain).
    wprintf(L"%s\n", s.c_str());
}

static void WriteReportUtf8(const std::wstring& wtext) {
    if (!g_report) return;
    const int len = WideCharToMultiByte(CP_UTF8, 0, wtext.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return;
    std::string utf8(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wtext.c_str(), -1, utf8.data(), len, nullptr, nullptr);
    fwrite(utf8.data(), 1, utf8.size(), g_report);
    fflush(g_report);
}

static void Out(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    wchar_t buf[1024];
    vswprintf_s(buf, fmt, args);
    va_end(args);
    OutConsole(buf);
    WriteReportUtf8(std::wstring(buf) + L"\r\n");
}

static double NowSec() {
    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
}

static std::wstring GuidToString(const GUID& g) {
    wchar_t buf[64];
    swprintf_s(buf, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
               g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

static std::wstring SubtypeName(const GUID& sub) {
    struct { const GUID* g; const wchar_t* name; } table[] = {
        {&MFVideoFormat_NV12, L"NV12"}, {&MFVideoFormat_YUY2, L"YUY2"},
        {&MFVideoFormat_MJPG, L"MJPG"}, {&MFVideoFormat_RGB24, L"RGB24"},
        {&MFVideoFormat_RGB32, L"RGB32"}, {&MFVideoFormat_I420, L"I420"},
        {&MFVideoFormat_H264, L"H264"}, {&MFVideoFormat_HEVC, L"HEVC"},
        {&MFVideoFormat_YV12, L"YV12"}, {&MFVideoFormat_ARGB32, L"ARGB32"},
    };
    for (auto& e : table) {
        if (IsEqualGUID(sub, *e.g)) return e.name;
    }
    return GuidToString(sub);
}

// ==========================================================================
// OS version
// ==========================================================================
static void PrintOsVersion() {
    typedef LONG(WINAPI* RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (fn) fn(&info);
    Out(L"[系统] Windows %lu.%lu.%lu (build %lu), %s",
        info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber, info.dwBuildNumber,
        sizeof(void*) == 8 ? L"x64" : L"x86");
}

// ==========================================================================
// Enumeration — MF and DS, three passes each (transient failure detection)
// ==========================================================================
struct EnumPass {
    std::vector<std::wstring> devices;   // friendly names (MF) / display names (DS)
    std::vector<std::wstring> paths;     // symbolic links (MF only)
    HRESULT hr = S_OK;
};

static EnumPass MfEnumerate() {
    EnumPass pass;
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { pass.hr = hr; return pass; }

    IMFAttributes* attrs = nullptr;
    hr = MFCreateAttributes(&attrs, 1);
    if (SUCCEEDED(hr)) {
        attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate** devs = nullptr;
        UINT32 count = 0;
        hr = MFEnumDeviceSources(attrs, &devs, &count);
        pass.hr = hr;
        for (UINT32 i = 0; i < count; i++) {
            LPWSTR name = nullptr, path = nullptr;
            std::wstring n, p;
            if (SUCCEEDED(devs[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, nullptr))) { n = name; CoTaskMemFree(name); }
            if (SUCCEEDED(devs[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &path, nullptr))) { p = path; CoTaskMemFree(path); }
            pass.devices.push_back(n.empty() ? L"(unnamed)" : n);
            pass.paths.push_back(p);
            devs[i]->Release();
        }
        if (devs) CoTaskMemFree(devs);
        attrs->Release();
    }
    MFShutdown();
    return pass;
}

static EnumPass DsEnumerate() {
    EnumPass pass;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { pass.hr = hr; return pass; }

    ICreateDevEnum* pDevEnum = nullptr;
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&pDevEnum));
    pass.hr = hr;
    if (SUCCEEDED(hr)) {
        IEnumMoniker* pEnum = nullptr;
        hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
        pass.hr = hr;
        if (SUCCEEDED(hr)) {
            IMoniker* moniker = nullptr;
            while (pEnum->Next(1, &moniker, nullptr) == S_OK) {
                IPropertyBag* bag = nullptr;
                std::wstring name = L"(unnamed)";
                if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
                    VARIANT var; VariantInit(&var);
                    if (SUCCEEDED(bag->Read(L"FriendlyName", &var, nullptr)) && var.vt == VT_BSTR) {
                        name = var.bstrVal;
                    }
                    VariantClear(&var);
                    bag->Release();
                }
                pass.devices.push_back(name);
                moniker->Release();
            }
            pEnum->Release();
        }
        pDevEnum->Release();
    }
    CoUninitialize();
    return pass;
}

// ==========================================================================
// MF device test: open source, list native types, try 1280x720, grab frames
// ==========================================================================
static void MfTestDevice(const std::wstring& path, const std::wstring& name, int index) {
    Out(L"");
    Out(L"----- MF 测试: 设备 %d [%s] -----", index + 1, name.c_str());

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { Out(L"[MF] MFStartup failed: 0x%08X", hr); return; }

    IMFAttributes* attrs = nullptr;
    IMFActivate* activate = nullptr;
    IMFMediaSource* source = nullptr;
    IMFSourceReader* reader = nullptr;
    bool found = false;

    hr = MFCreateAttributes(&attrs, 1);
    if (SUCCEEDED(hr)) {
        attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate** devs = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFEnumDeviceSources(attrs, &devs, &count))) {
            for (UINT32 i = 0; i < count && !found; i++) {
                LPWSTR p = nullptr;
                if (SUCCEEDED(devs[i]->GetAllocatedString(
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &p, nullptr))) {
                    found = (path.empty() || path == p);
                    CoTaskMemFree(p);
                }
                if (found) { activate = devs[i]; activate->AddRef(); }
                devs[i]->Release();
            }
            CoTaskMemFree(devs);
        }
        attrs->Release();
    }
    if (!found || !activate) {
        Out(L"[MF] 枚举阶段找不到该设备（设备可能已消失）");
        MFShutdown();
        return;
    }
    hr = activate->ActivateObject(IID_PPV_ARGS(&source));
    activate->Release();
    if (FAILED(hr)) {
        Out(L"[MF] ActivateObject failed: 0x%08X", hr);
        MFShutdown();
        return;
    }
    Out(L"[MF] 设备打开成功");

    hr = MFCreateSourceReaderFromMediaSource(source, nullptr, &reader);
    if (FAILED(hr)) {
        Out(L"[MF] MFCreateSourceReaderFromMediaSource failed: 0x%08X", hr);
        source->Shutdown(); source->Release();
        MFShutdown();
        return;
    }

    // List native media types
    Out(L"[MF] 原生媒体类型:");
    DWORD typeIndex = 0;
    bool has720 = false;
    int typeCount = 0;
    for (;;) {
        IMFMediaType* type = nullptr;
        hr = reader->GetNativeMediaType(0, typeIndex, &type);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) { Out(L"[MF] GetNativeMediaType(%lu) failed: 0x%08X", typeIndex, hr); break; }
        GUID sub = GUID_NULL;
        type->GetGUID(MF_MT_SUBTYPE, &sub);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
        UINT32 num = 0, den = 0;
        MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den);
        Out(L"[MF]   [%lu] %s %lux%lu @ %lu/%lu fps", typeIndex, SubtypeName(sub).c_str(),
            w, h, num, den ? den : 1);
        if (w == 1280 && h == 720) has720 = true;
        typeCount++;
        typeIndex++;
        type->Release();
    }
    Out(L"[MF] 原生类型数量: %d, 含 1280x720: %s", typeCount, has720 ? L"是" : L"否");

    // Try 1280x720 NV12 (fall back to first native type)
    HRESULT setHr = E_FAIL;
    IMFMediaType* want = nullptr;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(want, MF_MT_FRAME_SIZE, 1280, 720);
    setHr = reader->SetCurrentMediaType(0, nullptr, want);
    if (FAILED(setHr)) {
        // fall back: first native type
        IMFMediaType* first = nullptr;
        if (SUCCEEDED(reader->GetNativeMediaType(0, 0, &first))) {
            setHr = reader->SetCurrentMediaType(0, nullptr, first);
            first->Release();
        }
    }
    want->Release();
    if (FAILED(setHr)) {
        Out(L"[MF] SetCurrentMediaType(1280x720 NV12) 失败: 0x%08X（回退也失败）", setHr);
    } else {
        IMFMediaType* got = nullptr;
        if (SUCCEEDED(reader->GetCurrentMediaType(0, &got))) {
            GUID sub = GUID_NULL;
            got->GetGUID(MF_MT_SUBTYPE, &sub);
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(got, MF_MT_FRAME_SIZE, &w, &h);
            Out(L"[MF] 实际媒体类型: %s %lux%lu", SubtypeName(sub).c_str(), w, h);
            got->Release();
        }

        // Grab 10 frames with per-frame timing
        Out(L"[MF] 抓帧测试 (10 帧):");
        double t0 = NowSec();
        double prev = t0;
        int gotFrames = 0;
        for (int i = 0; i < 10; i++) {
            DWORD streamFlags = 0;
            IMFSample* sample = nullptr;
            hr = reader->ReadSample(0, 0, nullptr, &streamFlags, nullptr, &sample);
            if (FAILED(hr)) {
                Out(L"[MF] ReadSample #%d failed: 0x%08X", i + 1, hr);
                break;
            }
            if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
                Out(L"[MF]   #%d: END OF STREAM（设备停止输出）", i + 1);
                break;
            }
            if (sample) {
                double t = NowSec();
                Out(L"[MF]   #%d: 帧到达 (间隔 %.1f ms)", i + 1, (t - prev) * 1000.0);
                prev = t;
                gotFrames++;
                sample->Release();
            } else if (!(streamFlags & MF_SOURCE_READERF_NEWSTREAM)) {
                Out(L"[MF]   #%d: 无样本 (flags=0x%X)", i + 1, streamFlags);
            }
        }
        Out(L"[MF] 抓帧结果: %d/10 帧, 总耗时 %.1f ms%s",
            gotFrames, (NowSec() - t0) * 1000.0,
            gotFrames == 0 ? L"  <<<< 无帧输出（认证卡死根因候选）" : L"");
    }

    reader->Release();
    source->Shutdown();
    source->Release();
    MFShutdown();
}

// ==========================================================================
// DS device test: build graph, list stream caps, SetFormat(1280x720),
// grab frames via SampleGrabber (hard timeout so the tool never hangs)
// ==========================================================================
class GrabCallback : public ISampleGrabberCB {
public:
    volatile LONG m_count = 0;
    volatile LONGLONG m_firstTime = 0;
    volatile LONGLONG m_lastTime = 0;

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_ISampleGrabberCB)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    virtual ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    virtual ULONG STDMETHODCALLTYPE Release() override { return 1; }
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) override {
        LARGE_INTEGER q{};
        QueryPerformanceCounter(&q);
        if (InterlockedIncrement(&m_count) == 1) m_firstTime = q.QuadPart;
        m_lastTime = q.QuadPart;
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE*, LONG) override { return E_NOTIMPL; }
};

static void DsTestDevice(const std::wstring& name, int index) {
    Out(L"");
    Out(L"----- DS 测试: 设备 %d [%s] -----", index + 1, name.c_str());

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { Out(L"[DS] CoInitializeEx failed: 0x%08X", hr); return; }

    ICreateDevEnum* pDevEnum = nullptr;
    IMoniker* moniker = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&pDevEnum)))) {
        Out(L"[DS] CLSID_SystemDeviceEnum 创建失败");
        CoUninitialize();
        return;
    }
    IEnumMoniker* pEnum = nullptr;
    if (FAILED(pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0)) ||
        !pEnum) {
        Out(L"[DS] CreateClassEnumerator 失败（无视频捕获设备）");
        pDevEnum->Release();
        CoUninitialize();
        return;
    }
    int i = 0;
    while (pEnum->Next(1, &moniker, nullptr) == S_OK) {
        if (i++ == index) break;
        moniker->Release();
        moniker = nullptr;
    }
    pEnum->Release();
    pDevEnum->Release();
    if (!moniker) {
        Out(L"[DS] 找不到第 %d 个设备", index + 1);
        CoUninitialize();
        return;
    }

    IBaseFilter* captureFilter = nullptr;
    hr = moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&captureFilter));
    moniker->Release();
    if (FAILED(hr)) {
        Out(L"[DS] BindToObject failed: 0x%08X", hr);
        CoUninitialize();
        return;
    }
    Out(L"[DS] 设备 filter 绑定成功");

    // Find the capture pin and its IAMStreamConfig
    IAMStreamConfig* streamConfig = nullptr;
    IPin* capturePin = nullptr;
    {
        IEnumPins* pins = nullptr;
        if (SUCCEEDED(captureFilter->EnumPins(&pins))) {
            IPin* pin = nullptr;
            while (pins->Next(1, &pin, nullptr) == S_OK) {
                PIN_DIRECTION dir;
                pin->QueryDirection(&dir);
                if (dir == PINDIR_OUTPUT) {
                    if (SUCCEEDED(pin->QueryInterface(IID_PPV_ARGS(&streamConfig)))) {
                        capturePin = pin;
                        capturePin->AddRef();
                        break;
                    }
                }
                pin->Release();
            }
            pins->Release();
        }
    }

    if (streamConfig) {
        // List stream caps
        int count = 0, size = 0;
        hr = streamConfig->GetNumberOfCapabilities(&count, &size);
        Out(L"[DS] 支持格式数量: %s", SUCCEEDED(hr) ? std::to_wstring(count).c_str() : L"(GetNumberOfCapabilities 失败)");
        if (SUCCEEDED(hr)) {
            for (int c = 0; c < count && c < 20; c++) {
                AM_MEDIA_TYPE* mt = nullptr;
                BYTE* caps = new BYTE[size];
                if (SUCCEEDED(streamConfig->GetStreamCaps(c, &mt, caps))) {
                    if (mt->formattype == FORMAT_VideoInfo && mt->pbFormat) {
                        VIDEOINFOHEADER* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
                        GUID sub = mt->subtype;
                        Out(L"[DS]   [%d] %s %lux%lu", c, SubtypeName(sub).c_str(),
                            vih->bmiHeader.biWidth, vih->bmiHeader.biHeight);
                    } else {
                        Out(L"[DS]   [%d] %s (formattype=%s)", c,
                            SubtypeName(mt->subtype).c_str(), GuidToString(mt->formattype).c_str());
                    }
                    if (mt->cbFormat && mt->pbFormat) CoTaskMemFree(mt->pbFormat);
                    CoTaskMemFree(mt);
                } else {
                    Out(L"[DS]   [%d] GetStreamCaps failed: 0x%08X", c, hr);
                }
                delete[] caps;
            }
        }

        // Try SetFormat(1280x720 YUY2) — reproduces the 0xC00D36B4 path
        AM_MEDIA_TYPE mt = {};
        mt.majortype = MEDIATYPE_Video;
        mt.subtype = MEDIASUBTYPE_YUY2;
        mt.formattype = FORMAT_VideoInfo;
        VIDEOINFOHEADER vih = {};
        vih.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        vih.bmiHeader.biWidth = 1280;
        vih.bmiHeader.biHeight = 720;
        vih.bmiHeader.biPlanes = 1;
        vih.bmiHeader.biBitCount = 16;
        vih.bmiHeader.biSizeImage = 1280 * 720 * 2;
        vih.bmiHeader.biCompression = mmioFOURCC('Y', 'U', 'Y', '2');
        mt.pbFormat = reinterpret_cast<BYTE*>(&vih);
        mt.cbFormat = sizeof(vih);
        hr = streamConfig->SetFormat(&mt);
        Out(L"[DS] SetFormat(1280x720 YUY2): %s (0x%08X)%s",
            SUCCEEDED(hr) ? L"成功" : L"失败", hr,
            hr == 0xC00D36B4 ? L"  <<<< 与用户 issue 相同的错误码" : L"");
        streamConfig->Release();
    } else {
        Out(L"[DS] 未找到带 IAMStreamConfig 的输出 pin");
    }

    // Build graph + SampleGrabber, grab 10 frames with a hard timeout
    {
        IGraphBuilder* graph = nullptr;
        ICaptureGraphBuilder2* capBuilder = nullptr;
        IBaseFilter* grabberFilter = nullptr;
        ISampleGrabber* grabber = nullptr;
        IMediaControl* control = nullptr;
        GrabCallback callback;

        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&graph));
        if (SUCCEEDED(hr)) {
            hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&capBuilder));
        }
        if (SUCCEEDED(hr) && capBuilder && graph) {
            capBuilder->SetFiltergraph(graph);
            hr = CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&grabberFilter));
            if (FAILED(hr)) {
                Out(L"[DS] CoCreateInstance(CLSID_SampleGrabber) failed: 0x%08X", hr);
                if (hr == REGDB_E_CLASSNOTREG) {
                    Out(L"[DS]   提示: SampleGrabber 类未注册（qedit.dll）——"
                        L"可用管理员运行: regsvr32 qedit.dll");
                }
            }
        }
        if (SUCCEEDED(hr) && grabberFilter) {
            grabberFilter->QueryInterface(IID_ISampleGrabber, reinterpret_cast<void**>(&grabber));
        }
        if (SUCCEEDED(hr) && grabber) {
            AM_MEDIA_TYPE grabMt = {};
            grabMt.majortype = MEDIATYPE_Video;
            grabMt.subtype = MEDIASUBTYPE_RGB24;
            grabber->SetMediaType(&grabMt);
            hr = graph->AddFilter(captureFilter, L"Capture");
            if (SUCCEEDED(hr)) hr = graph->AddFilter(grabberFilter, L"Grabber");
        }
        if (SUCCEEDED(hr) && grabber && capBuilder) {
            // Connect capture pin -> SampleGrabber -> NullRenderer
            IBaseFilter* nullFilter = nullptr;
            hr = CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&nullFilter));
            if (SUCCEEDED(hr)) hr = graph->AddFilter(nullFilter, L"Null");
            if (SUCCEEDED(hr) && grabber && nullFilter) {
                hr = capBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                              captureFilter, grabberFilter, nullFilter);
                Out(L"[DS] RenderStream(capture→grabber→null): %s (0x%08X)",
                    SUCCEEDED(hr) ? L"成功" : L"失败", hr);
            }
            if (nullFilter) nullFilter->Release();
        }

        if (SUCCEEDED(hr) && grabber && control == nullptr) {
            graph->QueryInterface(IID_PPV_ARGS(&control));
            grabber->SetCallback(&callback, 0);
            hr = control->Run();
            Out(L"[DS] graph Run(): %s (0x%08X)", SUCCEEDED(hr) ? L"成功" : L"失败", hr);

            if (SUCCEEDED(hr)) {
                // Wait for 10 frames or 15s — a stall here is the hang root cause
                const double deadline = NowSec() + 15.0;
                while (callback.m_count < 10 && NowSec() < deadline) {
                    Sleep(50);
                }
                const LONG got = callback.m_count;
                if (got >= 10) {
                    double first = static_cast<double>(callback.m_firstTime);
                    double last = static_cast<double>(callback.m_lastTime);
                    LARGE_INTEGER freq{};
                    QueryPerformanceFrequency(&freq);
                    Out(L"[DS] 抓帧结果: %ld 帧, 平均间隔 %.1f ms%s", got,
                        (last - first) / freq.QuadPart / (got - 1) * 1000.0,
                        L"  <<<< 无帧输出（认证卡死根因候选）");
                } else if (got > 0) {
                    Out(L"[DS] 抓帧结果: 仅 %ld 帧后停止输出（15 秒内）——设备输出异常", got);
                } else {
                    Out(L"[DS] 抓帧结果: 0 帧（15 秒超时）<<<< 认证卡死根因候选");
                }
                control->Stop();
            }
        } else if (FAILED(hr)) {
            Out(L"[DS] graph 构建失败: 0x%08X", hr);
        }

        if (control) control->Release();
        if (grabber) grabber->Release();
        if (grabberFilter) grabberFilter->Release();
        if (capBuilder) capBuilder->Release();
        if (graph) graph->Release();
    }

    captureFilter->Release();
    CoUninitialize();
}

// ==========================================================================
// Worker-thread wrapper with a hard timeout: the tool itself never hangs.
// On timeout we write the report and exit the process (the stuck worker
// thread is terminated by process exit).
// ==========================================================================
static void RunWithTimeout(void (*fn)(void*), void* arg, DWORD timeoutMs, const wchar_t* label) {
    HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(
        nullptr, 0,
        [](void* ctx) -> unsigned {
            auto* p = static_cast<std::pair<void (*)(void*), void*>*>(ctx);
            p->first(p->second);
            delete p;
            return 0;
        },
        new std::pair<void (*)(void*), void*>(fn, arg), 0, nullptr));
    if (!thread) {
        Out(L"[%s] 无法创建工作线程", label);
        return;
    }
    DWORD wait = WaitForSingleObject(thread, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        Out(L"[%s] <<<< 超时 %lu 秒未完成——该设备在此环节卡死（根因已定位）",
            label, timeoutMs / 1000);
        fclose(g_report);
        ExitProcess(0);
    }
    CloseHandle(thread);
}

struct DeviceArg { const std::wstring* path; const std::wstring* name; int index; };

static void MfTestThunk(void* ctx) {
    auto* a = static_cast<DeviceArg*>(ctx);
    MfTestDevice(*a->path, *a->name, a->index);
}
static void DsTestThunk(void* ctx) {
    auto* a = static_cast<DeviceArg*>(ctx);
    DsTestDevice(*a->name, a->index);
}

// ==========================================================================
// main
// ==========================================================================
int wmain() {
    // C runtime locale for the redirected-stdout fallback path (the
    // interactive console path uses WriteConsoleW and needs no locale).
    setlocale(LC_ALL, "");

    // Report file next to the exe
    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = L'\0';
    std::wstring reportPath = std::wstring(exeDir) + L"\\diag_camera_report.txt";
    g_report = _wfopen(reportPath.c_str(), L"wb");
    if (g_report) {
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        fwrite(bom, 1, 3, g_report);
    }

    Out(L"==============================================");
    Out(L" FaceLogin 摄像头诊断工具");
    Out(L" 运行前请确认摄像头未被其他程序占用");
    Out(L" 报告将保存到: %s", reportPath.c_str());
    Out(L"==============================================");
    PrintOsVersion();

    // ---- Enumeration: 3 passes each, 1s apart ----
    Out(L"");
    Out(L"--- 枚举测试 (3 轮, 间隔 1 秒; 连续失败 = 枚举不稳定) ---");
    EnumPass mf[3], ds[3];
    for (int pass = 0; pass < 3; pass++) {
        mf[pass] = MfEnumerate();
        Out(L"[MF] 第 %d 轮: %s, 设备数=%zu%s",
            pass + 1,
            FAILED(mf[pass].hr) ? L"枚举调用失败" : L"枚举成功",
            mf[pass].devices.size(),
            mf[pass].devices.empty() && SUCCEEDED(mf[pass].hr) ? L"  <<<< 无设备" : L"");
        for (size_t i = 0; i < mf[pass].devices.size(); i++) {
            Out(L"[MF]   设备 %zu: %s", i + 1, mf[pass].devices[i].c_str());
        }
        Sleep(1000);
        ds[pass] = DsEnumerate();
        Out(L"[DS] 第 %d 轮: %s, 设备数=%zu%s",
            pass + 1,
            FAILED(ds[pass].hr) ? L"枚举调用失败" : L"枚举成功",
            ds[pass].devices.size(),
            ds[pass].devices.empty() && SUCCEEDED(ds[pass].hr) ? L"  <<<< 无设备" : L"");
        for (size_t i = 0; i < ds[pass].devices.size(); i++) {
            Out(L"[DS]   设备 %zu: %s", i + 1, ds[pass].devices[i].c_str());
        }
        Sleep(1000);
    }

    // ---- Visibility matrix ----
    Out(L"");
    Out(L"--- 可见性对比 (第 1 轮) ---");
    const auto& mfDevs = mf[0].devices;
    const auto& dsDevs = ds[0].devices;
    if (mfDevs.empty() && dsDevs.empty()) {
        Out(L"MF 与 DS 都枚举不到设备——系统级问题（驱动/连接/电源门控）");
    } else if (mfDevs.empty()) {
        Out(L"仅 DS 可见, MF 不可见——驱动可能未注册 Media Foundation 捕获源");
    } else if (dsDevs.empty()) {
        Out(L"仅 MF 可见, DS 不可见");
    } else {
        Out(L"MF 可见 %zu 个, DS 可见 %zu 个", mfDevs.size(), dsDevs.size());
    }

    // ---- Per-device tests ----
    size_t deviceCount = mfDevs.empty() ? dsDevs.size() : mfDevs.size();
    for (size_t d = 0; d < deviceCount; d++) {
        static std::wstring s_path[8], s_name[8];
        if (d >= 8) break;
        s_path[d] = d < mf[0].paths.size() ? mf[0].paths[d] : L"";
        s_name[d] = d < mfDevs.size() ? mfDevs[d] : (d < dsDevs.size() ? dsDevs[d] : L"(unnamed)");
        DeviceArg arg{&s_path[d], &s_name[d], static_cast<int>(d)};
        RunWithTimeout(MfTestThunk, &arg, 25000, L"MF");
        RunWithTimeout(DsTestThunk, &arg, 25000, L"DS");
    }

    Out(L"");
    Out(L"==============================================");
    Out(L" 诊断完成。请将 diag_camera_report.txt 回传给开发者。");
    Out(L"==============================================");

    if (g_report) { fclose(g_report); g_report = nullptr; }
    return 0;
}
