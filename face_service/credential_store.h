#pragma once

#include <dlib/matrix.h>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace facelogin {

// Stores and retrieves encrypted user credentials and face embeddings.
//
// File format (PROGRAMDATA/FaceLogin/data/users.dat):
//   Header:
//     Magic:  4 bytes ("FLOG")
//     Version: 4 bytes (uint32, currently 2)
//     Count:   4 bytes (uint32, number of records)
//   Records (Count times):
//     Username length: 4 bytes (uint32, in wchar_t units)
//     Username:        N*2 bytes (UTF-16LE)
//     UPN length:      4 bytes (uint32, in wchar_t units) ← V2
//     UPN:             N*2 bytes (UTF-16LE)               ← V2
//     SID length:      4 bytes (uint32, in wchar_t units) ← V2
//     SID:             N*2 bytes (UTF-16LE)               ← V2
//     Password length: 4 bytes (uint32, in bytes, encrypted)
//     Password:        N bytes (DPAPI encrypted)
//     Embedding:       512 bytes (128 floats * 4 bytes)
//
// V1 backward compat: version=1 records omit UPN/SID fields.
// On load, V1 records are auto-upgraded by looking up the SID/UPN from SAM.
//
// The file is protected by ACLs (SYSTEM + Administrators only).
// Passwords are encrypted with DPAPI CRYPTPROTECT_LOCAL_MACHINE.

struct UserRecord {
    std::wstring username;
    std::wstring upn;      // UserPrincipalName (e.g. "john@outlook.com"), V2
    std::wstring sid;      // Security Identifier (e.g. "S-1-5-21-..."), V2
    std::vector<uint8_t> encryptedPassword;  // DPAPI encrypted
    float embedding[128];                     // 128-D face embedding
};

class CredentialStore {
public:
    CredentialStore() = default;
    ~CredentialStore() = default;

    // Set the data directory path. Default: PROGRAMDATA/FaceLogin/
    void SetDataDir(const std::wstring& dir) { m_dataDir = dir; }

    // Load users.dat from disk. Returns true on success.
    bool LoadDatabase();

    // Save current in-memory records to users.dat. Returns true on success.
    bool SaveDatabase();

    // Get all loaded user records
    const std::vector<UserRecord>& GetUsers() const { return m_users; }

    // Add a user to the in-memory database.
    // Call SaveDatabase() to persist.
    bool AddUser(const std::wstring& username,
                 const std::wstring& upn,
                 const std::wstring& sid,
                 const std::vector<uint8_t>& encryptedPassword,
                 const float embedding[128]);

    // Delete a user from the in-memory database.
    // Call SaveDatabase() to persist.
    bool DeleteUser(const std::wstring& username);

    // Find the best matching user for a probe embedding.
    // Returns the UserRecord and the user's decrypted password if:
    //  1. distance < threshold, AND
    //  2. best distance / second-best distance < 0.75 (single user case: always passes)
    // Returns std::nullopt if no match found.
    struct MatchResult {
        std::wstring username;
        std::wstring upn;
        std::wstring sid;
        std::wstring password;  // Decrypted — zero after use!
        float distance;
    };
    std::optional<MatchResult> FindBestMatch(const float probeEmbedding[128],
                                              float threshold = 0.30f);

    // Get the number of registered users
    size_t GetUserCount() const { return m_users.size(); }

    // Get the full path to the database file
    std::wstring GetDataDir() const;

private:
    bool EnsureDataDir();

    std::wstring m_dataDir;  // If empty, uses default
    std::vector<UserRecord> m_users;
};

} // namespace facelogin
