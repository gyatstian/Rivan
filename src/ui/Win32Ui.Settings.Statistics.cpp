// Win32Ui.Settings.Statistics.cpp
// Statistics dashboard rendering for Win32Ui::Impl.
#include "Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::DrawStatisticsSection(const float left, const float right, float& y,
                                          std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    const auto& dashboard = model.statistics;
    const auto periodName = [](const stats::DashboardPeriod period) -> const wchar_t* {
        switch (period) {
        case stats::DashboardPeriod::Week: return L"WEEK";
        case stats::DashboardPeriod::FourWeeks: return L"4 WEEKS";
        case stats::DashboardPeriod::Month: return L"MONTH";
        case stats::DashboardPeriod::SixMonths: return L"6 MONTHS";
        case stats::DashboardPeriod::Year: return L"YEAR";
        case stats::DashboardPeriod::AllTime: return L"ALL TIME";
        }
        return L"WEEK";
    };
    constexpr std::array periodOptions = {
        stats::DashboardPeriod::Week,
        stats::DashboardPeriod::FourWeeks,
        stats::DashboardPeriod::Month,
        stats::DashboardPeriod::SixMonths,
        stats::DashboardPeriod::Year,
        stats::DashboardPeriod::AllTime
    };

    const auto number = [](const std::uint64_t value) { return std::to_wstring(value); };
    const auto minutes = [&number](const std::uint64_t seconds) {
        return number(seconds / 60);
    };
    const auto hours = [&number](const std::uint64_t seconds) {
        return number(seconds / 3600);
    };

    const float periodControlWidth = std::clamp((right - left) * 0.32F, 104.0F, 130.0F);
    DrawText(L"PERIOD", Rect(left, y, right - periodControlWidth - 7.0F, y + 23),
             b[8].Get(), headingFormat.Get());
    const auto periodButton = Rect(right - periodControlWidth, y, right, y + 24);
    std::wstring buttonLabel = std::wstring(periodName(dashboard.period)) + (statisticsPeriodDropdown ? L" ▴" : L" ▾");
    SettingsButton(periodButton, buttonLabel, 27, b);

    if (statisticsPeriodDropdown) {
        y = periodButton.bottom + 3.0F;
        const float optionHeight = 24.0F;
        const float dropBottom = y + optionHeight * periodOptions.size() + 4.0F;
        DrawBevel(Rect(periodButton.left, y, periodButton.right, dropBottom), b[1].Get(), b[3].Get(), b[4].Get(), true);
        for (std::size_t i = 0; i < periodOptions.size(); ++i) {
            const auto row = Rect(periodButton.left + 2.0F, y + 2.0F + i * optionHeight,
                                  periodButton.right - 2.0F, y + 2.0F + (i + 1) * optionHeight);
            const bool selected = dashboard.period == periodOptions[i];
            const bool hot = Contains(row, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            if (selected) target->FillRectangle(row, b[11].Get());
            else if (hot) target->FillRectangle(row, b[7].Get());
            DrawText(periodName(periodOptions[i]), Rect(row.left + 5, row.top, row.right - 4, row.bottom),
                     selected ? b[12].Get() : b[9].Get(), smallFormat.Get());
            // Clip hit region to viewport
            const float clipTop = settingsDetailsBounds.top + 15.0F;
            const float clipBottom = settingsDetailsBounds.bottom - 4.0F;
            if (row.bottom > clipTop && row.top < clipBottom) {
                D2D1_RECT_F hitBounds = row;
                hitBounds.top = std::max(row.top, clipTop);
                hitBounds.bottom = std::min(row.bottom, clipBottom);
                if (hitBounds.bottom > hitBounds.top) {
                    HitRegion hit;
                    hit.bounds = hitBounds;
                    hit.kind = HitKind::SettingsAction;
                    hit.id = 320 + i;
                    hits.push_back(hit);
                }
            }
        }
        y = dropBottom + 10.0F;
    } else {
        y = periodButton.bottom + 10.0F;
    }
    constexpr float metricGap = 7.0F;
    constexpr float minimumMetricWidth = 96.0F;
    constexpr float maximumMetricWidth = 100.0F;
    constexpr float metricHeight = 58.0F;
    constexpr std::size_t metricCount = 4;
    const float metricAreaWidth = std::max(0.0F, right - left);
    const std::size_t metricColumns = std::min<std::size_t>(metricCount,
        std::max<std::size_t>(1, static_cast<std::size_t>(
            (metricAreaWidth + metricGap) / (minimumMetricWidth + metricGap))));
    const float metricWidth = std::min(
        maximumMetricWidth,
        (metricAreaWidth - metricGap * static_cast<float>(metricColumns - 1)) /
            static_cast<float>(metricColumns));
    const float metricGridWidth = metricWidth * static_cast<float>(metricColumns) +
                                  metricGap * static_cast<float>(metricColumns - 1);
    const float metricLeft = left + std::max(0.0F, (metricAreaWidth - metricGridWidth) * 0.5F);
    const auto metric = [&](const std::size_t index, const std::wstring& value,
                            const wchar_t* label) {
        const std::size_t column = index % metricColumns;
        const std::size_t row = index / metricColumns;
        const auto card = Rect(metricLeft + static_cast<float>(column) * (metricWidth + metricGap),
                               y + static_cast<float>(row) * (metricHeight + metricGap),
                               metricLeft + static_cast<float>(column) * (metricWidth + metricGap) + metricWidth,
                               y + static_cast<float>(row) * (metricHeight + metricGap) + metricHeight);
        DrawBevel(card, b[1].Get(), b[3].Get(), b[4].Get(), true);
        DrawText(value, Rect(card.left + 2, card.top + 3, card.right - 2, card.top + 32),
                 b[12].Get(), headingFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(label, Rect(card.left + 2, card.top + 34, card.right - 2, card.bottom - 3),
                 b[6].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    metric(0, number(dashboard.plays), L"PLAY COUNT");
    metric(1, minutes(dashboard.seconds), L"MINUTES STREAMED");
    metric(2, hours(dashboard.seconds), L"HOURS STREAMED");
    metric(3, number(static_cast<std::uint64_t>(dashboard.differentTracks)), L"DIFFERENT TRACKS");
    y += static_cast<float>((metricCount + metricColumns - 1) / metricColumns) * metricHeight +
         static_cast<float>((metricCount - 1) / metricColumns) * metricGap + 10.0F;

    const auto drawList = [&](const wchar_t* title, const bool expanded, const std::size_t page,
                               const bool canPagePrevious, const bool canPageNext,
                               const std::uint64_t expandAction, const std::uint64_t previousAction,
                               const std::uint64_t nextAction, const auto& rows,
                               const auto& primaryFor, const auto& secondaryFor,
                               const bool showCoverArt, const auto& coverPathFor) {
        const std::size_t begin = expanded ? 0 : std::min(page * 7, rows.size());
        const std::size_t end = expanded ? rows.size() : std::min(begin + 7, rows.size());
        DrawText(title, Rect(left, y, right - 108.0F, y + 25), b[8].Get(), headingFormat.Get());
        const float buttonWidth = 30.0F;
        const auto control = [&](const D2D1_RECT_F& bounds, const wchar_t* label,
                                 const std::uint64_t action, const bool enabled) {
            if (enabled) {
                SettingsButton(bounds, label, action, b);
                return;
            }
            DrawBevel(bounds, b[1].Get(), b[3].Get(), b[4].Get(), true);
            DrawText(label, bounds, b[10].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        };
        control(Rect(right - buttonWidth, y, right, y + 24), L"\u25a6", expandAction,
                !rows.empty());
        control(Rect(right - buttonWidth * 3.0F - 6.0F, y,
                     right - buttonWidth * 2.0F - 6.0F, y + 24), L"\u25c0", previousAction,
                canPagePrevious);
        control(Rect(right - buttonWidth * 2.0F - 3.0F, y,
                     right - buttonWidth - 3.0F, y + 24), L"\u25b6", nextAction,
                canPageNext);
        y += 31;

        constexpr float gap = 7.0F;
        const float cardAreaWidth = std::max(0.0F, right - left);
        const float maximumCardWidth = showCoverArt ? 110.0F : 132.0F;
        // Keep the established Top Artists column calculation unchanged. Track cards
        // use smaller cells so their cover art fits without widening the dashboard.
        const std::size_t columns = showCoverArt
            ? std::min<std::size_t>(4, std::max<std::size_t>(1, static_cast<std::size_t>(
                  (cardAreaWidth + gap) / (96.0F + gap))))
            : std::max<std::size_t>(1, std::min<std::size_t>(
                  4, static_cast<std::size_t>((cardAreaWidth + gap) / 150.0F)));
        const float availableCardWidth =
            (cardAreaWidth - gap * static_cast<float>(columns - 1)) /
            static_cast<float>(columns);
        const float cardWidth = showCoverArt ? std::min(maximumCardWidth, availableCardWidth)
                                             : availableCardWidth;
        const float gridWidth = cardWidth * static_cast<float>(columns) +
                                gap * static_cast<float>(columns - 1);
        const float gridLeft = left + std::max(0.0F, (cardAreaWidth - gridWidth) * 0.5F);
        const float cardHeight = showCoverArt ? 118.0F : 51.0F;
        if (begin == end) {
            DrawText(L"< NO LISTENING DATA FOR THIS PERIOD >", Rect(left, y, right, y + 24),
                     b[10].Get(), regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 29;
            return;
        }
        for (std::size_t index = begin; index < end; ++index) {
            const std::size_t position = index - begin;
            const std::size_t column = position % columns;
            const std::size_t row = position / columns;
            const auto card = Rect(gridLeft + static_cast<float>(column) * (cardWidth + gap),
                                    y + static_cast<float>(row) * (cardHeight + gap),
                                    gridLeft + static_cast<float>(column) * (cardWidth + gap) + cardWidth,
                                    y + static_cast<float>(row) * (cardHeight + gap) + cardHeight);
            DrawBevel(card, b[1].Get(), b[3].Get(), b[4].Get(), true);
            const auto& item = rows[index];
            const std::wstring primary = primaryFor(item);
            const std::wstring secondary = secondaryFor(item);
            const std::wstring details = minutes(item.seconds) + L" min";
            float textTop = card.top + 3.0F;
            if (showCoverArt) {
                const float coverSize = std::clamp(cardWidth - 22.0F, 44.0F, 66.0F);
                const auto cover = Rect(card.left + (cardWidth - coverSize) * 0.5F, card.top + 5.0F,
                                        card.left + (cardWidth + coverSize) * 0.5F,
                                        card.top + 5.0F + coverSize);
                DrawBevel(cover, b[1].Get(), b[3].Get(), b[4].Get(), true);
                if (!DrawTrackCover(coverPathFor(item), cover)) {
                    DrawText(L"NO COVER", cover, b[10].Get(), tinyFormat.Get(),
                             DWRITE_TEXT_ALIGNMENT_CENTER);
                }
                textTop = cover.bottom + 2.0F;
            }
            DrawText(primary, Rect(card.left + 5, textTop, card.right - 5, textTop + 17.0F),
                      b[9].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, nullptr, true);
            DrawText(secondary, Rect(card.left + 5, textTop + 17.0F, card.right - 5, textTop + 32.0F),
                      b[6].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER,
                      DWRITE_PARAGRAPH_ALIGNMENT_CENTER, D2D1_DRAW_TEXT_OPTIONS_CLIP, nullptr, true);
            DrawText(details, Rect(card.left + 5, textTop + 32.0F, card.right - 5, card.bottom - 3),
                      b[10].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        y += static_cast<float>((end - begin + columns - 1) / columns) * (cardHeight + gap);
    };

    drawList(L"TOP TRACKS", dashboard.tracksExpanded, dashboard.tracksPage,
              dashboard.tracksCanPagePrevious, dashboard.tracksCanPageNext,
              300, 301, 302, dashboard.tracks,
              [](const StatisticsTrackView& track) { return track.title; },
              [](const StatisticsTrackView& track) { return track.artist; },
              true,
              [](const StatisticsTrackView& track) -> const std::wstring& {
                  return track.filePath;
              });
    y += 8;
    drawList(L"TOP ARTISTS", dashboard.artistsExpanded, dashboard.artistsPage,
             dashboard.artistsCanPagePrevious, dashboard.artistsCanPageNext,
              310, 311, 312, dashboard.artists,
              [](const StatisticsArtistView& artist) { return artist.name; },
              [](const StatisticsArtistView& artist) {
                  return std::to_wstring(artist.trackCount) + L" track" +
                         (artist.trackCount == 1 ? L"" : L"s");
              },
              false,
              [](const StatisticsArtistView&) -> const std::wstring& {
                  static const std::wstring empty;
                  return empty;
              });
    y += 4;
}

} // namespace rivan::ui
