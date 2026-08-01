#include "config_util.h"
#include "registry_util.h"
#include "logger.h"
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace facelogin {

static std::wstring ResolveDataDir(const std::wstring& dataDir) {
    if (!dataDir.empty()) return dataDir;
    auto fromReg = ReadRegString(REGVAL_DATA_PATH, L"");
    if (!fromReg.empty()) return fromReg;
    wchar_t programData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData)))
        return std::wstring(programData) + L"\\FaceLogin";
    return L"C:\\ProgramData\\FaceLogin";
}

// Simple JSON writer — flat struct, no nested objects
static void jsonWriteString(std::ostringstream& ss, const std::string& s) {
    ss << '"';
    for (char c : s) {
        if (c == '"' || c == '\\') ss << '\\';
        ss << c;
    }
    ss << '"';
}

// Minimal JSON parser for flat objects. Returns empty string on error.
// Handles \" and \\ escapes (backslash-heavy values like camera device
// symbolic links would otherwise get corrupted by the round-trip).
static std::string jsonGetString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    // skip ':'
    pos++;
    // skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) pos++;
    if (pos >= json.size()) return "";
    if (json[pos] != '"') {
        // Number or literal
        auto end = json.find_first_of(",}\n\r \t", pos);
        if (end == std::string::npos) return json.substr(pos);
        return json.substr(pos, end - pos);
    }

    // Parse the quoted string, unescaping \\ and \" as we go.
    pos++;  // skip opening quote
    std::string out;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '"') break;               // closing quote
        if (c == '\\' && pos + 1 < json.size()) {
            char next = json[pos + 1];
            if (next == '\\' || next == '"') {
                out.push_back(next);       // \\ → \  and  \" → "
                pos += 2;
                continue;
            }
            // Unknown escape: keep the backslash literally.
            out.push_back(c);
            pos++;
            continue;
        }
        out.push_back(c);
        pos++;
    }
    return out;
}

static float jsonGetFloat(const std::string& json, const std::string& key, float defVal) {
    auto s = jsonGetString(json, key);
    if (s.empty()) return defVal;
    try { return std::stof(s); } catch (...) { return defVal; }
}

std::string ConfigToJson(const AppConfig& cfg) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  "; jsonWriteString(ss, "recognition_model"); ss << ": "; jsonWriteString(ss, cfg.recognition_model); ss << ",\n";
    ss << "  "; jsonWriteString(ss, "detector"); ss << ": "; jsonWriteString(ss, cfg.detector); ss << ",\n";
    ss << "  "; jsonWriteString(ss, "liveness_method"); ss << ": "; jsonWriteString(ss, LivenessMethodToString(cfg.liveness_method)); ss << ",\n";
    ss << "  "; jsonWriteString(ss, "match_threshold"); ss << ": " << cfg.match_threshold << ",\n";
    ss << "  "; jsonWriteString(ss, "anti_spoof_threshold"); ss << ": " << cfg.anti_spoof_threshold << ",\n";
    ss << "  "; jsonWriteString(ss, "camera_device"); ss << ": "; jsonWriteString(ss, cfg.camera_device); ss << "\n";
    ss << "}\n";
    return ss.str();
}

AppConfig ConfigFromJson(const std::string& json) {
    AppConfig cfg = DefaultConfig();
    auto rec = jsonGetString(json, "recognition_model");
    if (!rec.empty()) cfg.recognition_model = rec;
    auto det = jsonGetString(json, "detector");
    if (!det.empty()) cfg.detector = det;
    auto live = jsonGetString(json, "liveness_method");
    if (!live.empty()) cfg.liveness_method = LivenessMethodFromString(live);
    cfg.match_threshold = jsonGetFloat(json, "match_threshold", 0.30f);
    cfg.anti_spoof_threshold = jsonGetFloat(json, "anti_spoof_threshold", 0.30f);
    auto cam = jsonGetString(json, "camera_device");
    if (!cam.empty()) cfg.camera_device = cam;
    return cfg;
}

AppConfig DefaultConfig() {
    return AppConfig{};
}

AppConfig LoadConfig(const std::wstring& dataDir) {
    std::wstring dir = ResolveDataDir(dataDir);
    std::wstring dataSubDir = dir + L"\\data";
    CreateDirectoryW(dataSubDir.c_str(), nullptr);
    std::wstring path = dataSubDir + L"\\config.json";

    std::ifstream file(path);
    if (!file.is_open()) {
        FACELOGIN_INFO(L"No config.json found, using defaults + registry");
        AppConfig cfg = DefaultConfig();
        // Fall back to registry match threshold if set
        float regThresh = 0.30f;
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, FACELOGIN_REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD val = 0, size = sizeof(val);
            if (RegQueryValueExW(hKey, L"MatchThreshold", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS) {
                regThresh = val / 100.0f;
            }
            RegCloseKey(hKey);
        }
        cfg.match_threshold = regThresh;
        return cfg;
    }

    std::stringstream buf;
    buf << file.rdbuf();
    file.close();

    AppConfig cfg = ConfigFromJson(buf.str());
    FACELOGIN_INFO(L"Loaded config.json: rec=%hs det=%hs live=%hs thr=%.2f camera=%hs",
                  cfg.recognition_model.c_str(), cfg.detector.c_str(),
                  LivenessMethodToString(cfg.liveness_method).c_str(),
                  cfg.match_threshold, cfg.camera_device.c_str());
    return cfg;
}

bool SaveConfig(const std::wstring& dataDir, const AppConfig& cfg) {
    std::wstring dir = ResolveDataDir(dataDir);
    std::wstring dataSubDir = dir + L"\\data";
    CreateDirectoryW(dataSubDir.c_str(), nullptr);
    std::wstring path = dataSubDir + L"\\config.json";

    std::string content = ConfigToJson(cfg);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        FACELOGIN_ERROR(L"Failed to write config.json: %s", path.c_str());
        return false;
    }
    file.write(content.c_str(), content.size());
    file.close();
    FACELOGIN_INFO(L"Saved config.json");
    return true;
}

std::string LivenessMethodToString(LivenessMethod m) {
    switch (m) {
        case LivenessMethod::Blink:     return "blink";
        case LivenessMethod::AntiSpoof: return "antispoof";
        case LivenessMethod::None:      return "none";
    }
    return "blink";
}

LivenessMethod LivenessMethodFromString(const std::string& s) {
    if (s == "antispoof") return LivenessMethod::AntiSpoof;
    if (s == "none")      return LivenessMethod::None;
    return LivenessMethod::Blink; // default
}

} // namespace facelogin
