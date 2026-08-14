// Rivan source file
// Purpose: Dependency-free listen-statistics core: period keys, snapshot names,
// per-entity counters, INI persistence, and period rollover.
#include "ListenStats.h"

#include "../core/IniDocument.h"

#include <charconv>
#include <system_error>

namespace rivan::stats {
namespace {

// ---------------------------------------------------------------------------
// Civil-date arithmetic (Howard Hinnant's days_from_civil / civil_from_days,
// integer only). Values are days since 1970-01-01.
// ---------------------------------------------------------------------------

struct CivilDate {
    int year;
    int month;  // 1-12
    int day;    // 1-31
};

constexpr int DaysFromCivil(int year, unsigned month, unsigned day) noexcept {
    const int m = static_cast<int>(month);
    year -= m <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const int yearOfEra = year - era * 400;
    const int dayOfYear = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 +
                          static_cast<int>(day) - 1;
    const int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}

constexpr CivilDate CivilFromDays(int daysSinceEpoch) noexcept {
    int z = daysSinceEpoch + 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const int dayOfEra = z - era * 146097;
    const int yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    int year = yearOfEra + era * 400;
    const int dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const int monthPrime = (5 * dayOfYear + 2) / 153;
    const int day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
    const int month = monthPrime + (monthPrime < 10 ? 3 : -9);
    year += month <= 2;
    return {year, month, day};
}

constexpr int kMondayReference = DaysFromCivil(1970, 1, 5);  // 1970-01-05 is a Monday

// ---------------------------------------------------------------------------
// Formatting / parsing helpers.
// ---------------------------------------------------------------------------

std::string TwoDigits(int value) {
    if (value < 0 || value > 99) {
        return std::to_string(value);
    }
    std::string result(2, '0');
    result[0] = static_cast<char>('0' + value / 10);
    result[1] = static_cast<char>('0' + value % 10);
    return result;
}

std::string FormatDdMmYyyy(const CivilDate& date) {
    return TwoDigits(date.day) + TwoDigits(date.month) + std::to_string(date.year);
}

std::string FormatMmYyyy(int month, int year) {
    return TwoDigits(month) + std::to_string(year);
}

bool ParseDigits(std::string_view text, int& out) noexcept {
    if (text.empty()) {
        return false;
    }
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

std::string FormatPeriodValue(const PeriodData& period) {
    return std::to_string(period.plays) + "|" + std::to_string(period.seconds);
}

bool ParseUint64(std::string_view text, std::uint64_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// Reads a "plays|seconds" value into `out`. Malformed values are ignored (the
// caller keeps whatever `out` already holds).
bool ParsePeriodKey(const rivan::core::IniDocument& doc,
                    const std::string& section,
                    std::string_view key,
                    PeriodData& out) {
    const auto value = doc.Get(section, key);
    if (!value) {
        return false;
    }
    const auto separator = value->find('|');
    if (separator == std::string_view::npos) {
        return false;
    }
    std::uint64_t plays = 0;
    std::uint64_t seconds = 0;
    if (!ParseUint64(value->substr(0, separator), plays)) {
        return false;
    }
    if (!ParseUint64(value->substr(separator + 1), seconds)) {
        return false;
    }
    out.plays = plays;
    out.seconds = seconds;
    return true;
}

// Builds one period's snapshot document and writes it. Returns:
//   nullopt          - no entity carries data for the period (nothing to write)
//   empty path value - the write failed (caller must keep the period unsnapped)
//   valid path       - the snapshot file was written
std::optional<std::filesystem::path> WritePeriodSnapshot(
    const std::filesystem::path& statsDir,
    const ListenStatsModel& model,
    PeriodData EntityData::*periodMember,
    std::string_view formatValue,
    std::string_view snapshotName) {
    bool hasData = false;
    for (const auto& [sectionName, data] : model.entities) {
        (void)sectionName;
        if (!(data.*periodMember).Empty()) {
            hasData = true;
            break;
        }
    }
    if (!hasData) {
        return std::nullopt;
    }

    rivan::core::IniDocument doc;
    doc.Set("meta", "format", std::string(formatValue));
    doc.Set("meta", "period", std::string(snapshotName));
    for (const auto& [sectionName, data] : model.entities) {
        const PeriodData& period = data.*periodMember;
        if (period.Empty()) {
            continue;
        }
        const auto key = DecodeSectionName(sectionName);
        if (key && !data.songPath.empty()) {
            doc.Set(sectionName, "path", data.songPath);
        }
        doc.Set(sectionName, "p", FormatPeriodValue(period));
    }

    // Snapshot files are INI documents; the product-spec base name gets a ".ini"
    // suffix: weekly "17082026-23082026.ini", monthly "082026-092026.ini",
    // yearly "2026-2027.ini".
    const auto filePath = statsDir / (std::string(snapshotName) + ".ini");
    std::string error;
    if (!doc.SaveAtomic(filePath, &error)) {
        return std::filesystem::path{};
    }
    return filePath;
}

// Rolls one period type over. Returns the snapshot paths written (may be empty).
std::vector<std::filesystem::path> RolloverOnePeriod(
    const std::filesystem::path& statsDir,
    ListenStatsModel& model,
    PeriodData EntityData::*periodMember,
    std::string& storedLabel,
    const std::string& currentLabel,
    std::string_view formatValue,
    std::string (*snapshotNameFn)(std::string_view)) {
    if (storedLabel.empty()) {
        storedLabel = currentLabel;
        return {};
    }
    if (storedLabel == currentLabel) {
        return {};
    }

    std::vector<std::filesystem::path> written;
    const auto snapshotName = snapshotNameFn(storedLabel);
    if (snapshotName.empty()) {
        // Unparseable stored label (hand-edited or corrupt meta): keep the label and
        // counters so the next tick retries, never silently dropping period data.
        return {};
    }
    std::optional<std::filesystem::path> result;
    result = WritePeriodSnapshot(statsDir, model, periodMember, formatValue, snapshotName);
    // nullopt means the period had no data: nothing was (or needs to be) written.
    // A failed write leaves the stored label in place so the next tick retries; the
    // counters are only zeroed once the snapshot actually lands on disk.
    if (result && result->empty()) {
        return {};
    }
    if (result) {
        written.push_back(*result);
    }
    for (auto& [sectionName, data] : model.entities) {
        (void)sectionName;
        if (periodMember == &EntityData::week) {
            data.ClearWeek();
        } else if (periodMember == &EntityData::month) {
            data.ClearMonth();
        } else {
            data.ClearYear();
        }
    }
    storedLabel = currentLabel;
    return written;
}

} // namespace

// ---------------------------------------------------------------------------
// Section-name encoding.
// ---------------------------------------------------------------------------

std::string SongSectionName(std::uint64_t trackId) {
    static constexpr char kHex[] = "0123456789abcdef";
    char buffer[16];
    int length = 0;
    if (trackId == 0) {
        buffer[length++] = '0';
    }
    while (trackId != 0) {
        buffer[length++] = kHex[trackId & 0x0Fu];
        trackId >>= 4;
    }
    std::string result = "song:";
    for (int index = length - 1; index >= 0; --index) {
        result.push_back(buffer[index]);
    }
    return result;
}

std::optional<std::string> DecodeSectionName(std::string_view section) {
    if (!section.starts_with("song:")) {
        return std::nullopt;
    }
    return std::string(section.substr(5));
}

// ---------------------------------------------------------------------------
// Local-time period computation and snapshot naming.
// ---------------------------------------------------------------------------

PeriodKeys CurrentPeriodKeys(const std::tm& localNow) {
    const int year = localNow.tm_year + 1900;
    const int month = localNow.tm_mon + 1;
    const int days = DaysFromCivil(year, static_cast<unsigned>(month),
                                   static_cast<unsigned>(localNow.tm_mday));
    const int mondayDays = days - (((days - kMondayReference) % 7 + 7) % 7);
    const auto monday = CivilFromDays(mondayDays);

    PeriodKeys keys;
    keys.weekMonday = FormatDdMmYyyy(monday);
    keys.month = FormatMmYyyy(month, year);
    keys.year = std::to_string(year);
    return keys;
}

std::string WeeklySnapshotName(std::string_view weekMondayDdMmYyyy) {
    if (weekMondayDdMmYyyy.size() != 8) {
        return {};
    }
    int day = 0;
    int month = 0;
    int year = 0;
    if (!ParseDigits(weekMondayDdMmYyyy.substr(0, 2), day) ||
        !ParseDigits(weekMondayDdMmYyyy.substr(2, 2), month) ||
        !ParseDigits(weekMondayDdMmYyyy.substr(4, 4), year)) {
        return {};
    }
    if (day < 1 || day > 31 || month < 1 || month > 12) {
        return {};
    }
    const int mondayDays = DaysFromCivil(year, static_cast<unsigned>(month),
                                         static_cast<unsigned>(day));
    const auto sunday = CivilFromDays(mondayDays + 6);
    return FormatDdMmYyyy(CivilDate{year, month, day}) + "-" + FormatDdMmYyyy(sunday);
}

std::string MonthlySnapshotName(std::string_view monthMmYyyy) {
    if (monthMmYyyy.size() != 6) {
        return {};
    }
    int month = 0;
    int year = 0;
    if (!ParseDigits(monthMmYyyy.substr(0, 2), month) ||
        !ParseDigits(monthMmYyyy.substr(2, 4), year)) {
        return {};
    }
    if (month < 1 || month > 12) {
        return {};
    }
    std::string result = FormatMmYyyy(month, year);
    result += '-';
    if (month == 12) {
        result += FormatMmYyyy(1, year + 1);
    } else {
        result += FormatMmYyyy(month + 1, year);
    }
    return result;
}

std::string YearlySnapshotName(std::string_view yearYyyy) {
    if (yearYyyy.size() != 4) {
        return {};
    }
    int year = 0;
    if (!ParseDigits(yearYyyy, year)) {
        return {};
    }
    return std::string(yearYyyy) + "-" + std::to_string(year + 1);
}

// ---------------------------------------------------------------------------
// In-memory store.
// ---------------------------------------------------------------------------

void ListenStatsModel::AddPlay(const std::string& sectionName) {
    auto& data = entities[sectionName];
    ++data.week.plays;
    ++data.month.plays;
    ++data.year.plays;
    ++data.lifetime.plays;
}

void ListenStatsModel::AddSeconds(const std::string& sectionName, std::uint64_t seconds) {
    auto& data = entities[sectionName];
    data.week.seconds += seconds;
    data.month.seconds += seconds;
    data.year.seconds += seconds;
    data.lifetime.seconds += seconds;
}

void ListenStatsModel::SetSongPath(const std::string& sectionName, std::string utf8Path) {
    entities[sectionName].songPath = std::move(utf8Path);
}

EntityData* ListenStatsModel::Find(const std::string& sectionName) {
    const auto it = entities.find(sectionName);
    return it == entities.end() ? nullptr : &it->second;
}

const EntityData* ListenStatsModel::Find(const std::string& sectionName) const {
    const auto it = entities.find(sectionName);
    return it == entities.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// File IO.
// ---------------------------------------------------------------------------

core::IniDocument BuildMainStatsDocument(const ListenStatsModel& model) {
    rivan::core::IniDocument doc;
    doc.Set("meta", "format", "rivan-stats");
    doc.Set("meta", "week", model.periods.weekMonday);
    doc.Set("meta", "month", model.periods.month);
    doc.Set("meta", "year", model.periods.year);
    for (const auto& [sectionName, data] : model.entities) {
        if (data.Empty()) {
            continue;
        }
        const auto key = DecodeSectionName(sectionName);
        if (key && !data.songPath.empty()) {
            doc.Set(sectionName, "path", data.songPath);
        }
        doc.Set(sectionName, "l", FormatPeriodValue(data.lifetime));
        doc.Set(sectionName, "w", FormatPeriodValue(data.week));
        doc.Set(sectionName, "m", FormatPeriodValue(data.month));
        doc.Set(sectionName, "y", FormatPeriodValue(data.year));
    }
    return doc;
}

bool SaveMainStatsFile(const std::filesystem::path& path,
                       const ListenStatsModel& model,
                       std::string* error) {
    return BuildMainStatsDocument(model).SaveAtomic(path, error);
}

bool LoadMainStatsFile(const std::filesystem::path& path,
                       ListenStatsModel& model,
                       std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        model = ListenStatsModel{};
        return true;
    }

    const auto doc = rivan::core::IniDocument::Load(path, error);
    if (!doc) {
        return false;
    }
    const auto format = doc->Get("meta", "format");
    if (!format || *format != "rivan-stats") {
        if (error != nullptr) {
            *error = "not a rivan-stats file";
        }
        return false;
    }

    ListenStatsModel loaded;
    if (const auto week = doc->Get("meta", "week")) {
        loaded.periods.weekMonday = std::string(*week);
    }
    if (const auto month = doc->Get("meta", "month")) {
        loaded.periods.month = std::string(*month);
    }
    if (const auto year = doc->Get("meta", "year")) {
        loaded.periods.year = std::string(*year);
    }
    for (const auto& [sectionName, section] : doc->Data()) {
        (void)section;
        if (sectionName == "meta") {
            continue;
        }
        const auto key = DecodeSectionName(sectionName);
        if (!key) {
            continue;  // includes legacy [genre:...] / [author:...] sections
        }
        auto& data = loaded.entities[sectionName];
        ParsePeriodKey(*doc, sectionName, "l", data.lifetime);
        ParsePeriodKey(*doc, sectionName, "w", data.week);
        ParsePeriodKey(*doc, sectionName, "m", data.month);
        ParsePeriodKey(*doc, sectionName, "y", data.year);
        if (const auto songPath = doc->Get(sectionName, "path")) {
            data.songPath = std::string(*songPath);
        }
    }
    model = std::move(loaded);
    return true;
}

// ---------------------------------------------------------------------------
// Period rollover.
// ---------------------------------------------------------------------------

std::vector<std::filesystem::path> RolloverPeriods(
    const std::filesystem::path& statsDir,
    ListenStatsModel& model,
    const PeriodKeys& current) {
    std::vector<std::filesystem::path> written;
    const auto append = [&written](std::vector<std::filesystem::path> paths) {
        written.insert(written.end(), paths.begin(), paths.end());
    };
    append(RolloverOnePeriod(statsDir, model, &EntityData::week, model.periods.weekMonday,
                             current.weekMonday, "rivan-stats-weekly", WeeklySnapshotName));
    append(RolloverOnePeriod(statsDir, model, &EntityData::month, model.periods.month,
                             current.month, "rivan-stats-monthly", MonthlySnapshotName));
    append(RolloverOnePeriod(statsDir, model, &EntityData::year, model.periods.year,
                             current.year, "rivan-stats-yearly", YearlySnapshotName));
    return written;
}

// ---------------------------------------------------------------------------
// Listening session.
// ---------------------------------------------------------------------------

bool ListenSession::CountPlayIfQualified(double durationSeconds) noexcept {
    if (counted_) {
        return false;
    }
    if (durationSeconds <= 0.0) {
        return false;
    }
    if (seconds_ + 1e-9 >= 0.30 * durationSeconds) {
        counted_ = true;
        return true;
    }
    return false;
}

void ListenSession::Reset() noexcept {
    seconds_ = 0.0;
    counted_ = false;
}

} // namespace rivan::stats
