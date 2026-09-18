#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace facelogin {

// Adaptive learning is intentionally separate from users.dat.  A regular
// FaceRecord remains the user's explicit enrollment and keeps the account's
// ten-face limit; an archive is supplementary evidence tied to one FaceRecord.
inline constexpr size_t kMaxAdaptiveSamplesPerFace = 100;
inline constexpr size_t kMaxAdaptivePrototypesPerFace = 3;
inline constexpr size_t kMinAdaptiveSamplesToBuild = 5;

struct AdaptiveLearningSample {
    uint64_t id = 0;
    uint64_t addedAt = 0;  // FILETIME ticks, used only for ordering in Console.
    std::wstring file;     // basename in data\\adaptive\\samples
    std::vector<float> embedding;
};

struct AdaptiveLearningGroup {
    uint64_t representativeSampleId = 0;
    std::vector<uint64_t> sampleIds;
    std::vector<float> embedding;
};

struct AdaptiveLearningArchive {
    std::wstring sid;
    uint32_t faceId = 0;
    bool enabled = true;
    std::vector<AdaptiveLearningSample> samples;
    // Hidden implementation detail: several compact representatives can cover
    // distinct, user-confirmed conditions without comparing every raw sample.
    // Legacy features have no known membership until the next successful build.
    std::vector<AdaptiveLearningGroup> groups;
    uint64_t evaluatedThroughSampleId = 0;
    size_t UsedSampleCount() const;
};

enum class AdaptiveBuildStatus { Success, NotEnoughSamples, NoConsistentGroup,
                                 ArchiveNotFound, SaveFailed };
struct AdaptiveBuildResult {
    AdaptiveBuildStatus status = AdaptiveBuildStatus::ArchiveNotFound;
    size_t totalSamples = 0;
    size_t usedSamples = 0;
    size_t groupCount = 0;
    std::string ToJson() const;
};

class AdaptiveLearningStore {
public:
    void SetDataDir(const std::wstring& dir) { m_dataDir = dir; }
    bool Load();
    bool Reload();
    bool Save() const;

    const AdaptiveLearningArchive* FindArchive(const std::wstring& sid,
                                                uint32_t faceId) const;
    std::string GetArchivesJson(const std::wstring& sid) const;

    // Adds a user-confirmed sample to the draft archive.  The caller owns image
    // validation/copying and must pass only a safe archive-local basename.
    bool AddSample(const std::wstring& sid, uint32_t faceId,
                   const std::wstring& file,
                   const std::vector<float>& embedding,
                   uint64_t addedAt);
    AdaptiveBuildResult RebuildArchive(const std::wstring& sid, uint32_t faceId);
    bool SetArchiveEnabled(const std::wstring& sid, uint32_t faceId, bool enabled);
    bool DeleteArchive(const std::wstring& sid, uint32_t faceId);
    bool DeleteAllForSid(const std::wstring& sid);

    // Returns the nearest active representative for one base face.  It never
    // searches another account or another base face.
    std::optional<float> FindBestDistance(const std::wstring& sid, uint32_t faceId,
                                          const float probe[], size_t probeDim) const;

private:
    bool EnsureDirectories() const;
    std::wstring StorePath() const;
    std::wstring SamplesDir() const;
    AdaptiveLearningArchive* FindArchiveMutable(const std::wstring& sid,
                                                 uint32_t faceId);
    void DeleteArchiveFiles(const AdaptiveLearningArchive& archive) const;

    std::wstring m_dataDir;
    std::vector<AdaptiveLearningArchive> m_archives;
    bool m_loaded = false;
};

} // namespace facelogin
