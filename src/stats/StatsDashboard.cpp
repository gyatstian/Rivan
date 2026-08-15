// src/stats/StatsDashboard.cpp
// Listen-statistics dashboard aggregation and metadata fallback logic.
#include "StatsDashboard.h"

#include "../core/Text.h"

#include <algorithm>
#include <charconv>

namespace rivan::stats {
namespace {

constexpr std::string_view kUnknownArtist = "Unknown artist";
constexpr std::string_view kUnknownTrack = "Unknown track";

[[nodiscard]] const PeriodData& ActivityFor(const EntityData& entity,
                                            DashboardPeriod period) noexcept {
    switch (period) {
    case DashboardPeriod::Week: return entity.week;
    case DashboardPeriod::FourWeeks: return entity.fourWeeks;
    case DashboardPeriod::Month: return entity.month;
    case DashboardPeriod::SixMonths: return entity.sixMonths;
    case DashboardPeriod::Year: return entity.year;
    case DashboardPeriod::AllTime: return entity.lifetime;
    }
    return entity.week;
}

[[nodiscard]] bool DecodeTrackId(const std::string& section,
                                 library::TrackId& id) noexcept {
    const auto encoded = DecodeSectionName(section);
    if (!encoded || encoded->empty()) return false;
    const auto result = std::from_chars(encoded->data(), encoded->data() + encoded->size(),
                                        id, 16);
    return result.ec == std::errc{} && result.ptr == encoded->data() + encoded->size();
}

[[nodiscard]] std::string SavedPathStem(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const auto filename = path.substr(slash == std::string_view::npos ? 0 : slash + 1);
    if (filename.empty()) return {};
    const auto dot = filename.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) return std::string(filename);
    return std::string(filename.substr(0, dot));
}

[[nodiscard]] std::string TrackTitle(const library::Track& track) {
    std::string title = core::WideToUtf8(track.title);
    if (!title.empty()) return title;
    title = core::WideToUtf8(track.filePath.stem().wstring());
    return title.empty() ? std::string(kUnknownTrack) : title;
}

[[nodiscard]] std::string TrackArtist(const library::Track& track) {
    const auto artist = core::WideToUtf8(track.artist);
    return artist.empty() ? std::string(kUnknownArtist) : artist;
}

[[nodiscard]] std::string FallbackTitle(const EntityData& entity) {
    const auto title = SavedPathStem(entity.songPath);
    return title.empty() ? std::string(kUnknownTrack) : title;
}

} // namespace

DashboardPeriod NextDashboardPeriod(const DashboardPeriod period) noexcept {
    switch (period) {
    case DashboardPeriod::Week: return DashboardPeriod::FourWeeks;
    case DashboardPeriod::FourWeeks: return DashboardPeriod::Month;
    case DashboardPeriod::Month: return DashboardPeriod::SixMonths;
    case DashboardPeriod::SixMonths: return DashboardPeriod::Year;
    case DashboardPeriod::Year: return DashboardPeriod::AllTime;
    case DashboardPeriod::AllTime: return DashboardPeriod::Week;
    }
    return DashboardPeriod::Week;
}

DashboardCatalog BuildDashboardCatalog(const std::span<const library::Track> allLibraryTracks) {
    DashboardCatalog catalog;
    catalog.tracks.reserve(allLibraryTracks.size());
    for (const auto& track : allLibraryTracks) {
        // A catalog should be unique, but imported/rescanned views can temporarily carry
        // the same id more than once. The first current metadata record wins.
        catalog.tracks.try_emplace(track.id,
                                   DashboardCatalogTrack{TrackTitle(track), TrackArtist(track),
                                                         track.filePath});
    }
    return catalog;
}

DashboardData BuildDashboard(const ListenStatsModel& model,
                             const DashboardCatalog& catalog,
                             const DashboardPeriod period) {
    DashboardData result;
    result.period = period;

    std::map<std::string, DashboardArtist> artists;
    for (const auto& [section, entity] : model.entities) {
        const auto& activity = ActivityFor(entity, period);
        if (activity.Empty()) continue;

        library::TrackId id{};
        if (!DecodeTrackId(section, id)) continue;

        DashboardTrack track;
        track.id = id;
        if (const auto found = catalog.tracks.find(id); found != catalog.tracks.end()) {
            track.title = found->second.title;
            track.artist = found->second.artist;
            track.filePath = found->second.filePath;
        } else {
            track.title = FallbackTitle(entity);
            track.artist = std::string(kUnknownArtist);
        }
        track.activity = activity;
        result.totals.plays += activity.plays;
        result.totals.seconds += activity.seconds;
        ++result.differentTracks;
        result.topTracks.push_back(std::move(track));

        auto [artist, inserted] = artists.try_emplace(result.topTracks.back().artist,
                                                       DashboardArtist{});
        if (inserted) artist->second.name = artist->first;
        artist->second.activity.plays += activity.plays;
        artist->second.activity.seconds += activity.seconds;
        ++artist->second.trackCount;
    }

    std::sort(result.topTracks.begin(), result.topTracks.end(), [](const auto& left, const auto& right) {
        if (left.activity.seconds != right.activity.seconds) {
            return left.activity.seconds > right.activity.seconds;
        }
        if (left.activity.plays != right.activity.plays) return left.activity.plays > right.activity.plays;
        if (left.title != right.title) return left.title < right.title;
        return left.id < right.id;
    });

    result.topArtists.reserve(artists.size());
    for (auto& [name, artist] : artists) {
        (void)name;
        result.topArtists.push_back(std::move(artist));
    }
    std::sort(result.topArtists.begin(), result.topArtists.end(), [](const auto& left, const auto& right) {
        if (left.activity.seconds != right.activity.seconds) {
            return left.activity.seconds > right.activity.seconds;
        }
        if (left.activity.plays != right.activity.plays) return left.activity.plays > right.activity.plays;
        return left.name < right.name;
    });
    return result;
}

} // namespace rivan::stats
