// src/stats/ListenStats.h
// Dependency-free listen-statistics core: local-time period keys, snapshot naming,
// the per-entity counters model, INI persistence, and period rollover.
#pragma once

#include "../core/IniDocument.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivan::stats {

// One period's accumulated data for a single entity.
struct PeriodData {
    std::uint64_t plays = 0;
    std::uint64_t seconds = 0;

    [[nodiscard]] bool Empty() const noexcept { return plays == 0 && seconds == 0; }
};

// All-time lifetime data plus the three concurrently tracked periods for one song.
struct EntityData {
    PeriodData week;
    PeriodData month;
    PeriodData year;
    // All-time totals. Bumped by every AddPlay/AddSeconds and never cleared, so
    // lifetime stats survive even when the period snapshot files are deleted.
    PeriodData lifetime;
    // UTF-8 absolute path of the file. Used for readability in snapshot and main files.
    std::string songPath;

    [[nodiscard]] bool Empty() const noexcept {
        return week.Empty() && month.Empty() && year.Empty() && lifetime.Empty();
    }
    void ClearWeek() noexcept { week = {}; }
    void ClearMonth() noexcept { month = {}; }
    void ClearYear() noexcept { year = {}; }
};

// ---------------------------------------------------------------------------
// Section-name encoding. Song sections are "song:" + lowercase track-id hex; the
// hex alphabet needs no sanitization against INI-forbidden characters.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string SongSectionName(std::uint64_t trackId);
// Returns the song's track-id hex for song sections, and nullopt for anything else
// ([meta], and legacy [genre:...]/[author:...] sections, which stay unloaded).
[[nodiscard]] std::optional<std::string> DecodeSectionName(std::string_view section);

// ---------------------------------------------------------------------------
// Local-time period computation. All keys are local-time based.
// ---------------------------------------------------------------------------
struct PeriodKeys {
    std::string weekMonday;  // "ddMMyyyy" of the Monday of the current week
    std::string month;       // "mmyyyy"
    std::string year;        // "yyyy"
};

// Computes period keys from a broken-down local date. Only tm_year (years since 1900),
// tm_mon (0-11), tm_mday (1-31) are read; tm_wday/tm_yday are ignored so tests can
// pass hand-built tm values.
[[nodiscard]] PeriodKeys CurrentPeriodKeys(const std::tm& localNow);

// Snapshot file base names (the ".ini" suffix is appended when writing the file):
//   Weekly  "17082026-23082026"  = Monday ddMMyyyy - Sunday ddMMyyyy of that week
//   Monthly "082026-092026"      = mmyyyy - mmyyyy (first of month - first of NEXT month)
//   Yearly  "2026-2027"          = yyyy - (yyyy+1)
[[nodiscard]] std::string WeeklySnapshotName(std::string_view weekMondayDdMmYyyy);
[[nodiscard]] std::string MonthlySnapshotName(std::string_view monthMmYyyy);
[[nodiscard]] std::string YearlySnapshotName(std::string_view yearYyyy);

// ---------------------------------------------------------------------------
// In-memory store. Entities are keyed by encoded section name.
// ---------------------------------------------------------------------------
struct ListenStatsModel {
    std::map<std::string, EntityData> entities;
    PeriodKeys periods;  // the periods the counters currently belong to ([meta])

    // +1 play on week, month AND year counters plus the all-time lifetime counter
    // for the entity.
    void AddPlay(const std::string& sectionName);
    // +seconds on week, month AND year counters plus the all-time lifetime counter
    // for the entity.
    void AddSeconds(const std::string& sectionName, std::uint64_t seconds);
    void SetSongPath(const std::string& sectionName, std::string utf8Path);
    [[nodiscard]] EntityData* Find(const std::string& sectionName);
    [[nodiscard]] const EntityData* Find(const std::string& sectionName) const;
};

// ---------------------------------------------------------------------------
// File IO
// ---------------------------------------------------------------------------
// Writes the main stats file with a [meta] header: format=rivan-stats and the three
// period keys (keys "week", "month", "year"). Song sections: [song:<hex>] with keys
// "l"=plays|seconds (all-time lifetime), "w"=plays|seconds, "m"=plays|seconds,
// "y"=plays|seconds, "path"=<utf8 path>. A section whose lifetime AND w/m/y are all
// empty is omitted. Returns false only on I/O failure.
[[nodiscard]] bool SaveMainStatsFile(const std::filesystem::path& path,
                                     const ListenStatsModel& model,
                                     std::string* error = nullptr);

// Loads the main file. A MISSING file is NOT an error: model is left empty and true
// is returned. If the file exists but its [meta] format != "rivan-stats", return
// false. Parses l/w/m/y/path keys; "l" may be absent (files written before lifetime
// existed) and then loads as zero. Unknown sections/keys are ignored, including
// legacy [genre:...] and [author:...] sections.
[[nodiscard]] bool LoadMainStatsFile(const std::filesystem::path& path,
                                     ListenStatsModel& model,
                                     std::string* error = nullptr);

// Builds the INI document for the main stats file (same content as
// SaveMainStatsFile writes). Exposed so callers can serialize under a lock
// and write outside it.
[[nodiscard]] core::IniDocument BuildMainStatsDocument(const ListenStatsModel& model);

// Period rollover. For each period type whose STORED label differs from the CURRENT
// label (and whose stored label is non-empty), write a snapshot file into statsDir
// containing that period's data, then zero that period in every entity and set the
// stored label to the current one. Snapshot files are INI documents named
// "<snapshot name>.ini" with content:
//   [meta] format=rivan-stats-weekly|monthly|yearly, period=<snapshot name>
//   per song section (only those whose period data is non-empty):
//     [song:<hex>] path=<utf8> p=plays|seconds
// Lifetime counters are never touched by rollover. A period with no data produces no
// snapshot file. Returns the paths of the snapshot files actually written. Applies
// to each of week, month, year independently.
[[nodiscard]] std::vector<std::filesystem::path> RolloverPeriods(
    const std::filesystem::path& statsDir,
    ListenStatsModel& model,
    const PeriodKeys& current);

// ---------------------------------------------------------------------------
// One continuous listening session: accumulated actual seconds + the 30% play rule.
// ---------------------------------------------------------------------------
class ListenSession {
public:
    void AddSeconds(double seconds) noexcept { seconds_ += seconds; }
    [[nodiscard]] double Seconds() const noexcept { return seconds_; }
    // Returns true exactly ONCE per session: the first time accumulated seconds
    // reach 30% of |durationSeconds|. Duration <= 0 (unknown) never qualifies.
    [[nodiscard]] bool CountPlayIfQualified(double durationSeconds) noexcept;
    void Reset() noexcept;

private:
    double seconds_{};
    bool counted_{};
};

} // namespace rivan::stats
