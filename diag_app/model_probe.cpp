#include <windows.h>
#include <shellapi.h>
#include <onnxruntime_cxx_api.h>
#include <array>
#include <string>

namespace {
struct ModelEntry {
    const wchar_t* name;
    bool required;
};

constexpr std::array<ModelEntry, 5> kModels{{
    {L"2d106det.onnx", true},
    {L"det_500m.onnx", true},
    {L"w600k_mbf.onnx", true},
    {L"head_pose_mobilenetv2.onnx", false},
    {L"minifas_quantized.onnx", false},
}};

std::wstring JoinPath(const std::wstring& dir, const wchar_t* name) {
    return dir.empty() || dir.back() == L'\\' ? dir + name : dir + L"\\" + name;
}

bool Exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

void Print(const char* status, const char* name, const char* detail) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) return;
    const std::string line = std::string(status) + "|" + name + "|" + detail + "\r\n";
    DWORD written = 0;
    WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc != 2) {
        Print("FAIL", "probe", "invalid_arguments");
        if (argv) LocalFree(argv);
        return 2;
    }
    const std::wstring modelsDir = argv[1];
    LocalFree(argv);

    try {
        Ort::Env environment(ORT_LOGGING_LEVEL_FATAL, "FaceLoginDiag");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);
        options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_DISABLE_ALL);
        const std::string runtimeVersion = Ort::GetVersionString();
        Print("PASS", "onnxruntime", runtimeVersion.c_str());

        bool requiredModelsPassed = true;
        for (const ModelEntry& model : kModels) {
            const std::wstring path = JoinPath(modelsDir, model.name);
            const std::string name = [&]() {
                const int count = WideCharToMultiByte(CP_UTF8, 0, model.name, -1, nullptr, 0, nullptr, nullptr);
                if (count <= 1) return std::string("model");
                std::string value(static_cast<size_t>(count), '\0');
                WideCharToMultiByte(CP_UTF8, 0, model.name, -1, value.data(), count, nullptr, nullptr);
                value.pop_back();
                return value;
            }();

            if (!Exists(path)) {
                Print(model.required ? "FAIL" : "WARN", name.c_str(), "missing");
                if (model.required) requiredModelsPassed = false;
                continue;
            }

            try {
                Ort::Session session(environment, path.c_str(), options);
                const size_t inputs = session.GetInputCount();
                const size_t outputs = session.GetOutputCount();
                if (inputs == 0 || outputs == 0) {
                    Print(model.required ? "FAIL" : "WARN", name.c_str(), "empty_graph");
                    if (model.required) requiredModelsPassed = false;
                    continue;
                }
                const std::string detail = "loaded;inputs=" + std::to_string(inputs) +
                                           ";outputs=" + std::to_string(outputs);
                Print("PASS", name.c_str(), detail.c_str());
            } catch (const Ort::Exception& error) {
                const std::string detail = "onnx_error=" + std::to_string(static_cast<int>(error.GetOrtErrorCode()));
                Print(model.required ? "FAIL" : "WARN", name.c_str(), detail.c_str());
                if (model.required) requiredModelsPassed = false;
            }
        }
        return requiredModelsPassed ? 0 : 1;
    } catch (const Ort::Exception& error) {
        const std::string detail = "onnxruntime_error=" + std::to_string(static_cast<int>(error.GetOrtErrorCode()));
        Print("FAIL", "onnxruntime", detail.c_str());
        return 3;
    } catch (...) {
        Print("FAIL", "onnxruntime", "unexpected_error");
        return 4;
    }
}
