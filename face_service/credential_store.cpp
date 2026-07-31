#include "credential_store.h"
#include "../common/logger.h"
#include "../common/dpapi_util.h"
#include <shlobj.h>
#include <fstream>
#include <algorithm>
#include <sddl.h>

namespace facelogin {

static constexpr uint32_t FILE_MAGIC = 0x474F4C46; // "FLOG" in little-endian
static constexpr uint32_t FILE_VERSION = 2;

// Helper: try to look up SID and UPN from a username.
// Used for upgrading V1 databases on the fly.
static void LookupUserIdentity(const std::wstring& username,
                                std::wstring& outSid,
                                std::wstring& outUpn) {
    outSid.clear();
    outUpn.clear();

    // Get SID via LookupAccountNameW
    DWORD sidSize = 0, domainSize = 0;
    SID_NAME_USE sidType;
    LookupAccountNameW(nullptr, username.c_str(),
                       nullptr, &sidSize, nullptr, &domainSize, &sidType);
    if (sidSize > 0) {
        std::vector<BYTE> sidBuf(sidSize);
        std::vector<wchar_t> domainBuf(domainSize > 0 ? domainSize : 1);
        if (LookupAccountNameW(nullptr, username.c_str(),
                               sidBuf.data(), &sidSize,
                               domainBuf.data(), &domainSize, &sidType)) {
            LPWSTR sidStr = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<PSID>(sidBuf.data()), &sidStr)) {
                outSid = sidStr;
                LocalFree(sidStr);
            }
        }
    }

    // Get UPN via GetUserNameExW (dynamic bind to avoid secext.h conflicts)
    HMODULE hSecur32 = LoadLibraryW(L"secur32.dll");
    if (hSecur32) {
        typedef BOOLEAN (WINAPI *PFN_GetUserNameExW)(int, LPWSTR, PULONG);
        auto pfn = reinterpret_cast<PFN_GetUserNameExW>(
            GetProcAddress(hSecur32, "GetUserNameExW"));
        if (pfn) {
            ULONG upnSize = 256;
            std::vector<wchar_t> upnBuf(upnSize);
            if (pfn(8 /* NameUserPrincipal */, upnBuf.data(), &upnSize)) {
                outUpn = upnBuf.data();
            }
        }
        FreeLibrary(hSecur32);
    }
}

std::wstring CredentialStore::GetDataDir() const {
    if (!m_dataDir.empty()) return m_dataDir;

    // Default: %PROGRAMDATA%\FaceLogin
    wchar_t programData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
        return std::wstring(programData) + L"\\FaceLogin";
    }
    return L"C:\\ProgramData\\FaceLogin";
}

bool CredentialStore::EnsureDataDir() {
    std::wstring dir = GetDataDir();
    if (CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return true;
    }
    FACELOGIN_ERROR(L"Failed to create data directory: %s", dir.c_str());
    return false;
}

bool CredentialStore::LoadDatabase() {
    std::wstring path = GetDataDir() + L"\\data\\users.dat";
    m_users.clear();

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        FACELOGIN_INFO(L"No existing database at %s (this is normal on first run)", path.c_str());
        return true; // Empty database is valid
    }

    // Read header
    uint32_t magic = 0, version = 0, count = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    if (magic != FILE_MAGIC) {
        FACELOGIN_ERROR(L"Invalid database file (bad magic: 0x%08X)", magic);
        return false;
    }
    if (version != FILE_VERSION && version != 1) {
        FACELOGIN_ERROR(L"Unsupported database version: %u", version);
        return false;
    }

    FACELOGIN_INFO(L"Loading %u user record(s) from database (v%u)", count, version);

    for (uint32_t i = 0; i < count; i++) {
        UserRecord rec = {};

        // Username
        uint32_t nameLen = 0;
        file.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        if (nameLen == 0 || nameLen > 256) {
            FACELOGIN_ERROR(L"Invalid username length: %u", nameLen);
            return false;
        }
        std::vector<wchar_t> nameBuf(nameLen + 1, 0);
        file.read(reinterpret_cast<char*>(nameBuf.data()), nameLen * sizeof(wchar_t));
        rec.username = nameBuf.data();

        if (version >= 2) {
            // UPN
            uint32_t upnLen = 0;
            file.read(reinterpret_cast<char*>(&upnLen), sizeof(upnLen));
            if (upnLen > 256) {
                FACELOGIN_ERROR(L"Invalid UPN length: %u", upnLen);
                return false;
            }
            if (upnLen > 0) {
                std::vector<wchar_t> upnBuf(upnLen + 1, 0);
                file.read(reinterpret_cast<char*>(upnBuf.data()), upnLen * sizeof(wchar_t));
                rec.upn = upnBuf.data();
            }

            // SID
            uint32_t sidLen = 0;
            file.read(reinterpret_cast<char*>(&sidLen), sizeof(sidLen));
            if (sidLen > 512) {
                FACELOGIN_ERROR(L"Invalid SID length: %u", sidLen);
                return false;
            }
            if (sidLen > 0) {
                std::vector<wchar_t> sidBuf(sidLen + 1, 0);
                file.read(reinterpret_cast<char*>(sidBuf.data()), sidLen * sizeof(wchar_t));
                rec.sid = sidBuf.data();
            }
        }

        // Password
        uint32_t passLen = 0;
        file.read(reinterpret_cast<char*>(&passLen), sizeof(passLen));
        if (passLen == 0 || passLen > 4096) {
            FACELOGIN_ERROR(L"Invalid password length: %u", passLen);
            return false;
        }
        rec.encryptedPassword.resize(passLen);
        file.read(reinterpret_cast<char*>(rec.encryptedPassword.data()), passLen);

        // Embedding (128 floats)
        file.read(reinterpret_cast<char*>(rec.embedding), 128 * sizeof(float));

        if (file.good()) {
            // V1 → V2 upgrade: look up SID/UPN for existing records
            if (version < 2 && rec.sid.empty()) {
                LookupUserIdentity(rec.username, rec.sid, rec.upn);
                FACELOGIN_INFO(L"Upgraded V1 record '%s' → SID=%s UPN=%s",
                              rec.username.c_str(), rec.sid.c_str(), rec.upn.c_str());
            }
            m_users.push_back(std::move(rec));
        } else {
            FACELOGIN_ERROR(L"Failed to read record %u", i);
            return false;
        }
    }

    FACELOGIN_INFO(L"Loaded %zu user(s) successfully", m_users.size());
    return true;
}

bool CredentialStore::SaveDatabase() {
    std::wstring dataDir = GetDataDir() + L"\\data";
    CreateDirectoryW(dataDir.c_str(), nullptr);
    if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != 0) {
        FACELOGIN_ERROR(L"Failed to create data directory: %s", dataDir.c_str());
        return false;
    }

    std::wstring path = GetDataDir() + L"\\data\\users.dat";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        FACELOGIN_ERROR(L"Failed to open database for writing: %s", path.c_str());
        return false;
    }

    // Write header
    uint32_t magic = FILE_MAGIC;
    uint32_t version = FILE_VERSION;
    uint32_t count = static_cast<uint32_t>(m_users.size());
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& rec : m_users) {
        // Username
        uint32_t nameLen = static_cast<uint32_t>(rec.username.size());
        file.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        file.write(reinterpret_cast<const char*>(rec.username.c_str()),
                   nameLen * sizeof(wchar_t));

        // UPN
        uint32_t upnLen = static_cast<uint32_t>(rec.upn.size());
        file.write(reinterpret_cast<const char*>(&upnLen), sizeof(upnLen));
        if (upnLen > 0) {
            file.write(reinterpret_cast<const char*>(rec.upn.c_str()),
                       upnLen * sizeof(wchar_t));
        }

        // SID
        uint32_t sidLen = static_cast<uint32_t>(rec.sid.size());
        file.write(reinterpret_cast<const char*>(&sidLen), sizeof(sidLen));
        if (sidLen > 0) {
            file.write(reinterpret_cast<const char*>(rec.sid.c_str()),
                       sidLen * sizeof(wchar_t));
        }

        // Password
        uint32_t passLen = static_cast<uint32_t>(rec.encryptedPassword.size());
        file.write(reinterpret_cast<const char*>(&passLen), sizeof(passLen));
        file.write(reinterpret_cast<const char*>(rec.encryptedPassword.data()), passLen);

        // Embedding
        file.write(reinterpret_cast<const char*>(rec.embedding), 128 * sizeof(float));
    }

    file.close();
    FACELOGIN_INFO(L"Saved %zu user(s) to database (v2)", m_users.size());
    return true;
}

bool CredentialStore::AddUser(const std::wstring& username,
                               const std::wstring& upn,
                               const std::wstring& sid,
                               const std::vector<uint8_t>& encryptedPassword,
                               const float embedding[128]) {
    // Check if user already exists (match by SID if available, then username)
    auto it = m_users.end();
    if (!sid.empty()) {
        it = std::find_if(m_users.begin(), m_users.end(),
            [&sid](const UserRecord& r) { return r.sid == sid; });
    }
    if (it == m_users.end() && !upn.empty()) {
        it = std::find_if(m_users.begin(), m_users.end(),
            [&upn](const UserRecord& r) { return r.upn == upn; });
    }
    if (it == m_users.end()) {
        it = std::find_if(m_users.begin(), m_users.end(),
            [&username](const UserRecord& r) { return r.username == username; });
    }

    if (it != m_users.end()) {
        it->username = username;
        it->upn = upn;
        it->sid = sid;
        it->encryptedPassword = encryptedPassword;
        memcpy(it->embedding, embedding, 128 * sizeof(float));
        FACELOGIN_INFO(L"Updated existing user: %s (SID=%s)", username.c_str(), sid.c_str());
    } else {
        UserRecord rec;
        rec.username = username;
        rec.upn = upn;
        rec.sid = sid;
        rec.encryptedPassword = encryptedPassword;
        memcpy(rec.embedding, embedding, 128 * sizeof(float));
        m_users.push_back(std::move(rec));
        FACELOGIN_INFO(L"Added new user: %s (SID=%s)", username.c_str(), sid.c_str());
    }

    return true;
}

bool CredentialStore::DeleteUser(const std::wstring& username) {
    auto it = std::remove_if(m_users.begin(), m_users.end(),
        [&username](const UserRecord& r) { return r.username == username; });

    if (it != m_users.end()) {
        m_users.erase(it, m_users.end());
        FACELOGIN_INFO(L"Deleted user: %s", username.c_str());
        return true;
    }

    FACELOGIN_WARN(L"User not found for deletion: %s", username.c_str());
    return false;
}

std::optional<CredentialStore::MatchResult> CredentialStore::FindBestMatch(
    const float probeEmbedding[128], float threshold) {

    if (m_users.empty()) {
        return std::nullopt;
    }

    float bestDist = 1e10f, secondBestDist = 1e10f;
    size_t bestIdx = 0;

    for (size_t i = 0; i < m_users.size(); i++) {
        float sum = 0.0f;
        for (int j = 0; j < 128; j++) {
            float diff = probeEmbedding[j] - m_users[i].embedding[j];
            sum += diff * diff;
        }
        float dist = std::sqrt(sum);

        if (dist < bestDist) {
            secondBestDist = bestDist;
            bestDist = dist;
            bestIdx = i;
        } else if (dist < secondBestDist) {
            secondBestDist = dist;
        }
    }

    // Reject if best match is not meaningfully better than second-best.
    // A ratio >= 0.75 means the probe is ambiguous between two users
    // (or between the real user and a noisy impostor).
    // Skip this check when only one user is enrolled — there is no
    // second-best to compare against.
    if (m_users.size() > 1 && secondBestDist < 1e9f) {
        float ratio = bestDist / secondBestDist;
        if (ratio >= 0.75f) {
            FACELOGIN_INFO(L"Match rejected: best/second-best ratio too high (%.3f/%.3f=%.3f)",
                          bestDist, secondBestDist, ratio);
            return std::nullopt;
        }
    }

    if (bestDist < threshold) {
        MatchResult best;
        best.distance = bestDist;
        best.upn = m_users[bestIdx].upn;
        best.sid = m_users[bestIdx].sid;
        best.username = m_users[bestIdx].username;
        // Decrypt the password
        auto plain = DpapiUtil::Unprotect(m_users[bestIdx].encryptedPassword);
        if (!plain.empty()) {
            // The password was stored as a wstring
            if (plain.size() % sizeof(wchar_t) == 0) {
                best.password.assign(
                    reinterpret_cast<const wchar_t*>(plain.data()),
                    plain.size() / sizeof(wchar_t));
            }
            // Zero the plaintext buffer
            SecureZeroMemory(plain.data(), plain.size());
        }

        if (!best.password.empty()) {
            return best;
        }
    }

    return std::nullopt;
}

} // namespace facelogin
