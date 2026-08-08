#include "credential_store.h"
#include "../common/logger.h"
#include "../common/dpapi_util.h"
#include <shlobj.h>
#include <fstream>
#include <algorithm>
#include <sddl.h>

namespace facelogin {

static constexpr uint32_t FILE_MAGIC = 0x474F4C46; // "FLOG" in little-endian
// V5 (1.6.0): V4 layout PLUS a per-face "legacy" flag (written right after the
// face id) that marks faces enrolled with the OLD pre-1.6.0 alignment. V4
// databases hold embeddings computed by the dlib 68-point chip-extraction
// alignment, which are NOT compatible with 1.6.0's 106-point similarity-
// transform alignment (verified: old data fails to match, recognition always
// times out). V5 is written after re-enrollment. A V4 file read by 1.6.0
// flags every face as legacy (greyed out in the Console) AND sets the
// "needs re-enrollment" flag instead of silently failing at match time.
static constexpr uint32_t FILE_VERSION = 5;
// The last version whose embeddings are compatible with the current recognizer
// alignment. Data with a lower version must be re-enrolled.
static constexpr uint32_t ALIGN_COMPAT_VERSION = 5;

// Helpers for V4 serialization
namespace {

// Default label for a face: L"脸N" where N = face id.
inline std::wstring DefaultFaceLabel(uint32_t id) {
    return L"脸" + std::to_wstring(id);  // 脸N
}

// Trim a face label for storage (empty → default). Keeps labels sane.
inline std::wstring NormalizeFaceLabel(const std::wstring& label, uint32_t id) {
    if (label.empty()) return DefaultFaceLabel(id);
    return label;
}

} // namespace

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
    // Accept v1..v5. v4 and below are loaded but flagged for re-enrollment
    // (their embeddings came from the old alignment).
    if (version > FILE_VERSION) {
        FACELOGIN_ERROR(L"Database version %u newer than supported (%u)", version, FILE_VERSION);
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
        // passLen 0 or 1 is valid: 0 = empty (defensive), 1 = passwordless
        // sentinel byte. Old versions only accepted >= 1; a 0-length record
        // would have failed their check, so we accept both here.
        if (passLen > 4096) {
            FACELOGIN_ERROR(L"Invalid password length: %u", passLen);
            return false;
        }
        rec.encryptedPassword.resize(passLen);
        if (passLen > 0) {
            file.read(reinterpret_cast<char*>(rec.encryptedPassword.data()), passLen);
        }

        if (version >= 4) {
            // V4/V5: one or more faces, each with id/label/embedding.
            uint32_t faceCount = 0;
            file.read(reinterpret_cast<char*>(&faceCount), sizeof(faceCount));
            // Writer is capped at kMaxFacesPerUser; read-side is lenient to
            // avoid killing the whole DB on a slightly-over spec file.
            if (faceCount < 1 || faceCount > 16) {
                FACELOGIN_ERROR(L"Invalid face count: %u", faceCount);
                return false;
            }
            rec.faces.reserve(faceCount);
            for (uint32_t f = 0; f < faceCount; f++) {
                FaceRecord face;
                file.read(reinterpret_cast<char*>(&face.id), sizeof(face.id));
                if (face.id < 1) {
                    FACELOGIN_ERROR(L"Invalid face id: %u", face.id);
                    return false;
                }
                // V5 added a per-face legacy flag after the id. V4 files have no
                // such field — their faces all came from the old alignment, so
                // flag every one of them.
                if (version >= 5) {
                    uint32_t legacy = 0;
                    file.read(reinterpret_cast<char*>(&legacy), sizeof(legacy));
                    face.legacy = (legacy != 0);
                } else {
                    face.legacy = true;
                }
                uint32_t labelLen = 0;
                file.read(reinterpret_cast<char*>(&labelLen), sizeof(labelLen));
                if (labelLen > 64) {
                    FACELOGIN_ERROR(L"Invalid face label length: %u", labelLen);
                    return false;
                }
                if (labelLen > 0) {
                    std::vector<wchar_t> labelBuf(labelLen + 1, 0);
                    file.read(reinterpret_cast<char*>(labelBuf.data()),
                              labelLen * sizeof(wchar_t));
                    face.label = labelBuf.data();
                }
                uint32_t embLen = 0;
                file.read(reinterpret_cast<char*>(&embLen), sizeof(embLen));
                // Sanity range: 64..4096 floats (covers 128-D dlib and 512-D ONNX)
                if (embLen < 64 || embLen > 4096) {
                    FACELOGIN_ERROR(L"Invalid embedding length: %u", embLen);
                    return false;
                }
                face.embedding.resize(embLen);
                file.read(reinterpret_cast<char*>(face.embedding.data()),
                          embLen * sizeof(float));
                rec.faces.push_back(std::move(face));
            }
        } else {
            // V1/V2/V3: a single embedding, upgraded in memory to one face.
            uint32_t embLen = 0;
            if (version >= 3) {
                file.read(reinterpret_cast<char*>(&embLen), sizeof(embLen));
                if (embLen < 64 || embLen > 4096) {
                    FACELOGIN_ERROR(L"Invalid embedding length: %u", embLen);
                    return false;
                }
            } else {
                embLen = 128;  // V1/V2 fixed 128 floats
            }
            FaceRecord face;
            face.id = 1;
            face.label = DefaultFaceLabel(1);
            face.legacy = true;  // V1/V2/V3 predate 1.6.0 alignment
            face.embedding.resize(embLen);
            file.read(reinterpret_cast<char*>(face.embedding.data()),
                      embLen * sizeof(float));
            rec.faces.push_back(std::move(face));
        }

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

    if (version < FILE_VERSION) {
        FACELOGIN_INFO(L"Upgraded database v%u → v%u in-memory (will be written on next save)",
                       version, FILE_VERSION);
    }

    // V4 or older embeddings came from the pre-1.6.0 alignment and cannot be
    // matched by the current recognizer. Flag for re-enrollment so the Console
    // can prompt instead of the user discovering it via a failed unlock.
    if (version < ALIGN_COMPAT_VERSION) {
        m_needsReenrollment = true;
        FACELOGIN_WARN(L"Database v%u: embeddings from old alignment — re-enrollment required", version);
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
    // Skip accounts with zero faces defensively — DeleteFace removes a whole
    // account when its last face is deleted, so this should never occur.
    uint32_t count = 0;
    for (const auto& rec : m_users) {
        if (!rec.faces.empty()) count++;
    }
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& rec : m_users) {
        if (rec.faces.empty()) continue;  // defensive: never persist 0-face account

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

        // Faces (V4)
        uint32_t faceCount = static_cast<uint32_t>(rec.faces.size());
        file.write(reinterpret_cast<const char*>(&faceCount), sizeof(faceCount));
        for (const auto& face : rec.faces) {
            uint32_t faceId = face.id;
            file.write(reinterpret_cast<const char*>(&faceId), sizeof(faceId));

            // V5: per-face legacy flag (1 = pre-1.6.0 alignment, display-only).
            uint32_t legacy = face.legacy ? 1 : 0;
            file.write(reinterpret_cast<const char*>(&legacy), sizeof(legacy));

            uint32_t labelLen = static_cast<uint32_t>(face.label.size());
            file.write(reinterpret_cast<const char*>(&labelLen), sizeof(labelLen));
            if (labelLen > 0) {
                file.write(reinterpret_cast<const char*>(face.label.c_str()),
                           labelLen * sizeof(wchar_t));
            }

            uint32_t embLen = static_cast<uint32_t>(face.embedding.size());
            file.write(reinterpret_cast<const char*>(&embLen), sizeof(embLen));
            if (embLen > 0) {
                file.write(reinterpret_cast<const char*>(face.embedding.data()),
                           embLen * sizeof(float));
            }
        }
    }

    file.close();
    // A successful save writes V5 (new alignment) data, so any prior
    // "old alignment, needs re-enrollment" flag is now resolved.
    m_needsReenrollment = false;
    FACELOGIN_INFO(L"Saved %zu user(s) to database (v%u)", count, FILE_VERSION);
    return true;
}

size_t CredentialStore::FindUserIndex(const std::wstring& sid,
                                      const std::wstring& upn,
                                      const std::wstring& username) const {
    if (!sid.empty()) {
        auto it = std::find_if(m_users.begin(), m_users.end(),
            [&sid](const UserRecord& r) { return r.sid == sid; });
        if (it != m_users.end()) return static_cast<size_t>(it - m_users.begin());
    }
    if (!upn.empty()) {
        auto it = std::find_if(m_users.begin(), m_users.end(),
            [&upn](const UserRecord& r) { return r.upn == upn; });
        if (it != m_users.end()) return static_cast<size_t>(it - m_users.begin());
    }
    if (!username.empty()) {
        auto it = std::find_if(m_users.begin(), m_users.end(),
            [&username](const UserRecord& r) { return r.username == username; });
        if (it != m_users.end()) return static_cast<size_t>(it - m_users.begin());
    }
    return m_users.size();
}

bool CredentialStore::AddFace(const std::wstring& username,
                              const std::wstring& upn,
                              const std::wstring& sid,
                              const std::vector<uint8_t>& encryptedPassword,
                              const std::vector<float>& embedding,
                              const std::wstring& label,
                              uint32_t* outFaceId) {
    size_t idx = FindUserIndex(sid, upn, username);

    if (idx < m_users.size()) {
        // Account exists → append a face, never touch stored password/faces.
        UserRecord& rec = m_users[idx];
        if (rec.faces.size() >= kMaxFacesPerUser) {
            FACELOGIN_WARN(L"AddFace rejected: %s already has %zu faces (max %zu)",
                           username.c_str(), rec.faces.size(), kMaxFacesPerUser);
            return false;
        }
        uint32_t newId = 1;
        for (const auto& f : rec.faces) {
            if (f.id >= newId) newId = f.id + 1;
        }
        FaceRecord face;
        face.id = newId;
        face.label = NormalizeFaceLabel(label, newId);
        face.embedding = embedding;
        rec.faces.push_back(std::move(face));
        // Refresh display identity in case username/upn/sid changed.
        rec.username = username;
        rec.upn = upn;
        rec.sid = sid;
        if (outFaceId) *outFaceId = newId;
        FACELOGIN_INFO(L"Appended face #%u to %s (SID=%s, emb=%zu-D, total=%zu)",
                       newId, username.c_str(), sid.c_str(), embedding.size(),
                       rec.faces.size());
        return true;
    }

    // Account not found → create it with the given password and first face.
    UserRecord rec;
    rec.username = username;
    rec.upn = upn;
    rec.sid = sid;
    rec.encryptedPassword = encryptedPassword;
    FaceRecord face;
    face.id = 1;
    face.label = NormalizeFaceLabel(label, 1);
    face.embedding = embedding;
    rec.faces.push_back(std::move(face));
    m_users.push_back(std::move(rec));
    if (outFaceId) *outFaceId = 1;
    FACELOGIN_INFO(L"Created user %s with first face (SID=%s, emb=%zu-D)",
                   username.c_str(), sid.c_str(), embedding.size());
    return true;
}

bool CredentialStore::AddUser(const std::wstring& username,
                               const std::wstring& upn,
                               const std::wstring& sid,
                               const std::vector<uint8_t>& encryptedPassword,
                               const std::vector<float>& embedding) {
    return AddFace(username, upn, sid, encryptedPassword, embedding);
}

bool CredentialStore::UpdateAccountIdentity(size_t idx,
                                            const std::wstring& username,
                                            const std::wstring& upn,
                                            const std::wstring& sid,
                                            const std::vector<uint8_t>& encryptedPassword) {
    if (idx >= m_users.size()) {
        FACELOGIN_WARN(L"UpdateAccountIdentity: index %zu out of range", idx);
        return false;
    }

    UserRecord& rec = m_users[idx];
    if (!rec.faces.empty()) {
        // In-place identity + password refresh; faces untouched.
        rec.username = username;
        rec.upn      = upn;
        rec.sid      = sid;
        rec.encryptedPassword = encryptedPassword;
        FACELOGIN_INFO(L"Updated identity of '%s' → username=%s SID=%s UPN=%s (faces preserved)",
                       username.c_str(), username.c_str(), sid.c_str(), upn.c_str());
        return true;
    }

    // rec has no faces (defensive — SaveDatabase never persists 0-face
    // records, but an in-memory edge could exist). Instead of persisting an
    // empty record, merge its identity into the matching live record.
    size_t target = m_users.size();
    if (!upn.empty()) target = FindUserIndex(L"", upn, L"");
    if (target >= m_users.size() && !username.empty()) target = FindUserIndex(L"", L"", username);
    if (target < m_users.size()) {
        UserRecord& dst = m_users[target];
        dst.username = username;
        dst.upn      = upn;
        dst.sid      = sid;
        dst.encryptedPassword = encryptedPassword;
        m_users.erase(m_users.begin() + static_cast<ptrdiff_t>(idx));
        FACELOGIN_INFO(L"Merged identity of empty stale record into existing account %s",
                       username.c_str());
        return true;
    }
    return false;
}

bool CredentialStore::DeleteFace(const std::wstring& sid, uint32_t faceId) {
    size_t idx = FindUserIndex(sid, L"", L"");
    if (idx >= m_users.size()) {
        FACELOGIN_WARN(L"DeleteFace: account not found (SID=%s)", sid.c_str());
        return false;
    }
    UserRecord& rec = m_users[idx];
    auto it = std::remove_if(rec.faces.begin(), rec.faces.end(),
        [faceId](const FaceRecord& f) { return f.id == faceId; });
    if (it == rec.faces.end()) {
        FACELOGIN_WARN(L"DeleteFace: face #%u not found for %s", faceId, sid.c_str());
        return false;
    }
    rec.faces.erase(it, rec.faces.end());
    FACELOGIN_INFO(L"Deleted face #%u from %s (%zu remaining)",
                   faceId, rec.username.c_str(), rec.faces.size());
    if (rec.faces.empty()) {
        // Last face removed → drop the account entirely so the login tile
        // (which reads the record count) doesn't show a tile that can never
        // match. Re-enrollment goes through the first-time flow again.
        m_users.erase(m_users.begin() + static_cast<ptrdiff_t>(idx));
        FACELOGIN_INFO(L"Removed account %s (no faces remain)", rec.username.c_str());
    }
    return true;
}

bool CredentialStore::ClearAllFaces(const std::wstring& sid) {
    return DeleteUserBySid(sid);
}

bool CredentialStore::DeleteUserBySid(const std::wstring& sid) {
    size_t idx = FindUserIndex(sid, L"", L"");
    if (idx >= m_users.size()) {
        FACELOGIN_WARN(L"DeleteUserBySid: account not found (SID=%s)", sid.c_str());
        return false;
    }
    std::wstring username = m_users[idx].username;
    m_users.erase(m_users.begin() + static_cast<ptrdiff_t>(idx));
    FACELOGIN_INFO(L"Deleted user %s (SID=%s)", username.c_str(), sid.c_str());
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

bool CredentialStore::RenameFace(const std::wstring& sid, uint32_t faceId,
                                 const std::wstring& label) {
    size_t idx = FindUserIndex(sid, L"", L"");
    if (idx >= m_users.size()) return false;
    UserRecord& rec = m_users[idx];
    for (auto& f : rec.faces) {
        if (f.id == faceId) {
            f.label = NormalizeFaceLabel(label, faceId);
            FACELOGIN_INFO(L"Renamed face #%u of %s → %s",
                           faceId, rec.username.c_str(), f.label.c_str());
            return true;
        }
    }
    return false;
}

size_t CredentialStore::GetFaceCount(const std::wstring& sid) const {
    size_t idx = FindUserIndex(sid, L"", L"");
    if (idx >= m_users.size()) return 0;
    return m_users[idx].faces.size();
}

std::optional<CredentialStore::MatchResult> CredentialStore::FindBestMatch(
    const float probeEmbedding[], size_t probeDim, float threshold) {

    if (m_users.empty() || probeDim == 0 || probeEmbedding == nullptr) {
        return std::nullopt;
    }

    // Account-level matching. Each account's closest face is its
    // representative distance; accounts are then compared against each other.
    // This keeps two faces of the SAME account from competing and inflating
    // the best/second-best ratio.
    float bestDist = 1e10f, secondBestDist = 1e10f;
    size_t bestIdx = m_users.size();
    uint32_t bestFaceId = 0;
    size_t comparableAccounts = 0;

    for (size_t i = 0; i < m_users.size(); i++) {
        const auto& faces = m_users[i].faces;
        float accountBest = 1e10f;
        uint32_t faceBestId = 0;

        for (const auto& face : faces) {
            // Skip stored embeddings that don't match the probe's dimensionality.
            // dlib (128-D) and InsightFace ONNX (512-D) embeddings live in
            // different metric spaces — comparing them would be meaningless.
            if (face.embedding.size() != probeDim) continue;

            float sum = 0.0f;
            for (size_t j = 0; j < probeDim; j++) {
                float diff = probeEmbedding[j] - face.embedding[j];
                sum += diff * diff;
            }
            float dist = std::sqrt(sum);

            if (dist < accountBest) {
                accountBest = dist;
                faceBestId = face.id;
            }
        }

        if (accountBest >= 1e9f) continue;  // no face with a comparable dimension
        comparableAccounts++;

        if (accountBest < bestDist) {
            secondBestDist = bestDist;
            bestDist = accountBest;
            bestIdx = i;
            bestFaceId = faceBestId;
        } else if (accountBest < secondBestDist) {
            secondBestDist = accountBest;
        }
    }

    // No account with a comparable-dimensionality embedding.
    // (e.g. dlib 128-D probe against an ONNX 512-D enrollment — a config/data
    // mismatch. DEBUG level: fires on every frame and would spam the log.)
    if (bestIdx >= m_users.size()) {
        FACELOGIN_DEBUG(L"FindBestMatch: no stored %zu-D embedding (accounts=%zu)",
                        probeDim, m_users.size());
        return std::nullopt;
    }

    // The base threshold is dlib-calibrated. For 512-D ONNX embeddings,
    // EmbeddingThresholdForDim returns 0.80, measured to separate same-person
    // (0.14–0.80) from other-person photos (0.94–0.99). For 128-D dlib it
    // returns the base unchanged.
    // (DEBUG level: this runs on every frame and would spam the log.)
    float effThreshold = EmbeddingThresholdForDim(threshold, probeDim);
    if (effThreshold != threshold) {
        FACELOGIN_DEBUG(L"FindBestMatch: dim=%zu → threshold %.3f scaled to %.3f",
                        probeDim, threshold, effThreshold);
    }

    // Reject if best match is not meaningfully better than second-best.
    // A ratio >= 0.75 means the probe is ambiguous between two accounts
    // (or between the real user and a noisy impostor).
    // Skip this check when only one account is comparable — there is no
    // second-best to compare against.
    if (comparableAccounts > 1 && secondBestDist < 1e9f) {
        float ratio = bestDist / secondBestDist;
        if (ratio >= 0.75f) {
            FACELOGIN_INFO(L"Match rejected: best/second-best ratio too high (%.3f/%.3f=%.3f)",
                          bestDist, secondBestDist, ratio);
            return std::nullopt;
        }
    }

    if (bestDist < effThreshold) {
        MatchResult best;
        best.distance = bestDist;
        best.matchedFaceId = bestFaceId;
        best.accountFaceCount = m_users[bestIdx].faces.size();
        best.upn = m_users[bestIdx].upn;
        best.sid = m_users[bestIdx].sid;
        best.username = m_users[bestIdx].username;

        if (IsPasswordlessRecord(m_users[bestIdx].encryptedPassword)) {
            // Passwordless account: no password to decrypt. Return the match
            // with passwordless=true so the caller (FaceService) knows not to
            // submit LSA credentials and instead shows a degraded notice.
            best.passwordless = true;
            return best;
        }

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
        // Password-bearing record whose decrypt failed (e.g. DPAPI key
        // lost) — keep the old strict behavior: no match.
        FACELOGIN_WARN(L"FindBestMatch: match found but password decrypt failed for %s",
                       m_users[bestIdx].username.c_str());
    }

    return std::nullopt;
}

} // namespace facelogin
