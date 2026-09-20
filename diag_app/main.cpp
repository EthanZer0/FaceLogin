#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <bcrypt.h>
#include <winevt.h>
#include <winsvc.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#include "resource.h"

#ifndef interface
#define interface struct
#endif
#include "../enrollment_app/webview2/include/WebView2.h"

using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kWindowClass[] = L"FaceLoginDiagWindow";
constexpr int kReportId = 1001;
constexpr int kRunId = 1002;
constexpr int kExportId = 1003;
constexpr int kCameraProbeId = 1004;
constexpr UINT_PTR kWebViewTimerId = 1;
constexpr UINT_PTR kCameraProbeTimerId = 2;
constexpr UINT_PTR kAuxProbeTimerId = 3;
constexpr UINT kWebViewTimeoutMs = 15000;
constexpr DWORD kCameraProbeTimeoutMs = 12000;
constexpr DWORD kConsoleProbeTimeoutMs = 25000;
constexpr DWORD kModelProbeTimeoutMs = 90000;

enum class AuxProbeKind { None, ConsoleStartup, ModelLoad };

HWND g_window = nullptr;
HWND g_reportEdit = nullptr;
HWND g_runButton = nullptr;
HWND g_exportButton = nullptr;
HWND g_cameraProbeButton = nullptr;
HWND g_cameraCombo = nullptr;
HWND g_webViewHost = nullptr;
std::wstring g_report;
std::wstring g_installDir;
std::wstring g_modelsDir;
std::wstring g_consoleHtml;
std::vector<std::wstring> g_cameraNames;
HANDLE g_cameraProbeProcess = nullptr;
HANDLE g_cameraProbeJob = nullptr;
HANDLE g_cameraProbeReadPipe = nullptr;
bool g_diagnosticsRunning = false;
bool g_cameraProbeRunning = false;
bool g_cameraProbeTimedOut = false;
bool g_cameraProbeForcedTermination = false;
DWORD g_cameraProbeWaitError = ERROR_SUCCESS;
ULONGLONG g_cameraProbeDeadline = 0;
ULONGLONG g_cameraProbeTerminationTick = 0;
HANDLE g_auxProbeProcess = nullptr;
HANDLE g_auxProbeJob = nullptr;
HANDLE g_auxProbeReadPipe = nullptr;
AuxProbeKind g_auxProbeKind = AuxProbeKind::None;
bool g_auxProbeRunning = false;
bool g_auxProbeTimedOut = false;
bool g_auxProbeForcedTermination = false;
DWORD g_auxProbeWaitError = ERROR_SUCCESS;
ULONGLONG g_auxProbeDeadline = 0;
ULONGLONG g_auxProbeTerminationTick = 0;
bool g_webViewFinished = false;
ULONGLONG g_webViewAttempt = 0;
ComPtr<ICoreWebView2Environment> g_environment;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webView;

void BeginConsoleStartupProbe();
void BeginModelLoadProbe();
void FinishDiagnostics();

std::wstring HexHr(HRESULT hr) {
    wchar_t value[16] = {};
    swprintf_s(value, L"0x%08X", static_cast<unsigned int>(hr));
    return value;
}

std::string HrAscii(HRESULT hr) {
    std::ostringstream value;
    value << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
          << static_cast<uint32_t>(hr);
    return value.str();
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    return dir.empty() || dir.back() == L'\\' ? dir + name : dir + L"\\" + name;
}

std::wstring ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return L".";
    path.resize(length);
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    path.resize(slash);
    return path;
}

bool ReadRegistryString(HKEY root, const wchar_t* keyPath, const wchar_t* valueName,
                        std::wstring& value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, keyPath, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0, bytes = 0;
    LONG rc = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return false;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    rc = RegQueryValueExW(key, valueName, nullptr, &type,
                          reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return false;
    value.assign(buffer.data());
    if (type == REG_EXPAND_SZ) {
        DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (required) {
            std::wstring expanded(required, L'\0');
            DWORD written = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required);
            if (written && written <= required) {
                expanded.resize(written - 1);
                value.swap(expanded);
            }
        }
    }
    return !value.empty();
}

void RefreshReport() {
    if (!g_reportEdit) return;
    SetWindowTextW(g_reportEdit, g_report.c_str());
    SendMessageW(g_reportEdit, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(g_reportEdit, EM_SCROLLCARET, 0, 0);
}

void Line(const std::wstring& text) {
    g_report += text + L"\r\n";
    RefreshReport();
}

void Result(const wchar_t* status, const std::wstring& name, const std::wstring& detail) {
    Line(L"[" + std::wstring(status) + L"] " + name + L" — " + detail);
}

std::wstring FileVersion(const std::wstring& path) {
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return L"版本资源缺失";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return L"无法读取版本资源";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) ||
        !info || length < sizeof(VS_FIXEDFILEINFO)) return L"版本资源无效";
    std::wostringstream out;
    out << HIWORD(info->dwFileVersionMS) << L'.' << LOWORD(info->dwFileVersionMS) << L'.'
        << HIWORD(info->dwFileVersionLS) << L'.' << LOWORD(info->dwFileVersionLS);
    return out.str();
}

bool Sha256(const std::wstring& path, std::wstring& hex) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD objectSize = 0, hashSize = 0, copied = 0;
    std::vector<BYTE> object, digest;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) goto done;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &copied, 0) < 0) goto done;
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &copied, 0) < 0) goto done;
    object.resize(objectSize);
    digest.resize(hashSize);
    if (BCryptCreateHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) goto done;
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) goto done;
    {
        std::array<BYTE, 64 * 1024> buffer{};
        DWORD count = 0;
        for (;;) {
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) goto done;
            if (!count) break;
            if (BCryptHashData(hash, buffer.data(), count, 0) < 0) goto done;
        }
    }
    if (BCryptFinishHash(hash, digest.data(), hashSize, 0) < 0) goto done;
    {
        static constexpr wchar_t digits[] = L"0123456789abcdef";
        hex.clear();
        for (BYTE byte : digest) {
            hex.push_back(digits[byte >> 4]);
            hex.push_back(digits[byte & 15]);
        }
    }
    ok = true;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::wstring OsDescription() {
    std::wstring product, build, display;
    ReadRegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName", product);
    ReadRegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber", build);
    ReadRegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion", display);
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    const wchar_t* arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? L"x64" :
        si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? L"ARM64" : L"other";
    if (product.empty()) product = L"Windows";
    wchar_t* parsedEnd = nullptr;
    const long buildNumber = wcstol(build.c_str(), &parsedEnd, 10);
    if (parsedEnd != build.c_str() && buildNumber >= 22000 && product.rfind(L"Windows 10", 0) == 0) {
        product.replace(0, 10, L"Windows 11");
    }
    return product + L" " + display + L" (build " + build + L", " + arch + L")";
}

void CheckRuntimeFiles() {
    static constexpr const wchar_t* files[] = {
        L"onnxruntime.dll", L"onnxruntime_providers_shared.dll", L"abseil_dll.dll",
        L"libgcc_s_seh-1.dll", L"libgfortran-5.dll", L"liblapack.dll",
        L"libprotobuf-lite.dll", L"libprotobuf.dll", L"libquadmath-0.dll",
        L"libwinpthread-1.dll", L"openblas.dll", L"re2.dll"
    };
    for (const wchar_t* name : files) {
        const std::wstring path = JoinPath(g_installDir, name);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            Result(L"WARN", name, L"运行库文件缺失；是否影响模型加载由隔离加载探测确认");
            continue;
        }
        ULARGE_INTEGER size{};
        size.HighPart = data.nFileSizeHigh;
        size.LowPart = data.nFileSizeLow;
        Result(L"PASS", name, L"文件存在，" + std::to_wstring(size.QuadPart) + L" 字节");
    }
}

std::wstring CheckFile(const wchar_t* fileName, bool required) {
    const std::wstring path = JoinPath(g_installDir, fileName);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        Result(required ? L"FAIL" : L"WARN", fileName, L"文件不存在");
        return L"";
    }
    ULARGE_INTEGER size{};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    std::wstring hash;
    const std::wstring version = FileVersion(path);
    std::wstring detail = L"版本 " + version + L"，大小 " +
        std::to_wstring(size.QuadPart) + L" 字节";
    if (Sha256(path, hash)) detail += L"，SHA-256 " + hash;
    else detail += L"，SHA-256 读取失败";
    Result(L"PASS", fileName, detail);
    return version;
}

void CheckService() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        Result(L"WARN", L"FaceLoginService", L"无法打开服务管理器，错误码 " + std::to_wstring(GetLastError()));
        return;
    }
    SC_HANDLE service = OpenServiceW(manager, L"FaceLoginService", SERVICE_QUERY_STATUS);
    if (!service) {
        const DWORD error = GetLastError();
        CloseServiceHandle(manager);
        Result(error == ERROR_SERVICE_DOES_NOT_EXIST ? L"WARN" : L"FAIL", L"FaceLoginService",
            error == ERROR_SERVICE_DOES_NOT_EXIST ? L"未安装" : L"查询失败，错误码 " + std::to_wstring(error));
        return;
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    const BOOL ok = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!ok) {
        Result(L"FAIL", L"FaceLoginService", L"状态查询失败，错误码 " + std::to_wstring(error));
        return;
    }
    const wchar_t* state = L"未知";
    switch (status.dwCurrentState) {
        case SERVICE_RUNNING: state = L"运行中"; break;
        case SERVICE_STOPPED: state = L"已停止"; break;
        case SERVICE_START_PENDING: state = L"正在启动"; break;
        case SERVICE_STOP_PENDING: state = L"正在停止"; break;
        case SERVICE_PAUSED: state = L"已暂停"; break;
    }
    Result(status.dwCurrentState == SERVICE_RUNNING ? L"PASS" : L"WARN", L"FaceLoginService", state);
}

void CheckModels() {
    const wchar_t* names[] = { L"2d106det.onnx", L"det_500m.onnx", L"w600k_mbf.onnx",
                               L"head_pose_mobilenetv2.onnx", L"minifas_quantized.onnx" };
    for (size_t i = 0; i < std::size(names); ++i) {
        const std::wstring path = JoinPath(g_modelsDir, names[i]);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            Result(i < 3 ? L"FAIL" : L"WARN", names[i], L"模型文件不存在");
        } else {
            ULARGE_INTEGER size{};
            size.HighPart = data.nFileSizeHigh;
            size.LowPart = data.nFileSizeLow;
            Result(L"PASS", names[i], std::to_wstring(size.QuadPart) + L" 字节");
        }
    }
}

bool LoadConsoleHtml(std::wstring& html, std::wstring& detail) {
    const std::wstring path = JoinPath(g_installDir, L"FaceLoginConsole.exe");
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) {
        detail = L"无法读取 Console 资源，错误码 " + std::to_wstring(GetLastError());
        return false;
    }
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_CONSOLE_HTML), RT_RCDATA);
    if (!resource) {
        FreeLibrary(module);
        detail = L"未找到内嵌 HTML 资源 ID 1009";
        return false;
    }
    HGLOBAL loaded = LoadResource(module, resource);
    const DWORD bytes = SizeofResource(module, resource);
    const char* data = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
    if (!data || !bytes) {
        FreeLibrary(module);
        detail = L"HTML 资源为空";
        return false;
    }
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data,
        static_cast<int>(bytes), nullptr, 0);
    if (!chars) {
        FreeLibrary(module);
        detail = L"内嵌 HTML 不是有效 UTF-8";
        return false;
    }
    html.resize(chars);
    const bool decoded = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data,
        static_cast<int>(bytes), html.data(), chars) != 0;
    FreeLibrary(module);
    if (!decoded) {
        detail = L"内嵌 HTML 解码失败";
        return false;
    }
    detail = std::to_wstring(bytes) + L" 字节，UTF-8 有效";
    return true;
}

void CheckCameraEnumeration() {
    g_cameraNames.clear();
    if (g_cameraCombo) SendMessageW(g_cameraCombo, CB_RESETCONTENT, 0, 0);
    if (g_cameraProbeButton) EnableWindow(g_cameraProbeButton, FALSE);
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        Result(L"FAIL", L"Media Foundation 摄像头枚举", L"MFStartup 失败，HRESULT " + HexHr(hr));
        return;
    }
    IMFAttributes* attributes = nullptr;
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr)) hr = MFEnumDeviceSources(attributes, &devices, &count);
    if (FAILED(hr)) {
        Result(L"FAIL", L"Media Foundation 摄像头枚举", L"设备枚举失败，HRESULT " + HexHr(hr));
    } else if (!count) {
        Result(L"WARN", L"Media Foundation 摄像头枚举", L"系统未枚举到摄像头；本项不验证取流");
    } else {
        Result(L"PASS", L"Media Foundation 摄像头枚举",
            L"发现 " + std::to_wstring(count) + L" 台设备（仅枚举，未打开摄像头或读取画面）");
        for (UINT32 i = 0; i < count && i < 8; ++i) {
            WCHAR* name = nullptr;
            UINT32 chars = 0;
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &chars)) && name) {
                Line(L"    摄像头 " + std::to_wstring(i + 1) + L"：" + name);
                g_cameraNames.emplace_back(name);
                CoTaskMemFree(name);
            } else {
                g_cameraNames.emplace_back(L"摄像头 " + std::to_wstring(i + 1));
            }
        }
    }
    if (devices) {
        for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
    }
    if (attributes) attributes->Release();
    MFShutdown();
    if (g_cameraCombo) {
        SendMessageW(g_cameraCombo, CB_RESETCONTENT, 0, 0);
        for (const auto& name : g_cameraNames) SendMessageW(g_cameraCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(name.c_str()));
        if (!g_cameraNames.empty()) {
            SendMessageW(g_cameraCombo, CB_SETCURSEL, 0, 0);
            EnableWindow(g_cameraProbeButton, !g_cameraProbeRunning);
        } else {
            EnableWindow(g_cameraProbeButton, FALSE);
        }
    }
}

int CameraProbeChild(UINT32 deviceIndex) {
    std::ostringstream output;
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    output << "MFStartup=" << HrAscii(hr) << "\n";
    const bool mfStarted = SUCCEEDED(hr);

    IMFAttributes* attributes = nullptr;
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (mfStarted) {
        hr = MFCreateAttributes(&attributes, 1);
        if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (SUCCEEDED(hr)) hr = MFEnumDeviceSources(attributes, &devices, &count);
    }
    output << "MFEnumDeviceSources=" << HrAscii(hr)
           << "\nDeviceCount=" << count << "\nSelectedDevice=" << deviceIndex << "\n";

    IMFMediaSource* source = nullptr;
    IMFSourceReader* reader = nullptr;
    IMFSample* sample = nullptr;
    if (SUCCEEDED(hr) && deviceIndex < count) {
        hr = devices[deviceIndex]->ActivateObject(IID_PPV_ARGS(&source));
        output << "ActivateObject=" << HrAscii(hr) << "\n";
    } else if (SUCCEEDED(hr)) {
        hr = MF_E_NOT_FOUND;
    }
    if (SUCCEEDED(hr)) {
        HRESULT readerHr = MFCreateSourceReaderFromMediaSource(source, nullptr, &reader);
        output << "CreateSourceReader=" << HrAscii(readerHr) << "\n";
        hr = readerHr;
    }
    bool gotFrame = false;
    if (SUCCEEDED(hr)) {
        for (unsigned int attempt = 1; attempt <= 5 && !gotFrame; ++attempt) {
            DWORD streamIndex = 0, flags = 0;
            LONGLONG timestamp = 0;
            hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                    &streamIndex, &flags, &timestamp, &sample);
            output << "ReadSample[" << attempt << "]=" << HrAscii(hr)
                   << ", stream=" << streamIndex << ", flags=" << flags << "\n";
            gotFrame = SUCCEEDED(hr) && sample != nullptr;
            if (sample) {
                sample->Release();
                sample = nullptr;
            }
            if (FAILED(hr)) break;
        }
        output << "FirstFrame=" << (gotFrame ? 1 : 0) << "\n";
    }
    if (reader) reader->Release();
    if (source) {
        source->Shutdown();
        source->Release();
    }
    if (devices) {
        for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
    }
    if (attributes) attributes->Release();
    if (mfStarted) MFShutdown();

    const std::string text = output.str();
    HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    if (stdoutHandle && stdoutHandle != INVALID_HANDLE_VALUE) {
        WriteFile(stdoutHandle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
    return gotFrame ? 0 : FAILED(hr) ? 1 : 3;
}

void FinishCameraProbe() {
    if (!g_cameraProbeRunning) return;
    KillTimer(g_window, kCameraProbeTimerId);

    DWORD exitCode = 0;
    const bool exitCodeKnown = GetExitCodeProcess(g_cameraProbeProcess, &exitCode) != FALSE;
    std::string bytes;
    std::array<char, 2048> buffer{};
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(g_cameraProbeReadPipe, nullptr, 0, nullptr, &available, nullptr) || !available) break;
        DWORD count = 0;
        const DWORD request = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        if (!ReadFile(g_cameraProbeReadPipe, buffer.data(), request, &count, nullptr) || !count) break;
        bytes.append(buffer.data(), count);
    }
    const std::wstring detail(bytes.begin(), bytes.end());
    const bool passed = !g_cameraProbeTimedOut && g_cameraProbeWaitError == ERROR_SUCCESS &&
                        exitCodeKnown && exitCode == 0;
    const wchar_t* status = passed ? L"PASS" : g_cameraProbeTimedOut ? L"WARN" : L"FAIL";
    std::wstring summary;
    if (g_cameraProbeTimedOut) {
        summary = L"12 秒内未能完成摄像头打开并读取首帧，隔离进程已终止；\r\n";
    } else if (g_cameraProbeWaitError != ERROR_SUCCESS) {
        summary = L"等待摄像头隔离进程失败，错误码 " + std::to_wstring(g_cameraProbeWaitError) + L"；\r\n";
    } else if (!exitCodeKnown) {
        summary = L"无法读取摄像头隔离进程的退出状态；\r\n";
    } else if (passed) {
        summary = L"成功打开所选摄像头并读取到一帧（图像未保存）；\r\n";
    } else {
        summary = L"摄像头打开/首帧读取失败，子进程退出码 " + std::to_wstring(exitCode) + L"；\r\n";
    }

    CloseHandle(g_cameraProbeReadPipe);
    CloseHandle(g_cameraProbeProcess);
    CloseHandle(g_cameraProbeJob);
    g_cameraProbeReadPipe = nullptr;
    g_cameraProbeProcess = nullptr;
    g_cameraProbeJob = nullptr;
    g_cameraProbeRunning = false;
    g_cameraProbeTimedOut = false;
    g_cameraProbeForcedTermination = false;
    g_cameraProbeWaitError = ERROR_SUCCESS;
    EnableWindow(g_cameraProbeButton, !g_cameraNames.empty());
    Line(L"[" + std::wstring(status) + L"] 摄像头取流测试：\r\n" + summary + detail);
}

void BeginCameraProbe() {
    if (g_cameraProbeRunning) return;
    const LRESULT selected = SendMessageW(g_cameraCombo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) {
        Line(L"[WARN] 请先选择要测试的摄像头。");
        return;
    }

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        Line(L"[FAIL] 摄像头取流测试无法创建隔离管道，错误码 " + std::to_wstring(GetLastError()));
        return;
    }
    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        const DWORD error = GetLastError();
        CloseHandle(readPipe); CloseHandle(writePipe);
        Line(L"[FAIL] 摄像头取流测试无法保护管道句柄，错误码 " + std::to_wstring(error));
        return;
    }
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE || !job) {
        const DWORD error = GetLastError();
        if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
        if (job) CloseHandle(job);
        CloseHandle(readPipe); CloseHandle(writePipe);
        Line(L"[FAIL] 摄像头取流测试无法创建隔离资源，错误码 " + std::to_wstring(error));
        return;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        const DWORD error = GetLastError();
        CloseHandle(job); CloseHandle(nullInput); CloseHandle(readPipe); CloseHandle(writePipe);
        Line(L"[FAIL] 摄像头取流测试无法配置进程隔离，错误码 " + std::to_wstring(error));
        return;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    std::wstring executable(32768, L'\0');
    DWORD pathLength = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!pathLength || pathLength >= executable.size()) {
        CloseHandle(job); CloseHandle(nullInput); CloseHandle(readPipe); CloseHandle(writePipe);
        Line(L"[FAIL] 无法定位诊断程序自身路径。");
        return;
    }
    executable.resize(pathLength);
    std::wstring command = L"\"" + executable + L"\" --camera-probe " + std::to_wstring(selected);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, g_installDir.c_str(), &startup, &process)) {
        const DWORD error = GetLastError();
        CloseHandle(job); CloseHandle(nullInput); CloseHandle(readPipe); CloseHandle(writePipe);
        Line(L"[FAIL] 无法启动摄像头隔离进程，错误码 " + std::to_wstring(error));
        return;
    }
    CloseHandle(writePipe);
    CloseHandle(nullInput);
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, error);
        WaitForSingleObject(process.hProcess, 3000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        CloseHandle(job); CloseHandle(readPipe);
        Line(L"[FAIL] 无法将摄像头检查置于可终止隔离进程，错误码 " + std::to_wstring(error));
        return;
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        TerminateJobObject(job, error);
        WaitForSingleObject(process.hProcess, 3000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        CloseHandle(job); CloseHandle(readPipe);
        Line(L"[FAIL] 无法启动摄像头隔离进程，错误码 " + std::to_wstring(error));
        return;
    }
    CloseHandle(process.hThread);
    g_cameraProbeProcess = process.hProcess;
    g_cameraProbeJob = job;
    g_cameraProbeReadPipe = readPipe;
    g_cameraProbeRunning = true;
    g_cameraProbeTimedOut = false;
    g_cameraProbeForcedTermination = false;
    g_cameraProbeWaitError = ERROR_SUCCESS;
    g_cameraProbeDeadline = GetTickCount64() + kCameraProbeTimeoutMs;
    g_cameraProbeTerminationTick = 0;
    if (!SetTimer(g_window, kCameraProbeTimerId, 100, nullptr)) {
        TerminateJobObject(g_cameraProbeJob, GetLastError());
        CloseHandle(g_cameraProbeJob); CloseHandle(g_cameraProbeProcess); CloseHandle(g_cameraProbeReadPipe);
        g_cameraProbeJob = nullptr; g_cameraProbeProcess = nullptr; g_cameraProbeReadPipe = nullptr;
        g_cameraProbeRunning = false;
        Line(L"[FAIL] 无法启动摄像头取流检查计时器。");
        return;
    }
    EnableWindow(g_cameraProbeButton, FALSE);
    const std::wstring cameraName = static_cast<size_t>(selected) < g_cameraNames.size()
        ? g_cameraNames[static_cast<size_t>(selected)] : L"所选摄像头";
    Line(L"[INFO] 摄像头取流检查已启动：" + cameraName + L"；摄像头可能短暂亮灯，本次不会保存图像。");
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

std::wstring XmlDataValue(const std::wstring& xml, const std::wstring& requestedName) {
    size_t position = 0;
    while ((position = xml.find(L"<Data", position)) != std::wstring::npos) {
        const size_t tagEnd = xml.find(L'>', position);
        if (tagEnd == std::wstring::npos) return L"";
        const std::wstring tag = xml.substr(position, tagEnd - position);
        const size_t namePosition = tag.find(L"Name=");
        if (namePosition != std::wstring::npos && namePosition + 5 < tag.size()) {
            const wchar_t quote = tag[namePosition + 5];
            if (quote == L'\'' || quote == L'"') {
                const size_t nameEnd = tag.find(quote, namePosition + 6);
                if (nameEnd != std::wstring::npos &&
                    tag.substr(namePosition + 6, nameEnd - namePosition - 6) == requestedName) {
                    const size_t valueEnd = xml.find(L"</Data>", tagEnd + 1);
                    if (valueEnd == std::wstring::npos) return L"";
                    return xml.substr(tagEnd + 1, valueEnd - tagEnd - 1);
                }
            }
        }
        position = tagEnd + 1;
    }
    return L"";
}

std::wstring ModuleFileName(std::wstring value) {
    const size_t slash = value.find_last_of(L"\\/");
    if (slash != std::wstring::npos) value.erase(0, slash + 1);
    if (value.size() > 128) value.resize(128);
    return value;
}

std::wstring ExceptionCode(std::wstring value) {
    if (value.empty() || value.size() > 32) return L"";
    const bool hexadecimal = std::all_of(value.begin(), value.end(), [](wchar_t c) {
        const wchar_t lower = static_cast<wchar_t>(towlower(c));
        return (lower >= L'0' && lower <= L'9') || (lower >= L'a' && lower <= L'f') || lower == L'x';
    });
    if (!hexadecimal) return L"";
    if (value.find(L'x') == std::wstring::npos && value.find(L'X') == std::wstring::npos)
        value = L"0x" + value;
    return value;
}

void CheckCrashEvents() {
    constexpr wchar_t query[] =
        L"*[System[TimeCreated[timediff(@SystemTime) <= 604800000] and "
        L"(Provider[@Name='Application Error'] or Provider[@Name='Windows Error Reporting']) and "
        L"(EventID=1000 or EventID=1001)]]";
    EVT_HANDLE queryHandle = EvtQuery(nullptr, L"Application", query,
        EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!queryHandle) {
        Result(L"WARN", L"Windows 崩溃事件", L"无法读取最近 7 天的 Application 日志，错误码 " + std::to_wstring(GetLastError()));
        return;
    }
    int matches = 0;
    std::vector<std::wstring> samples;
    for (int i = 0; i < 1000; ++i) {
        EVT_HANDLE eventHandle = nullptr;
        DWORD returned = 0;
        if (!EvtNext(queryHandle, 1, &eventHandle, 0, 0, &returned) || returned != 1) break;
        DWORD bytes = 0, properties = 0;
        EvtRender(nullptr, eventHandle, EvtRenderEventXml, 0, nullptr, &bytes, &properties);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes) {
            std::vector<wchar_t> xml(bytes / sizeof(wchar_t) + 1, L'\0');
            if (EvtRender(nullptr, eventHandle, EvtRenderEventXml, bytes, xml.data(), &bytes, &properties)) {
                std::wstring rendered(xml.data());
                std::wstring lower = Lower(rendered);
                std::wstring appName = XmlDataValue(rendered, L"AppName");
                if (appName.empty()) appName = XmlDataValue(rendered, L"P1");
                const std::wstring lowerAppName = Lower(appName);
                if (lowerAppName.find(L"faceloginconsole.exe") != std::wstring::npos ||
                    lowerAppName.find(L"facelogindiag.exe") != std::wstring::npos ||
                    lowerAppName.find(L"faceloginservice.exe") != std::wstring::npos ||
                    (appName.empty() && (lower.find(L"faceloginconsole.exe") != std::wstring::npos ||
                                         lower.find(L"facelogindiag.exe") != std::wstring::npos ||
                                         lower.find(L"faceloginservice.exe") != std::wstring::npos))) {
                    ++matches;
                    if (samples.size() < 8) {
                        std::wstring summary = appName.empty() ? L"FaceLogin 应用" : ModuleFileName(appName);
                        size_t marker = rendered.find(L"<EventID");
                        if (marker != std::wstring::npos) {
                            size_t valueStart = rendered.find(L">", marker);
                            size_t valueEnd = valueStart == std::wstring::npos ? valueStart : rendered.find(L"<", valueStart + 1);
                            if (valueStart != std::wstring::npos && valueEnd != std::wstring::npos)
                                summary += L"，事件 ID " + rendered.substr(valueStart + 1, valueEnd - valueStart - 1);
                        }
                        const std::wstring module = ModuleFileName(XmlDataValue(rendered, L"ModuleName"));
                        if (!module.empty()) summary += L"，故障模块 " + module;
                        const std::wstring exception = ExceptionCode(XmlDataValue(rendered, L"ExceptionCode"));
                        if (!exception.empty())
                            summary += L"，异常码 " + exception;
                        samples.push_back(std::move(summary));
                    }
                }
            }
        }
        EvtClose(eventHandle);
    }
    EvtClose(queryHandle);
    if (!matches) Result(L"PASS", L"Windows 崩溃事件", L"最近 7 天未检索到 FaceLogin 相关崩溃事件");
    else {
        Result(L"WARN", L"Windows 崩溃事件", L"最近 7 天检索到 " + std::to_wstring(matches) + L" 条相关事件");
        for (const auto& sample : samples) Line(L"    " + sample);
    }
}

bool StartAuxProbe(AuxProbeKind kind, const wchar_t* executableName,
                   const std::wstring& arguments, DWORD timeoutMs) {
    const std::wstring executable = JoinPath(g_installDir, executableName);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) return false;
    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nullError = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE || nullError == INVALID_HANDLE_VALUE || !job) {
        if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
        if (nullError != INVALID_HANDLE_VALUE) CloseHandle(nullError);
        if (job) CloseHandle(job);
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        CloseHandle(nullInput);
        CloseHandle(nullError);
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput;
    startup.hStdOutput = writePipe;
    startup.hStdError = nullError;
    std::wstring command = L"\"" + executable + L"\"";
    if (!arguments.empty()) command += L" " + arguments;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, g_installDir.c_str(),
                        &startup, &process)) {
        CloseHandle(job);
        CloseHandle(nullInput);
        CloseHandle(nullError);
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }
    CloseHandle(writePipe);
    CloseHandle(nullInput);
    CloseHandle(nullError);
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, error);
        WaitForSingleObject(process.hProcess, 3000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        CloseHandle(readPipe);
        return false;
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        TerminateJobObject(job, error);
        TerminateProcess(process.hProcess, error);
        WaitForSingleObject(process.hProcess, 3000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        CloseHandle(readPipe);
        return false;
    }
    CloseHandle(process.hThread);

    if (!SetTimer(g_window, kAuxProbeTimerId, 100, nullptr)) {
        TerminateJobObject(job, GetLastError());
        WaitForSingleObject(process.hProcess, 3000);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        CloseHandle(readPipe);
        return false;
    }

    g_auxProbeProcess = process.hProcess;
    g_auxProbeJob = job;
    g_auxProbeReadPipe = readPipe;
    g_auxProbeKind = kind;
    g_auxProbeRunning = true;
    g_auxProbeTimedOut = false;
    g_auxProbeForcedTermination = false;
    g_auxProbeWaitError = ERROR_SUCCESS;
    g_auxProbeDeadline = GetTickCount64() + timeoutMs;
    g_auxProbeTerminationTick = 0;
    return true;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return L"";
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (!chars) return L"模型探测输出无法解码";
    std::wstring result(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), chars);
    return result;
}

void FinishAuxProbe() {
    if (!g_auxProbeRunning) return;
    KillTimer(g_window, kAuxProbeTimerId);
    DWORD exitCode = 0;
    const bool exitCodeKnown = GetExitCodeProcess(g_auxProbeProcess, &exitCode) != FALSE;
    std::string output;
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(g_auxProbeReadPipe, nullptr, 0, nullptr, &available, nullptr) || !available) break;
        DWORD count = 0;
        const DWORD request = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        if (!ReadFile(g_auxProbeReadPipe, buffer.data(), request, &count, nullptr) || !count) break;
        output.append(buffer.data(), count);
    }

    const AuxProbeKind kind = g_auxProbeKind;
    const bool passed = !g_auxProbeTimedOut && g_auxProbeWaitError == ERROR_SUCCESS &&
                        exitCodeKnown && exitCode == 0;
    if (kind == AuxProbeKind::ConsoleStartup) {
        Result(passed ? L"PASS" : L"FAIL", L"Console 无摄像头启动探测",
            g_auxProbeTimedOut ? L"25 秒内未完成，探测进程已终止" :
            passed ? L"实际启动 FaceLoginConsole.exe，完成 WebView2 页面、四个导航页签和安全桥接检查；未启动采集、未点亮摄像头" :
            L"Console 探测进程未能完成启动，退出码 " + std::to_wstring(exitCode));
    } else if (kind == AuxProbeKind::ModelLoad) {
        const std::wstring decoded = Utf8ToWide(output);
        bool runtimeReported = false;
        size_t position = 0;
        while (position < decoded.size()) {
            const size_t end = decoded.find(L'\n', position);
            std::wstring line = decoded.substr(position, end == std::wstring::npos ? end : end - position);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            const size_t first = line.find(L'|');
            const size_t second = first == std::wstring::npos ? first : line.find(L'|', first + 1);
            if (first != std::wstring::npos && second != std::wstring::npos) {
                const std::wstring status = line.substr(0, first);
                const std::wstring name = line.substr(first + 1, second - first - 1);
                const std::wstring detail = line.substr(second + 1);
                if (name == L"onnxruntime" && status == L"PASS") runtimeReported = true;
                Result(status.c_str(), name, detail);
            }
            if (end == std::wstring::npos) break;
            position = end + 1;
        }
        Result(passed && runtimeReported ? L"PASS" : L"FAIL", L"ONNX Runtime/模型加载",
            g_auxProbeTimedOut ? L"90 秒内未完成，隔离进程已终止" :
            passed && runtimeReported ? L"隔离进程成功加载运行库并检查所有已配置模型会话" :
            L"隔离探测失败，进程退出码 " + std::to_wstring(exitCode));
    }

    CloseHandle(g_auxProbeReadPipe);
    CloseHandle(g_auxProbeProcess);
    CloseHandle(g_auxProbeJob);
    g_auxProbeReadPipe = nullptr;
    g_auxProbeProcess = nullptr;
    g_auxProbeJob = nullptr;
    g_auxProbeRunning = false;
    g_auxProbeKind = AuxProbeKind::None;

    if (kind == AuxProbeKind::ConsoleStartup) BeginModelLoadProbe();
    else FinishDiagnostics();
}

void BeginConsoleStartupProbe() {
    if (!StartAuxProbe(AuxProbeKind::ConsoleStartup, L"FaceLoginConsole.exe",
                       L"--diagnostic-startup", kConsoleProbeTimeoutMs)) {
        Result(L"FAIL", L"Console 无摄像头启动探测", L"无法启动 Console 隔离探测进程；目标文件缺失或系统阻止了启动");
        BeginModelLoadProbe();
    } else {
        Result(L"INFO", L"Console 无摄像头启动探测", L"已启动隐藏隔离进程；页面完成后自动退出，不会打开摄像头");
    }
}

void BeginModelLoadProbe() {
#ifndef FACELOGIN_HAS_MODEL_PROBE
    Result(L"WARN", L"ONNX Runtime/模型加载", L"当前诊断工具构建未包含模型加载探测器");
    FinishDiagnostics();
#else
    if (!StartAuxProbe(AuxProbeKind::ModelLoad, L"FaceLoginModelProbe.exe",
                       L"\"" + g_modelsDir + L"\"", kModelProbeTimeoutMs)) {
        Result(L"FAIL", L"ONNX Runtime/模型加载", L"无法启动独立模型探测程序；请确认 FaceLoginModelProbe.exe 已安装");
        FinishDiagnostics();
    } else {
        Result(L"INFO", L"ONNX Runtime/模型加载", L"开始在隔离进程中创建 ONNX 会话；不访问摄像头");
    }
#endif
}

void FinishDiagnostics() {
    g_diagnosticsRunning = false;
    EnableWindow(g_runButton, TRUE);
}

void PollAuxProbe() {
    if (!g_auxProbeRunning) return;
    const ULONGLONG now = GetTickCount64();
    if (!g_auxProbeTimedOut && now >= g_auxProbeDeadline) {
        g_auxProbeTimedOut = true;
        g_auxProbeTerminationTick = now;
        TerminateJobObject(g_auxProbeJob, ERROR_TIMEOUT);
    }
    const DWORD wait = WaitForSingleObject(g_auxProbeProcess, 0);
    if (wait == WAIT_OBJECT_0) {
        FinishAuxProbe();
    } else if (wait == WAIT_FAILED) {
        g_auxProbeWaitError = GetLastError();
        TerminateJobObject(g_auxProbeJob, g_auxProbeWaitError);
        FinishAuxProbe();
    } else if (g_auxProbeTimedOut && !g_auxProbeForcedTermination &&
               now - g_auxProbeTerminationTick >= 3000) {
        g_auxProbeForcedTermination = true;
        g_auxProbeTerminationTick = now;
        TerminateProcess(g_auxProbeProcess, ERROR_TIMEOUT);
    } else if (g_auxProbeTimedOut && g_auxProbeForcedTermination &&
               now - g_auxProbeTerminationTick >= 3000) {
        FinishAuxProbe();
    }
}

class EnvironmentHandler final : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    EnvironmentHandler(HWND window, ULONGLONG attempt) : m_window(window), m_attempt(attempt) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        if (iid == IID_IUnknown || iid == __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
            *object = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
            AddRef(); return S_OK;
        }
        *object = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&m_refs);
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment* environment) override;
private:
    volatile LONG m_refs = 1;
    HWND m_window;
    ULONGLONG m_attempt;
};

class ScriptCompletedHandler final : public ICoreWebView2ExecuteScriptCompletedHandler {
public:
    ScriptCompletedHandler(HWND window, ULONGLONG attempt) : m_window(window), m_attempt(attempt) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        if (iid == IID_IUnknown || iid == __uuidof(ICoreWebView2ExecuteScriptCompletedHandler)) {
            *object = static_cast<ICoreWebView2ExecuteScriptCompletedHandler*>(this);
            AddRef(); return S_OK;
        }
        *object = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&m_refs);
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT error, LPCWSTR result) override {
        if (!IsWindow(m_window) || g_webViewFinished || m_attempt != g_webViewAttempt) return S_OK;
        g_webViewFinished = true;
        KillTimer(m_window, kWebViewTimerId);
        if (SUCCEEDED(error) && result && wcscmp(result, L"\"complete\"") == 0) {
            Result(L"PASS", L"WebView2 JavaScript", L"页面脚本引擎可执行，document.readyState=complete");
        } else {
            Result(L"FAIL", L"WebView2 JavaScript", L"脚本执行失败，HRESULT " + HexHr(error));
        }
        BeginConsoleStartupProbe();
        return S_OK;
    }
private:
    volatile LONG m_refs = 1;
    HWND m_window;
    ULONGLONG m_attempt;
};

class NavigationHandler final : public ICoreWebView2NavigationCompletedEventHandler {
public:
    NavigationHandler(HWND window, ULONGLONG attempt) : m_window(window), m_attempt(attempt) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        if (iid == IID_IUnknown || iid == __uuidof(ICoreWebView2NavigationCompletedEventHandler)) {
            *object = static_cast<ICoreWebView2NavigationCompletedEventHandler*>(this);
            AddRef(); return S_OK;
        }
        *object = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&m_refs);
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) override {
        if (!IsWindow(m_window) || g_webViewFinished || m_attempt != g_webViewAttempt) return S_OK;
        BOOL success = FALSE;
        COREWEBVIEW2_WEB_ERROR_STATUS error = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
        if (args) {
            args->get_IsSuccess(&success);
            args->get_WebErrorStatus(&error);
        }
        if (!success) {
            g_webViewFinished = true;
            KillTimer(m_window, kWebViewTimerId);
            Result(L"FAIL", L"WebView2 实际 HTML 导航", L"导航失败，WebErrorStatus=" + std::to_wstring(static_cast<int>(error)));
            BeginConsoleStartupProbe();
            return S_OK;
        }
        Result(L"PASS", L"WebView2 实际 HTML 导航", L"Console 原始内嵌 HTML 导航完成（未注入 HostObject/语言包）");
        auto* script = new ScriptCompletedHandler(m_window, m_attempt);
        HRESULT hr = g_webView->ExecuteScript(L"document.readyState", script);
        script->Release();
        if (FAILED(hr)) {
            g_webViewFinished = true;
            KillTimer(m_window, kWebViewTimerId);
            Result(L"FAIL", L"WebView2 JavaScript", L"ExecuteScript 调用失败，HRESULT " + HexHr(hr));
            BeginConsoleStartupProbe();
        }
        return S_OK;
    }
private:
    volatile LONG m_refs = 1;
    HWND m_window;
    ULONGLONG m_attempt;
};

class ProcessFailedHandler final : public ICoreWebView2ProcessFailedEventHandler {
public:
    ProcessFailedHandler(HWND window, ULONGLONG attempt) : m_window(window), m_attempt(attempt) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        if (iid == IID_IUnknown || iid == __uuidof(ICoreWebView2ProcessFailedEventHandler)) {
            *object = static_cast<ICoreWebView2ProcessFailedEventHandler*>(this);
            AddRef(); return S_OK;
        }
        *object = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&m_refs);
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) override {
        if (!IsWindow(m_window) || g_webViewFinished || m_attempt != g_webViewAttempt) return S_OK;
        COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
        if (args) args->get_ProcessFailedKind(&kind);
        g_webViewFinished = true;
        KillTimer(m_window, kWebViewTimerId);
        Result(L"FAIL", L"WebView2 子进程", L"ProcessFailed，kind=" + std::to_wstring(static_cast<int>(kind)));
        BeginConsoleStartupProbe();
        return S_OK;
    }
private:
    volatile LONG m_refs = 1;
    HWND m_window;
    ULONGLONG m_attempt;
};

class ControllerHandler final : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    ControllerHandler(HWND window, ULONGLONG attempt) : m_window(window), m_attempt(attempt) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        if (iid == IID_IUnknown || iid == __uuidof(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
            *object = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
            AddRef(); return S_OK;
        }
        *object = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&m_refs);
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* controller) override {
        if (!IsWindow(m_window) || g_webViewFinished || m_attempt != g_webViewAttempt) return S_OK;
        if (FAILED(hr) || !controller) {
            g_webViewFinished = true;
            KillTimer(m_window, kWebViewTimerId);
            Result(L"FAIL", L"WebView2 Controller", L"创建失败，HRESULT " + HexHr(hr));
            BeginConsoleStartupProbe();
            return S_OK;
        }
        g_controller = controller;
        g_controller->put_IsVisible(FALSE);
        RECT bounds{0, 0, 1, 1};
        g_controller->put_Bounds(bounds);
        hr = g_controller->get_CoreWebView2(&g_webView);
        if (FAILED(hr) || !g_webView) {
            g_webViewFinished = true;
            KillTimer(m_window, kWebViewTimerId);
            Result(L"FAIL", L"WebView2 Core", L"获取失败，HRESULT " + HexHr(hr));
            BeginConsoleStartupProbe();
            return S_OK;
        }
        auto* processFailed = new ProcessFailedHandler(m_window, m_attempt);
        EventRegistrationToken failedToken{};
        hr = g_webView->add_ProcessFailed(processFailed, &failedToken);
        processFailed->Release();
        if (FAILED(hr)) {
            Result(L"WARN", L"WebView2 ProcessFailed", L"无法注册子进程故障事件，HRESULT " + HexHr(hr));
        }
        auto* navigation = new NavigationHandler(m_window, m_attempt);
        EventRegistrationToken token{};
        hr = g_webView->add_NavigationCompleted(navigation, &token);
        navigation->Release();
        if (FAILED(hr)) {
            g_webViewFinished = true;
            KillTimer(m_window, kWebViewTimerId);
            Result(L"FAIL", L"WebView2 NavigationCompleted", L"事件注册失败，HRESULT " + HexHr(hr));
            BeginConsoleStartupProbe();
            return S_OK;
        }
        hr = g_webView->NavigateToString(g_consoleHtml.c_str());
        if (FAILED(hr)) {
            g_webViewFinished = true;
            KillTimer(m_window, kWebViewTimerId);
            Result(L"FAIL", L"WebView2 NavigateToString", L"调用失败，HRESULT " + HexHr(hr));
            BeginConsoleStartupProbe();
        } else {
            Result(L"INFO", L"WebView2 页面加载", L"已导航到 Console 内嵌 HTML，等待完成事件");
        }
        return S_OK;
    }
private:
    volatile LONG m_refs = 1;
    HWND m_window;
    ULONGLONG m_attempt;
};

HRESULT EnvironmentHandler::Invoke(HRESULT hr, ICoreWebView2Environment* environment) {
    if (!IsWindow(m_window) || g_webViewFinished || m_attempt != g_webViewAttempt) return S_OK;
    if (FAILED(hr) || !environment) {
        g_webViewFinished = true;
        KillTimer(m_window, kWebViewTimerId);
        Result(L"FAIL", L"WebView2 Environment", L"创建失败，HRESULT " + HexHr(hr));
            BeginConsoleStartupProbe();
        return S_OK;
    }
    g_environment = environment;
    Result(L"PASS", L"WebView2 Environment", L"创建成功");
    auto* controller = new ControllerHandler(m_window, m_attempt);
    hr = g_environment->CreateCoreWebView2Controller(g_webViewHost, controller);
    controller->Release();
    if (FAILED(hr) && !g_webViewFinished) {
        g_webViewFinished = true;
        KillTimer(m_window, kWebViewTimerId);
        Result(L"FAIL", L"WebView2 Controller", L"创建请求失败，HRESULT " + HexHr(hr));
        BeginConsoleStartupProbe();
    }
    return S_OK;
}

void BeginWebViewProbe() {
    if (g_consoleHtml.empty()) {
        BeginConsoleStartupProbe();
        return;
    }
    ++g_webViewAttempt;
    if (g_webViewHost && IsWindow(g_webViewHost)) DestroyWindow(g_webViewHost);
    g_webViewFinished = false;
    g_webViewHost = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 1, 1,
        g_window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_webViewHost) {
        Result(L"FAIL", L"WebView2 测试窗口", L"创建隐藏宿主窗口失败，错误码 " + std::to_wstring(GetLastError()));
        BeginConsoleStartupProbe();
        return;
    }
    std::wstring temp(MAX_PATH, L'\0');
    DWORD length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    if (!length || length >= temp.size()) temp = g_installDir;
    else temp.resize(length);
    std::wstring userData = JoinPath(temp, L"FaceLoginDiag.WebView2");
    CreateDirectoryW(userData.c_str(), nullptr);
    SetTimer(g_window, kWebViewTimerId, kWebViewTimeoutMs, nullptr);
    auto* callback = new EnvironmentHandler(g_window, g_webViewAttempt);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr, callback);
    callback->Release();
    if (FAILED(hr)) {
        g_webViewFinished = true;
        KillTimer(g_window, kWebViewTimerId);
        Result(L"FAIL", L"WebView2 Environment", L"提交创建请求失败，HRESULT " + HexHr(hr));
        BeginConsoleStartupProbe();
    }
}

void RunDiagnostics() {
    if (g_diagnosticsRunning) return;
    g_report.clear();
    g_consoleHtml.clear();
    if (g_controller) g_controller->Close();
    g_webView.Reset();
    g_controller.Reset();
    g_environment.Reset();
    g_diagnosticsRunning = true;
    EnableWindow(g_runButton, FALSE);
    Line(L"FaceLogin 诊断报告");
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t timestamp[80] = {};
    swprintf_s(timestamp, L"生成时间：%04u-%02u-%02u %02u:%02u:%02u",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    Line(timestamp);
    Line(L"隐私说明：不读取用户名、邮箱、SID、密码或人脸数据；自动探测不会打开摄像头，取流测试须手动启动且不保存图像。");
    Line(L"");

    g_installDir = ExecutableDirectory();
    std::wstring dataDir;
    g_modelsDir = ReadRegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\FaceLogin", L"DataPath", dataDir)
        ? JoinPath(dataDir, L"models") : JoinPath(g_installDir, L"models");
    Result(L"INFO", L"Windows", OsDescription());
    const std::wstring diagVersion = CheckFile(L"FaceLoginDiag.exe", true);
    if (!diagVersion.empty()) Result(L"INFO", L"诊断工具版本", diagVersion);
    const std::wstring consoleVersion = CheckFile(L"FaceLoginConsole.exe", true);
    const std::wstring serviceVersion = CheckFile(L"FaceLoginService.exe", true);
    const std::wstring providerVersion = CheckFile(L"FaceLoginCredentialProvider.dll", true);
    if (!consoleVersion.empty() && !serviceVersion.empty() && !providerVersion.empty()) {
        const bool aligned = consoleVersion == serviceVersion && consoleVersion == providerVersion;
        Result(aligned ? L"PASS" : L"WARN", L"FaceLogin 主程序版本一致性",
            L"Console=" + consoleVersion + L"，Service=" + serviceVersion + L"，Provider=" + providerVersion);
    }
    CheckService();
    CheckModels();
    CheckRuntimeFiles();

    std::wstring htmlDetail;
    if (LoadConsoleHtml(g_consoleHtml, htmlDetail)) {
        const bool structure = g_consoleHtml.find(L"<head>") != std::wstring::npos &&
                               g_consoleHtml.find(L"<script>") != std::wstring::npos;
        Result(structure ? L"PASS" : L"WARN", L"Console 内嵌 HTML",
            htmlDetail + (structure ? L"，HTML/脚本标记正常" : L"，HTML 基础结构标记不完整"));
    } else {
        Result(L"FAIL", L"Console 内嵌 HTML", htmlDetail);
    }
    LPWSTR browserVersion = nullptr;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &browserVersion);
    if (SUCCEEDED(hr) && browserVersion) {
        Result(L"PASS", L"WebView2 Runtime", browserVersion);
        CoTaskMemFree(browserVersion);
    } else {
        Result(L"FAIL", L"WebView2 Runtime", L"无法查询 Evergreen Runtime，HRESULT " + HexHr(hr));
    }
    CheckCameraEnumeration();
    CheckCrashEvents();
    Line(L"");
    Line(L"Console 启动测试：在隔离隐藏进程中运行实际 FaceLoginConsole.exe，检查 WebView2、内嵌页面、导航页签和安全桥接；禁止打开摄像头，最多等待 25 秒。");
    Line(L"模型测试：在独立子进程中加载 ONNX Runtime 并为模型创建真实 InferenceSession；不访问摄像头，最多等待 90 秒。");
    Line(L"摄像头取流测试：选择设备并点击按钮后才会短暂打开摄像头；隔离进程最多运行 12 秒，首帧立即丢弃。若设备正被 Console 或其他程序占用，测试可能失败。");
    BeginWebViewProbe();
}

void ExportReport() {
    wchar_t path[MAX_PATH] = L"FaceLoginDiag-report.txt";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_window;
    dialog.lpstrFilter = L"文本报告 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"txt";
    if (!GetSaveFileNameW(&dialog)) return;
    const int count = WideCharToMultiByte(CP_UTF8, 0, g_report.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        MessageBoxW(g_window, L"无法编码报告。", L"FaceLogin 诊断", MB_ICONERROR);
        return;
    }
    std::vector<char> utf8(count);
    WideCharToMultiByte(CP_UTF8, 0, g_report.c_str(), -1, utf8.data(), count, nullptr, nullptr);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    constexpr char bom[] = {static_cast<char>(0xEF), static_cast<char>(0xBB), static_cast<char>(0xBF)};
    output.write(bom, sizeof(bom));
    output.write(utf8.data(), count - 1);
    if (!output) MessageBoxW(g_window, L"报告保存失败。", L"FaceLogin 诊断", MB_ICONERROR);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            g_window = window;
            HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            g_reportEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"正在准备诊断...",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | ES_NOHIDESEL,
                16, 56, 860, 520, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kReportId)), GetModuleHandleW(nullptr), nullptr);
            g_runButton = CreateWindowExW(0, L"BUTTON", L"重新检测", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                16, 586, 120, 32, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRunId)), GetModuleHandleW(nullptr), nullptr);
            g_exportButton = CreateWindowExW(0, L"BUTTON", L"导出报告", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                150, 586, 120, 32, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kExportId)), GetModuleHandleW(nullptr), nullptr);
            g_cameraCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                284, 586, 340, 240, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCameraProbeId + 1)), GetModuleHandleW(nullptr), nullptr);
            g_cameraProbeButton = CreateWindowExW(0, L"BUTTON", L"测试摄像头取流",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                634, 586, 220, 32, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCameraProbeId)), GetModuleHandleW(nullptr), nullptr);
            EnableWindow(g_cameraProbeButton, FALSE);
            SendMessageW(g_reportEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(g_runButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(g_exportButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(g_cameraCombo, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(g_cameraProbeButton, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return 0;
        }
        case WM_SIZE: {
            const int width = LOWORD(lParam), height = HIWORD(lParam);
            if (g_reportEdit) MoveWindow(g_reportEdit, 16, 56, std::max(100, width - 32), std::max(100, height - 110), TRUE);
            if (g_runButton) MoveWindow(g_runButton, 16, std::max(62, height - 44), 120, 32, TRUE);
            if (g_exportButton) MoveWindow(g_exportButton, 150, std::max(62, height - 44), 120, 32, TRUE);
            if (g_cameraCombo) MoveWindow(g_cameraCombo, 284, std::max(62, height - 44),
                std::max(120, width - 284 - 250), 240, TRUE);
            if (g_cameraProbeButton) MoveWindow(g_cameraProbeButton,
                std::max(414, width - 234), std::max(62, height - 44), 220, 32, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == kRunId) { RunDiagnostics(); return 0; }
            if (LOWORD(wParam) == kExportId) { ExportReport(); return 0; }
            if (LOWORD(wParam) == kCameraProbeId) { BeginCameraProbe(); return 0; }
            break;
        case WM_TIMER:
            if (wParam == kAuxProbeTimerId && g_auxProbeRunning) {
                PollAuxProbe();
                return 0;
            }
            if (wParam == kCameraProbeTimerId && g_cameraProbeRunning) {
                const ULONGLONG now = GetTickCount64();
                if (!g_cameraProbeTimedOut && now >= g_cameraProbeDeadline) {
                    g_cameraProbeTimedOut = true;
                    g_cameraProbeTerminationTick = now;
                    TerminateJobObject(g_cameraProbeJob, ERROR_TIMEOUT);
                }
                const DWORD wait = WaitForSingleObject(g_cameraProbeProcess, 0);
                if (wait == WAIT_OBJECT_0) {
                    FinishCameraProbe();
                } else if (wait == WAIT_FAILED) {
                    g_cameraProbeWaitError = GetLastError();
                    TerminateJobObject(g_cameraProbeJob, g_cameraProbeWaitError);
                    FinishCameraProbe();
                } else if (g_cameraProbeTimedOut && !g_cameraProbeForcedTermination &&
                           now - g_cameraProbeTerminationTick >= 3000) {
                    g_cameraProbeForcedTermination = true;
                    g_cameraProbeTerminationTick = now;
                    TerminateProcess(g_cameraProbeProcess, ERROR_TIMEOUT);
                } else if (g_cameraProbeTimedOut && g_cameraProbeForcedTermination &&
                           now - g_cameraProbeTerminationTick >= 3000) {
                    FinishCameraProbe();
                }
                return 0;
            }
            if (wParam == kWebViewTimerId && !g_webViewFinished) {
                g_webViewFinished = true;
                KillTimer(window, kWebViewTimerId);
                Result(L"FAIL", L"WebView2 初始化/导航超时", L"15 秒内未收到完成回调；请检查 Runtime、目录权限或安全软件拦截");
                BeginConsoleStartupProbe();
                return 0;
            }
            break;
        case WM_CLOSE:
            KillTimer(window, kWebViewTimerId);
            KillTimer(window, kCameraProbeTimerId);
            KillTimer(window, kAuxProbeTimerId);
            if (g_auxProbeRunning) {
                TerminateJobObject(g_auxProbeJob, ERROR_CANCELLED);
                WaitForSingleObject(g_auxProbeProcess, 3000);
                CloseHandle(g_auxProbeJob);
                CloseHandle(g_auxProbeProcess);
                CloseHandle(g_auxProbeReadPipe);
                g_auxProbeJob = nullptr;
                g_auxProbeProcess = nullptr;
                g_auxProbeReadPipe = nullptr;
                g_auxProbeRunning = false;
            }
            if (g_cameraProbeRunning) {
                TerminateJobObject(g_cameraProbeJob, ERROR_CANCELLED);
                CloseHandle(g_cameraProbeJob);
                CloseHandle(g_cameraProbeProcess);
                CloseHandle(g_cameraProbeReadPipe);
                g_cameraProbeJob = nullptr;
                g_cameraProbeProcess = nullptr;
                g_cameraProbeReadPipe = nullptr;
                g_cameraProbeRunning = false;
            }
            if (g_controller) g_controller->Close();
            g_webView.Reset();
            g_controller.Reset();
            g_environment.Reset();
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            if (g_webViewHost && IsWindow(g_webViewHost)) DestroyWindow(g_webViewHost);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
    UINT32 cameraIndex = 0;
    if (commandLine && swscanf_s(commandLine, L"--camera-probe %u", &cameraIndex) == 1) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const int result = CameraProbeChild(cameraIndex);
        CoUninitialize();
        return result;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_FACELOGIN_DIAG));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&wc)) { CoUninitialize(); return 1; }
    HWND window = CreateWindowExW(0, kWindowClass, L"FaceLogin 诊断工具",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 920, 690,
        nullptr, nullptr, instance, nullptr);
    if (!window) { CoUninitialize(); return 1; }
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    RunDiagnostics();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
