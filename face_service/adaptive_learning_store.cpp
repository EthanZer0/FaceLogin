#include "adaptive_learning_store.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace facelogin {
namespace {

constexpr char kMagic[] = {'F', 'L', 'A', 'D'};
constexpr uint32_t kVersion = 1;
constexpr float kDuplicateDistance = 0.08f;
constexpr float kClusterDistance = 0.42f;

template <typename T>
bool ReadValue(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(in);
}

template <typename T>
void WriteValue(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool ReadString(std::ifstream& in, std::wstring& value) {
    uint32_t length = 0;
    if (!ReadValue(in, length) || length > 4096) return false;
    value.assign(length, L'\0');
    if (length != 0) {
        in.read(reinterpret_cast<char*>(value.data()),
                static_cast<std::streamsize>(length * sizeof(wchar_t)));
    }
    return static_cast<bool>(in);
}

void WriteString(std::ofstream& out, const std::wstring& value) {
    const uint32_t length = static_cast<uint32_t>(value.size());
    WriteValue(out, length);
    if (length != 0) {
        out.write(reinterpret_cast<const char*>(value.data()),
                  static_cast<std::streamsize>(length * sizeof(wchar_t)));
    }
}

bool ReadEmbedding(std::ifstream& in, std::vector<float>& embedding) {
    uint32_t length = 0;
    if (!ReadValue(in, length) || length == 0 || length > 1024) return false;
    embedding.assign(length, 0.0f);
    in.read(reinterpret_cast<char*>(embedding.data()),
            static_cast<std::streamsize>(length * sizeof(float)));
    return static_cast<bool>(in);
}

void WriteEmbedding(std::ofstream& out, const std::vector<float>& embedding) {
    const uint32_t length = static_cast<uint32_t>(embedding.size());
    WriteValue(out, length);
    if (length != 0) {
        out.write(reinterpret_cast<const char*>(embedding.data()),
                  static_cast<std::streamsize>(length * sizeof(float)));
    }
}

float Distance(const float a[], const float b[], size_t count) {
    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return std::sqrt(sum);
}

std::vector<float> MeanEmbedding(const std::vector<const std::vector<float>*>& values) {
    if (values.empty() || values.front()->empty()) return {};
    std::vector<float> mean(values.front()->size(), 0.0f);
    for (const auto* value : values) {
        if (!value || value->size() != mean.size()) return {};
        for (size_t i = 0; i < mean.size(); ++i) mean[i] += (*value)[i];
    }
    for (float& value : mean) value /= static_cast<float>(values.size());

    float norm = 0.0f;
    for (const float value : mean) norm += value * value;
    norm = std::sqrt(norm);
    if (norm <= std::numeric_limits<float>::epsilon()) return {};
    for (float& value : mean) value /= norm;
    return mean;
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), bytes, nullptr, nullptr);
    return result;
}

} // namespace

std::wstring AdaptiveLearningStore::StorePath() const {
    return m_dataDir + L"\\data\\adaptive\\profiles.dat";
}

std::wstring AdaptiveLearningStore::SamplesDir() const {
    return m_dataDir + L"\\data\\adaptive\\samples";
}

bool AdaptiveLearningStore::EnsureDirectories() const {
    if (m_dataDir.empty()) return false;
    const std::wstring dataDir = m_dataDir + L"\\data";
    const std::wstring adaptiveDir = dataDir + L"\\adaptive";
    const auto ensureDirectory = [](const std::wstring& path) {
        if (CreateDirectoryW(path.c_str(), nullptr)) return true;
        return GetLastError() == ERROR_ALREADY_EXISTS;
    };
    return ensureDirectory(dataDir) && ensureDirectory(adaptiveDir) &&
           ensureDirectory(SamplesDir());
}

bool AdaptiveLearningStore::Load() {
    if (m_loaded) return true;
    m_archives.clear();
    if (!EnsureDirectories()) return false;

    std::ifstream in(StorePath(), std::ios::binary);
    if (!in) {
        m_loaded = true;
        return true;
    }

    char magic[sizeof(kMagic)] = {};
    uint32_t version = 0;
    uint32_t archiveCount = 0;
    in.read(magic, sizeof(magic));
    if (!in || !std::equal(std::begin(magic), std::end(magic), std::begin(kMagic)) ||
        !ReadValue(in, version) || version != kVersion ||
        !ReadValue(in, archiveCount) || archiveCount > 1024) {
        return false;
    }

    m_archives.reserve(archiveCount);
    for (uint32_t i = 0; i < archiveCount; ++i) {
        AdaptiveLearningArchive archive;
        uint32_t enabled = 0;
        uint32_t sampleCount = 0;
        uint32_t prototypeCount = 0;
        if (!ReadString(in, archive.sid) || archive.sid.empty() ||
            !ReadValue(in, archive.faceId) || archive.faceId == 0 ||
            !ReadValue(in, enabled) || !ReadValue(in, archive.builtSampleCount) ||
            !ReadValue(in, sampleCount) || sampleCount > kMaxAdaptiveSamplesPerFace ||
            !ReadValue(in, prototypeCount) || prototypeCount > kMaxAdaptivePrototypesPerFace) {
            return false;
        }
        archive.enabled = enabled != 0;
        archive.samples.reserve(sampleCount);
        for (uint32_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
            AdaptiveLearningSample sample;
            if (!ReadValue(in, sample.id) || sample.id == 0 ||
                !ReadValue(in, sample.addedAt) || !ReadString(in, sample.file) ||
                sample.file.empty() || !ReadEmbedding(in, sample.embedding)) {
                return false;
            }
            archive.samples.push_back(std::move(sample));
        }
        archive.prototypes.reserve(prototypeCount);
        for (uint32_t prototypeIndex = 0; prototypeIndex < prototypeCount; ++prototypeIndex) {
            std::vector<float> prototype;
            if (!ReadEmbedding(in, prototype)) return false;
            archive.prototypes.push_back(std::move(prototype));
        }
        archive.builtSampleCount = std::min<uint32_t>(archive.builtSampleCount,
                                                       static_cast<uint32_t>(archive.samples.size()));
        m_archives.push_back(std::move(archive));
    }
    m_loaded = true;
    return true;
}

bool AdaptiveLearningStore::Reload() {
    m_loaded = false;
    return Load();
}

bool AdaptiveLearningStore::Save() const {
    if (!EnsureDirectories()) return false;
    const std::wstring path = StorePath();
    const std::wstring temp = path + L".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    out.write(kMagic, sizeof(kMagic));
    WriteValue(out, kVersion);
    const uint32_t archiveCount = static_cast<uint32_t>(m_archives.size());
    WriteValue(out, archiveCount);
    for (const auto& archive : m_archives) {
        WriteString(out, archive.sid);
        WriteValue(out, archive.faceId);
        const uint32_t enabled = archive.enabled ? 1u : 0u;
        WriteValue(out, enabled);
        WriteValue(out, archive.builtSampleCount);
        const uint32_t sampleCount = static_cast<uint32_t>(archive.samples.size());
        const uint32_t prototypeCount = static_cast<uint32_t>(archive.prototypes.size());
        WriteValue(out, sampleCount);
        WriteValue(out, prototypeCount);
        for (const auto& sample : archive.samples) {
            WriteValue(out, sample.id);
            WriteValue(out, sample.addedAt);
            WriteString(out, sample.file);
            WriteEmbedding(out, sample.embedding);
        }
        for (const auto& prototype : archive.prototypes) WriteEmbedding(out, prototype);
    }
    out.close();
    if (!out) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return MoveFileExW(temp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

const AdaptiveLearningArchive* AdaptiveLearningStore::FindArchive(
    const std::wstring& sid, uint32_t faceId) const {
    const auto it = std::find_if(m_archives.begin(), m_archives.end(),
        [&sid, faceId](const AdaptiveLearningArchive& archive) {
            return archive.faceId == faceId &&
                CompareStringOrdinal(archive.sid.c_str(), -1, sid.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
    return it == m_archives.end() ? nullptr : &*it;
}

AdaptiveLearningArchive* AdaptiveLearningStore::FindArchiveMutable(
    const std::wstring& sid, uint32_t faceId) {
    return const_cast<AdaptiveLearningArchive*>(
        static_cast<const AdaptiveLearningStore*>(this)->FindArchive(sid, faceId));
}

bool AdaptiveLearningStore::AddSample(const std::wstring& sid, uint32_t faceId,
                                      const std::wstring& file,
                                      const std::vector<float>& embedding,
                                      uint64_t addedAt) {
    if (!Load() || sid.empty() || faceId == 0 || file.empty() || embedding.empty() ||
        file.find_first_of(L"\\/:*") != std::wstring::npos) {
        return false;
    }
    AdaptiveLearningArchive* archive = FindArchiveMutable(sid, faceId);
    bool createdArchive = false;
    if (!archive) {
        AdaptiveLearningArchive created;
        created.sid = sid;
        created.faceId = faceId;
        m_archives.push_back(std::move(created));
        archive = &m_archives.back();
        createdArchive = true;
    }
    if (archive->samples.size() >= kMaxAdaptiveSamplesPerFace) return false;
    for (const auto& existing : archive->samples) {
        if (existing.embedding.size() == embedding.size() &&
            Distance(existing.embedding.data(), embedding.data(), embedding.size()) < kDuplicateDistance) {
            return false;
        }
    }
    uint64_t nextId = 1;
    for (const auto& existing : archive->samples) nextId = std::max(nextId, existing.id + 1);
    AdaptiveLearningSample sample;
    sample.id = nextId;
    sample.addedAt = addedAt;
    sample.file = file;
    sample.embedding = embedding;
    archive->samples.push_back(std::move(sample));
    if (Save()) return true;

    archive->samples.pop_back();
    if (createdArchive && archive->samples.empty()) m_archives.pop_back();
    return false;
}

bool AdaptiveLearningStore::RebuildArchive(const std::wstring& sid, uint32_t faceId) {
    if (!Load()) return false;
    AdaptiveLearningArchive* archive = FindArchiveMutable(sid, faceId);
    if (!archive || archive->samples.size() < kMinAdaptiveSamplesToBuild) return false;

    struct Cluster { std::vector<const std::vector<float>*> values; std::vector<float> center; };
    std::vector<Cluster> clusters;
    for (const auto& sample : archive->samples) {
        if (sample.embedding.empty()) continue;
        size_t bestIndex = clusters.size();
        float bestDistance = std::numeric_limits<float>::max();
        for (size_t i = 0; i < clusters.size(); ++i) {
            if (clusters[i].center.size() != sample.embedding.size()) continue;
            const float distance = Distance(clusters[i].center.data(), sample.embedding.data(),
                                            sample.embedding.size());
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = i;
            }
        }
        if (bestIndex == clusters.size() && clusters.size() < kMaxAdaptivePrototypesPerFace) {
            Cluster cluster;
            cluster.values.push_back(&sample.embedding);
            cluster.center = sample.embedding;
            clusters.push_back(std::move(cluster));
        } else if (bestIndex < clusters.size() && bestDistance <= kClusterDistance) {
            auto& cluster = clusters[bestIndex];
            cluster.values.push_back(&sample.embedding);
            cluster.center = MeanEmbedding(cluster.values);
        }
        // A very distant sample is intentionally not forced into a profile.
    }

    std::vector<std::vector<float>> prototypes;
    for (const auto& cluster : clusters) {
        if (cluster.values.size() < 3) continue;
        const auto prototype = MeanEmbedding(cluster.values);
        if (!prototype.empty()) prototypes.push_back(prototype);
    }
    if (prototypes.empty()) return false;

    const auto previousPrototypes = archive->prototypes;
    const uint32_t previousBuiltSampleCount = archive->builtSampleCount;
    const bool previousEnabled = archive->enabled;
    archive->prototypes = std::move(prototypes);
    archive->builtSampleCount = static_cast<uint32_t>(archive->samples.size());
    archive->enabled = true;
    if (Save()) return true;

    archive->prototypes = previousPrototypes;
    archive->builtSampleCount = previousBuiltSampleCount;
    archive->enabled = previousEnabled;
    return false;
}

bool AdaptiveLearningStore::SetArchiveEnabled(const std::wstring& sid, uint32_t faceId,
                                              bool enabled) {
    if (!Load()) return false;
    AdaptiveLearningArchive* archive = FindArchiveMutable(sid, faceId);
    if (!archive || archive->prototypes.empty()) return false;
    archive->enabled = enabled;
    return Save();
}

void AdaptiveLearningStore::DeleteArchiveFiles(const AdaptiveLearningArchive& archive) const {
    for (const auto& sample : archive.samples) {
        DeleteFileW((SamplesDir() + L"\\" + sample.file).c_str());
    }
}

bool AdaptiveLearningStore::DeleteArchive(const std::wstring& sid, uint32_t faceId) {
    if (!Load()) return false;
    const auto it = std::find_if(m_archives.begin(), m_archives.end(),
        [&sid, faceId](const AdaptiveLearningArchive& archive) {
            return archive.faceId == faceId &&
                CompareStringOrdinal(archive.sid.c_str(), -1, sid.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
    if (it == m_archives.end()) return false;
    DeleteArchiveFiles(*it);
    m_archives.erase(it);
    return Save();
}

bool AdaptiveLearningStore::DeleteAllForSid(const std::wstring& sid) {
    if (!Load()) return false;
    bool removed = false;
    auto it = m_archives.begin();
    while (it != m_archives.end()) {
        if (CompareStringOrdinal(it->sid.c_str(), -1, sid.c_str(), -1, TRUE) == CSTR_EQUAL) {
            DeleteArchiveFiles(*it);
            it = m_archives.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    return !removed || Save();
}

std::optional<float> AdaptiveLearningStore::FindBestDistance(
    const std::wstring& sid, uint32_t faceId, const float probe[], size_t probeDim) const {
    if (!m_loaded || !probe || probeDim == 0) return std::nullopt;
    const AdaptiveLearningArchive* archive = FindArchive(sid, faceId);
    if (!archive || !archive->enabled || archive->prototypes.empty()) return std::nullopt;
    float best = std::numeric_limits<float>::max();
    for (const auto& prototype : archive->prototypes) {
        if (prototype.size() != probeDim) continue;
        best = std::min(best, Distance(prototype.data(), probe, probeDim));
    }
    return best == std::numeric_limits<float>::max() ? std::nullopt
                                                       : std::optional<float>(best);
}

std::string AdaptiveLearningStore::GetArchivesJson(const std::wstring& sid) const {
    std::ostringstream out;
    out << "[";
    bool firstArchive = true;
    for (const auto& archive : m_archives) {
        if (CompareStringOrdinal(archive.sid.c_str(), -1, sid.c_str(), -1, TRUE) != CSTR_EQUAL) continue;
        if (!firstArchive) out << ",";
        firstArchive = false;
        out << "{\"faceId\":" << archive.faceId
            << ",\"enabled\":" << (archive.enabled ? "true" : "false")
            << ",\"sampleCount\":" << archive.samples.size()
            << ",\"builtSampleCount\":" << archive.builtSampleCount
            << ",\"prototypeCount\":" << archive.prototypes.size()
            << ",\"samples\":[";
        for (size_t i = 0; i < archive.samples.size(); ++i) {
            if (i != 0) out << ",";
            const auto& sample = archive.samples[i];
            out << "{\"id\":" << sample.id
                << ",\"file\":\"" << Utf8(sample.file) << "\""
                << ",\"addedAt\":" << sample.addedAt << "}";
        }
        out << "]}";
    }
    out << "]";
    return out.str();
}

} // namespace facelogin
