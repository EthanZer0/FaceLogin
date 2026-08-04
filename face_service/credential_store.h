#pragma once

#include <dlib/matrix.h>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <cmath>

namespace facelogin {

// Map a base "strictness" threshold to the embedding dimensionality actually
// in use.
//
// The system now uses InsightFace ONNX (512-D) embeddings exclusively (the dlib
// recognizer was removed). L2-normalized embeddings have Euclidean distance
// bounded by sqrt(2) ≈ 1.414 regardless of dimension, so sqrt(dim/128) scaling
// is invalid.
//
// For 512-D ONNX we return a fixed 0.80, calibrated from measured data:
//   same-person matches on this system: 0.14–0.80
//   other-person photo match:           0.94–0.99
// (0.80 cleanly separates them; 1.0 admitted a photo.)
// Any other (legacy) dimension falls back to the base threshold.
inline float EmbeddingThresholdForDim(float baseThreshold, size_t dim) {
    if (dim >= 256) return 0.80f;             // ONNX 512-D: measured safe boundary
    return baseThreshold;                     // dlib 128-D and unknown: caller base
}

// Stores and retrieves encrypted user credentials and face embeddings.
//
// File format (PROGRAMDATA/FaceLogin/data/users.dat):
//   Header:
//     Magic:  4 bytes ("FLOG")
//     Version: 4 bytes (uint32, currently 4)
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
//     Face count:      4 bytes (uint32, >= 1)             ← V4
//     Faces (Face count times):                            ← V4
//       Face id:       4 bytes (uint32, >= 1, per-account unique)
//       Label length:  4 bytes (uint32, in wchar_t units, may be 0)
//       Label:         N*2 bytes (UTF-16LE, e.g. L"脸1" or a custom name)
//       Embedding length: 4 bytes (uint32, in floats)
//       Embedding:     D*4 bytes (D floats * 4 bytes)
//
// V1 backward compat: version=1 records omit UPN/SID fields.
// On load, V1 records are auto-upgraded by looking up the SID/UPN from SAM.
// V2 backward compat: version=2 records store a fixed 128-float embedding.
// V3 backward compat: version=3 records store one length-prefixed embedding.
// V1/V2/V3 databases are upgraded IN MEMORY on load: the single embedding is
// wrapped into a one-element faces vector (id=1, label="脸1"). Nothing is
// written back to disk during load; the file is only re-written as V4 when the
// next SaveDatabase() happens (enrollment/deletion). This keeps old versions
// readable for as long as possible (see FaceLoginProvider's version gate).
//
// The file is protected by ACLs (SYSTEM + Administrators only).
// Passwords are encrypted with DPAPI CRYPTPROTECT_LOCAL_MACHINE.

// Maximum faces one account may enroll. Prevents abuse; AddFace rejects when
// the account already has this many faces.
inline constexpr size_t kMaxFacesPerUser = 5;

// Passwordless account: the encryptedPassword field holds a single sentinel
// byte instead of a DPAPI blob. (An empty vector is also treated as
// passwordless, as a defensive fallback.)
inline constexpr uint8_t kPasswordlessSentinelByte = 0x00;
inline bool IsPasswordlessRecord(const std::vector<uint8_t>& encryptedPassword) {
    return encryptedPassword.empty() ||
           (encryptedPassword.size() == 1 &&
            encryptedPassword[0] == kPasswordlessSentinelByte);
}

// One enrolled face for a user account (V4). Each face carries a stable,
// per-account id (never reused after deletion) and a user-given label
// (defaults to L"脸N" where N = id).
struct FaceRecord {
    uint32_t           id = 0;
    std::wstring       label;              // display name; "脸N" if user left blank
    std::vector<float> embedding;          // D-D embedding (128 for dlib, 512 for ONNX)
};

struct UserRecord {
    std::wstring username;
    std::wstring upn;      // UserPrincipalName (e.g. "john@outlook.com"), V2
    std::wstring sid;      // Security Identifier (e.g. "S-1-5-21-..."), V2
    std::vector<uint8_t> encryptedPassword;  // DPAPI encrypted (or passwordless sentinel)
    std::vector<FaceRecord> faces;           // one or more enrolled faces (V4)
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

    // Find the index of the record matching the given identity.
    // Match priority: SID > UPN > username (only non-empty candidates are
    // tried). Returns m_users.size() (i.e. "not found") when nothing matches.
    size_t FindUserIndex(const std::wstring& sid,
                         const std::wstring& upn = L"",
                         const std::wstring& username = L"") const;

    // Add a face to a user account (create-or-append):
    //   - Account not found: creates it with the given encrypted password and
    //     the first face (id = 1).
    //   - Account found: appends a new face (id = max(existing)+1) WITHOUT
    //     touching existing faces or the stored password. The passed
    //     encryptedPassword is ignored in this case.
    // Rejects (returns false) when the account already holds
    // kMaxFacesPerUser faces.
    // Call SaveDatabase() to persist.
    bool AddFace(const std::wstring& username,
                 const std::wstring& upn,
                 const std::wstring& sid,
                 const std::vector<uint8_t>& encryptedPassword,
                 const std::vector<float>& embedding,
                 const std::wstring& label = L"",
                 uint32_t* outFaceId = nullptr);

    // Add a user to the in-memory database (first-time full enrollment entry
    // point). Same semantics as AddFace for the "account not found" case;
    // kept for compatibility with existing call sites.
    bool AddUser(const std::wstring& username,
                 const std::wstring& upn,
                 const std::wstring& sid,
                 const std::vector<uint8_t>& encryptedPassword,
                 const std::vector<float>& embedding);

    // Update the identity + stored password of an existing account IN PLACE,
    // preserving all enrolled faces (their ids/labels/embeddings are untouched).
    // Used when the account switches from a Microsoft (MSA) to a local account:
    // clears the stale MSA UPN (pass an empty upn) and refreshes username/SID.
    // Returns false when idx is out of range.
    // Call SaveDatabase() to persist.
    bool UpdateAccountIdentity(size_t idx,
                               const std::wstring& username,
                               const std::wstring& upn,
                               const std::wstring& sid,
                               const std::vector<uint8_t>& encryptedPassword);

    // Delete one face of an account. If the account ends up with no faces,
    // the whole account record is removed (an account with zero faces must
    // never be persisted — the login tile reads the record count and would
    // show a tile that can never match).
    // Call SaveDatabase() to persist.
    bool DeleteFace(const std::wstring& sid, uint32_t faceId);

    // Remove an account entirely (equivalent to deleting all of its faces).
    // Call SaveDatabase() to persist.
    bool ClearAllFaces(const std::wstring& sid);

    // Remove an account by SID. Call SaveDatabase() to persist.
    bool DeleteUserBySid(const std::wstring& sid);

    // Delete a user from the in-memory database (by username).
    // Call SaveDatabase() to persist.
    bool DeleteUser(const std::wstring& username);

    // Rename one face of an account (e.g. via the face management UI).
    // Returns false if the account or face id is unknown.
    // Call SaveDatabase() to persist.
    bool RenameFace(const std::wstring& sid, uint32_t faceId,
                    const std::wstring& label);

    // Number of faces enrolled for an account (0 = not enrolled).
    size_t GetFaceCount(const std::wstring& sid) const;

    // Find the best matching user for a probe embedding.
    // Matching is account-level: each account's closest face is its
    // representative distance, then accounts are compared against each other
    // (so two faces of the same account never compete and inflate the
    // best/second-best ratio). Returns the UserRecord and the user's decrypted
    // password if:
    //  1. distance < threshold, AND
    //  2. best distance / second-best distance < 0.75 (single account case: always passes)
    // Returns std::nullopt if no match found.
    struct MatchResult {
        std::wstring username;
        std::wstring upn;
        std::wstring sid;
        std::wstring password;  // Decrypted — zero after use!
        bool         passwordless = false;  // true: no password stored, must NOT submit LSA creds
        float distance;
        uint32_t     matchedFaceId = 0;     // V4: id of the closest face in the matched account
        size_t       accountFaceCount = 0;  // V4: total faces of the matched account
    };
    // probeDim is the number of floats in probeEmbedding (128 for dlib,
    // 512 for InsightFace ONNX). Only stored embeddings of the same
    // dimensionality are compared; others are skipped as non-comparable.
    std::optional<MatchResult> FindBestMatch(const float probeEmbedding[],
                                              size_t probeDim,
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
