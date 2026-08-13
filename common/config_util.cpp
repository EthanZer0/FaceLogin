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
// Handles \" , \\ and \uXXXX escapes (backslash-heavy values like camera
// device symbolic links would otherwise get corrupted by the round-trip;
// \uXXXX is emitted by JSON.stringify for characters like '&' in WebView2).
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

    // Parse the quoted string, unescaping \\ , \" and \uXXXX as we go.
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
            if (next == 'u' && pos + 5 < json.size()) {
                // \uXXXX — parse 4 hex digits into a UTF-8 char.
                bool ok = true;
                unsigned int code = 0;
                for (int i = 0; i < 4; i++) {
                    char h = json[pos + 2 + i];
                    int v = -1;
                    if (h >= '0' && h <= '9') v = h - '0';
                    else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
                    if (v < 0) { ok = false; break; }
                    code = (code << 4) | static_cast<unsigned int>(v);
                }
                if (ok) {
                    // Encode the code point as UTF-8.
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    pos += 6;
                    continue;
                }
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

static int jsonGetInt(const std::string& json, const std::string& key, int defVal) {
    auto s = jsonGetString(json, key);
    if (s.empty()) return defVal;
    try { return std::stoi(s); } catch (...) { return defVal; }
}

std::string ConfigToJson(const AppConfig& cfg) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  "; jsonWriteString(ss, "recognition_model"); ss << ": "; jsonWriteString(ss, cfg.recognition_model); ss << ",\n";
    ss << "  "; jsonWriteString(ss, "detector"); ss << ": "; jsonWriteString(ss, cfg.detector); ss << ",\n";
    ss << "  "; jsonWriteString(ss, "liveness_method"); ss << ": "; jsonWriteString(ss, LivenessMethodToString(cfg.liveness_method)); ss << ",\n";
    ss << "  "; jsonWriteString(ss, "match_threshold"); ss << ": " << cfg.match_threshold << ",\n";
    ss << "  "; jsonWriteString(ss, "anti_spoof_threshold"); ss << ": " << cfg.anti_spoof_threshold << ",\n";
    ss << "  "; jsonWriteString(ss, "blink_glasses_mode"); ss << ": " << (cfg.blink_glasses_mode ? "true" : "false") << ",\n";
    ss << "  "; jsonWriteString(ss, "low_light_enhance"); ss << ": " << (cfg.low_light_enhance ? "true" : "false") << ",\n";
    ss << "  "; jsonWriteString(ss, "unload_models_after_auth"); ss << ": " << (cfg.unload_models_after_auth ? "true" : "false") << ",\n";
    ss << "  "; jsonWriteString(ss, "camera_rotation"); ss << ": " << cfg.camera_rotation << ",\n";
    ss << "  "; jsonWriteString(ss, "capture_unknown_faces"); ss << ": " << (cfg.capture_unknown_faces ? "true" : "false") << ",\n";
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
    cfg.match_threshold = jsonGetFloat(json, "match_threshold", 0.65f);
    cfg.anti_spoof_threshold = jsonGetFloat(json, "anti_spoof_threshold", 0.30f);
    cfg.blink_glasses_mode = (jsonGetString(json, "blink_glasses_mode") == "true");
    cfg.low_light_enhance = (jsonGetString(json, "low_light_enhance") == "true");
    cfg.unload_models_after_auth = (jsonGetString(json, "unload_models_after_auth") == "true");
    int rotation = jsonGetInt(json, "camera_rotation", 0);
    // Only accept 0/90/180/270; anything else silently does nothing in
    // RotateFrame, so fall back to 0 and log it — a configured-but-ignored
    // rotation is a silent failure the user can't diagnose.
    if (rotation != 90 && rotation != 180 && rotation != 270) {
        if (rotation != 0)
            FACELOGIN_WARN(L"Invalid camera_rotation=%d in config, falling back to 0", rotation);
        rotation = 0;
    }
    cfg.camera_rotation = rotation;
    cfg.capture_unknown_faces = (jsonGetString(json, "capture_unknown_faces") == "true");
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
    FACELOGIN_INFO(L"Loaded config.json: rec=%hs det=%hs live=%hs thr=%.2f camera=%hs rotation=%d",
                  cfg.recognition_model.c_str(), cfg.detector.c_str(),
                  LivenessMethodToString(cfg.liveness_method).c_str(),
                  cfg.match_threshold, cfg.camera_device.c_str(),
                  cfg.camera_rotation);
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
    return "none";
}

LivenessMethod LivenessMethodFromString(const std::string& s) {
    if (s == "blink")     return LivenessMethod::Blink;
    if (s == "antispoof") return LivenessMethod::AntiSpoof;
    return LivenessMethod::None; // default: no liveness check
}

} // namespace facelogin
