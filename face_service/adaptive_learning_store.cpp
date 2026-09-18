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
constexpr uint32_t kVersion = 2;
constexpr float kDuplicateDistance = 0.08f;
constexpr float kMaxDistanceToRepresentative = 0.70f;
constexpr float kMaxPairDistance = 0.70f;

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

size_t AdaptiveLearningArchive::UsedSampleCount() const {
    size_t count = 0;
    for (const auto& group : groups) count += group.sampleIds.size();
    return count;
}

std::string AdaptiveBuildResult::ToJson() const {
    const char* names[] = {"success", "notEnoughSamples", "noConsistentGroup",
                           "archiveNotFound", "saveFailed"};
    std::ostringstream out;
    out << "{\"status\":\"" << names[static_cast<size_t>(status)]
        << "\",\"totalSamples\":" << totalSamples
        << ",\"usedSamples\":" << usedSamples
        << ",\"unusedSamples\":" << totalSamples - usedSamples
        << ",\"groupCount\":" << groupCount << "}";
    return out.str();
}

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
        !ReadValue(in, version) || (version != 1 && version != kVersion) ||
        !ReadValue(in, archiveCount) || archiveCount > 1024) {
        return false;
    }

    m_archives.reserve(archiveCount);
    for (uint32_t i = 0; i < archiveCount; ++i) {
        AdaptiveLearningArchive archive;
        uint32_t enabled = 0;
        uint32_t sampleCount = 0;
        uint32_t prototypeCount = 0;
        uint32_t legacyBuiltCount = 0;
        if (!ReadString(in, archive.sid) || archive.sid.empty() ||
            !ReadValue(in, archive.faceId) || archive.faceId == 0 ||
            !ReadValue(in, enabled) ||
            !(version == 1 ? ReadValue(in, legacyBuiltCount)
                           : ReadValue(in, archive.evaluatedThroughSampleId)) ||
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
        archive.groups.reserve(prototypeCount);
        for (uint32_t prototypeIndex = 0; prototypeIndex < prototypeCount; ++prototypeIndex) {
            AdaptiveLearningGroup group;
            if (!ReadEmbedding(in, group.embedding)) return false;
            if (version >= 2) {
                uint32_t count = 0;
                if (!ReadValue(in, group.representativeSampleId) ||
                    !ReadValue(in, count) || count > sampleCount) return false;
                for (uint32_t j = 0; j < count; ++j) {
                    uint64_t id = 0;
                    if (!ReadValue(in, id) ||
                        std::none_of(archive.samples.begin(), archive.samples.end(),
                            [id](const AdaptiveLearningSample& sample) { return sample.id == id; }))
                        return false;
                    group.sampleIds.push_back(id);
                }
                if (count != 0 && std::find(group.sampleIds.begin(), group.sampleIds.end(),
                    group.representativeSampleId) == group.sampleIds.end()) return false;
            }
            archive.groups.push_back(std::move(group));
        }
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
        WriteValue(out, archive.evaluatedThroughSampleId);
        const uint32_t sampleCount = static_cast<uint32_t>(archive.samples.size());
        const uint32_t prototypeCount = static_cast<uint32_t>(archive.groups.size());
        WriteValue(out, sampleCount);
        WriteValue(out, prototypeCount);
        for (const auto& sample : archive.samples) {
            WriteValue(out, sample.id);
            WriteValue(out, sample.addedAt);
            WriteString(out, sample.file);
            WriteEmbedding(out, sample.embedding);
        }
        for (const auto& group : archive.groups) {
            WriteEmbedding(out, group.embedding);
            WriteValue(out, group.representativeSampleId);
            const auto& members = group.sampleIds;
            WriteValue(out, static_cast<uint32_t>(members.size()));
            for (uint64_t id : members) WriteValue(out, id);
        }
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

namespace {
// Build and presentation share the same analysis. Qualified groups are selected
// first; the remainder is partitioned into small groups and isolated samples.
std::vector<AdaptiveLearningGroup> AnalyzeSamples(const std::vector<AdaptiveLearningSample>& samples) {
    const size_t n = samples.size();
    std::vector<std::vector<float>> distances(n, std::vector<float>(n, 0));
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            const float distance = samples[i].embedding.size() == samples[j].embedding.size()
                ? Distance(samples[i].embedding.data(), samples[j].embedding.data(),
                           samples[i].embedding.size())
                : std::numeric_limits<float>::infinity();
            distances[i][j] = distances[j][i] = distance;
        }
    }
    struct Candidate { std::vector<size_t> members; size_t representative = 0; float average = 0; };
    const auto medoid = [&](const std::vector<size_t>& members) {
        size_t best = members.front();
        float bestSum = std::numeric_limits<float>::infinity();
        for (size_t i : members) {
            float sum = 0;
            for (size_t j : members) sum += distances[i][j];
            if (sum < bestSum || (sum == bestSum && samples[i].id < samples[best].id)) {
                best = i; bestSum = sum;
            }
        }
        return best;
    };
    std::vector<bool> available(n, true);
    std::vector<AdaptiveLearningGroup> groups;
    size_t minimum = kMinAdaptiveSamplesToBuild;
    while (true) {
        Candidate best;
        for (size_t seed = 0; seed < n; ++seed) {
            if (!available[seed]) continue;
            Candidate candidate;
            for (size_t i = 0; i < n; ++i)
                if (available[i] && distances[seed][i] <= kMaxDistanceToRepresentative)
                    candidate.members.push_back(i);
            while (candidate.members.size() >= minimum) {
                candidate.representative = medoid(candidate.members);
                size_t worst = candidate.members.front();
                float worstViolation = 1.0f;
                for (size_t i : candidate.members) {
                    float violation = distances[i][candidate.representative] / kMaxDistanceToRepresentative;
                    for (size_t j : candidate.members)
                        violation = std::max(violation, distances[i][j] / kMaxPairDistance);
                    if (violation > worstViolation ||
                        (violation == worstViolation && violation > 1 &&
                         samples[i].id > samples[worst].id)) {
                        worst = i; worstViolation = violation;
                    }
                }
                if (worstViolation <= 1) break;
                candidate.members.erase(std::find(candidate.members.begin(), candidate.members.end(), worst));
            }
            if (candidate.members.size() < minimum) continue;
            float sum = 0;
            for (size_t i : candidate.members)
                for (size_t j : candidate.members) sum += distances[i][j];
            candidate.average = sum / static_cast<float>(candidate.members.size() * candidate.members.size());
            if (candidate.members.size() > best.members.size() ||
                (candidate.members.size() == best.members.size() &&
                 (candidate.average < best.average ||
                  (candidate.average == best.average &&
                   samples[candidate.representative].id < samples[best.representative].id))))
                best = std::move(candidate);
        }
        if (best.members.empty()) {
            if (minimum != 1) { minimum = 1; continue; }
            break;
        }
        AdaptiveLearningGroup group;
        group.representativeSampleId = samples[best.representative].id;
        group.embedding = samples[best.representative].embedding;
        for (size_t i : best.members) { available[i] = false; group.sampleIds.push_back(samples[i].id); }
        std::sort(group.sampleIds.begin(), group.sampleIds.end());
        groups.push_back(std::move(group));
    }
    return groups;
}
} // namespace

AdaptiveBuildResult AdaptiveLearningStore::RebuildArchive(const std::wstring& sid, uint32_t faceId) {
    AdaptiveBuildResult result;
    if (!Load()) { result.status = AdaptiveBuildStatus::SaveFailed; return result; }
    AdaptiveLearningArchive* archive = FindArchiveMutable(sid, faceId);
    if (!archive) return result;
    const auto& samples = archive->samples;
    result.totalSamples = samples.size();
    if (samples.size() < kMinAdaptiveSamplesToBuild) {
        result.status = AdaptiveBuildStatus::NotEnoughSamples;
        return result;
    }
    AdaptiveLearningArchive next = *archive;
    next.groups.clear();
    for (auto& group : AnalyzeSamples(samples)) {
        if (group.sampleIds.size() < kMinAdaptiveSamplesToBuild) continue;
        next.groups.push_back(std::move(group));
        if (next.groups.size() == kMaxAdaptivePrototypesPerFace) break;
    }
    if (next.groups.empty()) {
        result.status = AdaptiveBuildStatus::NoConsistentGroup;
        return result;
    }
    next.evaluatedThroughSampleId = 0;
    for (const auto& sample : samples)
        next.evaluatedThroughSampleId = std::max(next.evaluatedThroughSampleId, sample.id);
    next.enabled = true;
    AdaptiveLearningArchive previous = std::move(*archive);
    *archive = std::move(next);
    if (!Save()) {
        *archive = std::move(previous);
        result.status = AdaptiveBuildStatus::SaveFailed;
        return result;
    }
    result.status = AdaptiveBuildStatus::Success;
    result.usedSamples = archive->UsedSampleCount();
    result.groupCount = archive->groups.size();
    return result;
}

bool AdaptiveLearningStore::SetArchiveEnabled(const std::wstring& sid, uint32_t faceId,
                                              bool enabled) {
    if (!Load()) return false;
    AdaptiveLearningArchive* archive = FindArchiveMutable(sid, faceId);
    if (!archive || archive->groups.empty()) return false;
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
    if (!archive || !archive->enabled || archive->groups.empty()) return std::nullopt;
    float best = std::numeric_limits<float>::max();
    for (const auto& group : archive->groups) {
        const auto& prototype = group.embedding;
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
            << ",\"usedSampleCount\":" << archive.UsedSampleCount()
            << ",\"prototypeCount\":" << archive.groups.size()
            << ",\"samples\":[";
        for (size_t i = 0; i < archive.samples.size(); ++i) {
            if (i != 0) out << ",";
            const auto& sample = archive.samples[i];
            const bool used = std::any_of(archive.groups.begin(), archive.groups.end(),
                [&sample](const AdaptiveLearningGroup& group) {
                    return std::find(group.sampleIds.begin(), group.sampleIds.end(), sample.id) != group.sampleIds.end();
                });
            out << "{\"id\":" << sample.id
                << ",\"file\":\"" << Utf8(sample.file) << "\""
                << ",\"addedAt\":" << sample.addedAt
                << ",\"state\":\"" << (used ? "used" :
                    sample.id <= archive.evaluatedThroughSampleId ? "unused" : "pending") << "\"}";
        }
        out << "],\"analysis\":{\"minimumSamples\":" << kMinAdaptiveSamplesToBuild
            << ",\"representativeLimit\":" << kMaxDistanceToRepresentative
            << ",\"pairLimit\":" << kMaxPairDistance << ",\"groups\":[";
        const auto analyzed = AnalyzeSamples(archive.samples);
        size_t selectedCount = 0;
        for (size_t i = 0; i < analyzed.size(); ++i) {
            if (i != 0) out << ",";
            const auto& group = analyzed[i];
            float maxPair = 0;
            std::vector<const AdaptiveLearningSample*> members;
            for (uint64_t id : group.sampleIds) {
                const auto sample = std::find_if(archive.samples.begin(), archive.samples.end(),
                    [id](const AdaptiveLearningSample& item) { return item.id == id; });
                members.push_back(&*sample);
            }
            for (size_t a = 0; a < members.size(); ++a)
                for (size_t b = a + 1; b < members.size(); ++b)
                    maxPair = std::max(maxPair, Distance(members[a]->embedding.data(),
                        members[b]->embedding.data(), group.embedding.size()));
            const bool selected = members.size() >= kMinAdaptiveSamplesToBuild &&
                                  selectedCount < kMaxAdaptivePrototypesPerFace;
            if (selected) ++selectedCount;
            out << "{\"representativeSampleId\":" << group.representativeSampleId
                << ",\"maxPairDistance\":" << maxPair
                << ",\"selected\":" << (selected ? "true" : "false")
                << ",\"members\":[";
            for (size_t j = 0; j < members.size(); ++j) {
                if (j != 0) out << ",";
                out << "{\"id\":" << members[j]->id << ",\"distance\":"
                    << Distance(members[j]->embedding.data(), group.embedding.data(),
                        group.embedding.size());
                if (members.size() == 1) {
                    const AdaptiveLearningSample* nearest = nullptr;
                    float nearestDistance = std::numeric_limits<float>::infinity();
                    for (const auto& other : archive.samples) {
                        if (other.id == members[j]->id ||
                            other.embedding.size() != group.embedding.size()) continue;
                        const float distance = Distance(members[j]->embedding.data(),
                            other.embedding.data(), group.embedding.size());
                        if (distance < nearestDistance ||
                            (nearest && distance == nearestDistance && other.id < nearest->id)) {
                            nearest = &other; nearestDistance = distance;
                        }
                    }
                    if (nearest) out << ",\"nearestSampleId\":" << nearest->id
                                     << ",\"nearestDistance\":" << nearestDistance;
                }
                out << "}";
            }
            out << "]}";
        }
        out << "]}}";
    }
    out << "]";
    return out.str();
}

} // namespace facelogin
