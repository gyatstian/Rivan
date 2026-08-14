// src/stats/StatsDashboard.h
// Dependency-free derivation of the current listen-statistics dashboard.
#pragma once

#include "ListenStats.h"

#include "../library/Track.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace rivan::stats {

enum class DashboardPeriod : std::uint8_t {
    Week,
    Month,
    Year,
    AllTime,
};

[[nodiscard]] DashboardPeriod NextDashboardPeriod(DashboardPeriod period) noexcept;

struct DashboardTrack {
    library::TrackId id{};
    std::string title;
    std::string artist;
    // Empty when this statistic no longer has a current catalog record. The view uses
    // this only to look up cover art; saved historical paths are intentionally not used.
    std::filesystem::path filePath;
    PeriodData activity;
};

struct DashboardArtist {
    std::string name;
    PeriodData activity;
    std::size_t trackCount{};
};

struct DashboardData {
    DashboardPeriod period{DashboardPeriod::Week};
    PeriodData totals;
    std::size_t differentTracks{};
    std::vector<DashboardTrack> topTracks;
    std::vector<DashboardArtist> topArtists;
};

struct DashboardCatalogTrack {
    std::string title;
    std::string artist;
    std::filesystem::path filePath;
};

// Canonical metadata keyed by library track id. Build it when the catalog changes,
// then reuse it for every current-period query.
struct DashboardCatalog {
    std::unordered_map<library::TrackId, DashboardCatalogTrack> tracks;
};

[[nodiscard]] DashboardCatalog BuildDashboardCatalog(
    std::span<const library::Track> allLibraryTracks);

// Derives one dashboard from the already loaded stats model and catalog. The catalog is
// read-only and no filesystem work is performed. Duplicate catalog ids were removed when
// it was built, so an all-library projection cannot inflate artist totals.
[[nodiscard]] DashboardData BuildDashboard(const ListenStatsModel& model,
                                           const DashboardCatalog& catalog,
                                           DashboardPeriod period);

} // namespace rivan::stats
