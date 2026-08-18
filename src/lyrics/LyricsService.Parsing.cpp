// LyricsService.Parsing.cpp
// LRC / lrclib-JSON / custom-lyrics parsing and lyric document transformations.
#include "LyricsService.Internal.h"

#include "../core/Text.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <random>
#include <sstream>
#include <utility>

namespace rivan::lyrics::detail {

std::wstring Trim(std::wstring value) {
    const auto notSpace = [](wchar_t character) { return !std::iswspace(character); };
    const auto first = std::find_if(value.begin(), value.end(), notSpace);
    const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
    if (first >= last) return {};
    return std::wstring(first, last);
}

std::optional<std::string> JsonString(const core::JsonValue& object, std::string_view field) {
    const auto* member = core::JsonMember(object, field);
    return member && member->kind == core::JsonValue::Kind::String
               ? std::optional<std::string>(member->string)
               : std::nullopt;
}

std::optional<double> JsonNumber(const core::JsonValue& object, std::string_view field) {
    const auto* member = core::JsonMember(object, field);
    return member && member->kind == core::JsonValue::Kind::Number &&
                   std::isfinite(member->number)
               ? std::optional<double>(member->number)
               : std::nullopt;
}

std::wstring JsonWideString(const core::JsonValue& object, std::string_view field) {
    const auto value = JsonString(object, field);
    return value ? core::Utf8ToWide(*value) : std::wstring{};
}

LyricsDocument ParseLrclibObject(const core::JsonValue& root) {
    LyricsDocument document;
    if (const auto synced = JsonString(root, "syncedLyrics"); synced && !synced->empty()) {
        document = LyricsService::ParseLrc(core::Utf8ToWide(*synced));
    }
    if (document.Empty()) {
        if (const auto plain = JsonString(root, "plainLyrics"); plain && !plain->empty()) {
            document = LyricsService::ParseLrc(core::Utf8ToWide(*plain));
            document.synced = false;
            for (auto& line : document.lines) line.timestampSeconds = -1.0;
        }
    }
    return document;
}

std::optional<double> ParseNumber(std::wstring_view value) {
    double number{};
    std::wistringstream stream{std::wstring(value)};
    stream >> number;
    return stream && std::isfinite(number) ? std::optional<double>(number) : std::nullopt;
}

} // namespace rivan::lyrics::detail

namespace rivan::lyrics {

LyricsDocument LyricsService::ParseLrc(std::wstring_view text) {
    LyricsDocument document;
    std::wistringstream stream{std::wstring(text)};
    std::wstring row;
    while (std::getline(stream, row)) {
        row = detail::Trim(std::move(row));
        if (row.empty()) continue;
        std::vector<double> timestamps;
        std::size_t position = 0;
        while (position < row.size() && row[position] == L'[') {
            const auto close = row.find(L']', position + 1);
            if (close == std::wstring::npos) break;
            const auto colon = row.find(L':', position + 1);
            if (colon == std::wstring::npos || colon > close) break;
            const auto minutes = detail::ParseNumber(std::wstring_view(row).substr(position + 1, colon - position - 1));
            const auto seconds = detail::ParseNumber(std::wstring_view(row).substr(colon + 1, close - colon - 1));
            if (!minutes || !seconds || *minutes < 0.0 || *seconds < 0.0 || *seconds >= 60.0) break;
            timestamps.push_back(*minutes * 60.0 + *seconds);
            position = close + 1;
        }
        if (timestamps.empty() && row.front() == L'[' && row.find(L']') != std::wstring::npos) {
            const auto tagEnd = row.find(L']');
            const auto tag = row.substr(1, tagEnd - 1);
            const auto tagSeparator = tag.find(L':');
            if (tagSeparator != std::wstring::npos && tagSeparator > 0 &&
                std::all_of(tag.begin(), tag.begin() + static_cast<std::ptrdiff_t>(tagSeparator),
                            [](const wchar_t character) { return std::iswalpha(character) != 0; })) {
                continue;
            }
        }
        const auto lyric = detail::Trim(row.substr(position));
        if (timestamps.empty()) {
            if (!lyric.empty()) document.lines.push_back({-1.0, lyric});
        } else {
            document.synced = true;
            for (const double timestamp : timestamps) document.lines.push_back({timestamp, lyric});
        }
    }
    std::stable_sort(document.lines.begin(), document.lines.end(), [](const auto& left, const auto& right) {
        return left.timestampSeconds >= 0.0 &&
               (right.timestampSeconds < 0.0 || left.timestampSeconds < right.timestampSeconds);
    });
    return document;
}

// Whole-document strictness: any malformed token anywhere in the response (not just in
// the fields read) rejects the entire document -- stricter than the former field scanner,
// which tolerated stray garbage outside the fields it looked at.
LyricsDocument LyricsService::ParseLrclibResponse(std::string_view json) {
    const auto root = core::ParseJson(json);
    if (!root || root->kind != core::JsonValue::Kind::Object) return {};
    return detail::ParseLrclibObject(*root);
}

LyricsDocument LyricsService::ParseCustomLyrics(std::wstring_view text) {
    // Strip the leading '#RIVAN-CUSTOM-LYRICS-1' header block, then feed the remaining
    // text to the shared LRC parser so plain and synced custom lyrics honor linebreaks
    // and timestamps exactly like fetched lyrics.
    std::wistringstream stream{std::wstring(text)};
    std::wstring row;
    std::wstring body;
    bool inHeader = true;
    while (std::getline(stream, row)) {
        row = detail::Trim(std::move(row));
        if (inHeader) {
            if (row.empty()) continue;
            if (row.front() == L'#') continue;
            inHeader = false;
        }
        if (!body.empty()) body += L'\n';
        body += row;
    }
    return ParseLrc(body);
}

LyricsDocument LyricsService::WithFakeTimestamps(LyricsDocument document) {
    bool hasUntimedLine = false;
    for (const auto& line : document.lines) {
        if (line.timestampSeconds < 0.0) {
            hasUntimedLine = true;
            break;
        }
    }
    // Fully timed documents keep their real timestamps untouched.
    if (!hasUntimedLine) return document;
    std::mt19937 generator(
        static_cast<std::uint32_t>(std::random_device{}()) ^
        static_cast<std::uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    // Weighted gaps over 2-7s: 4s and 5s dominate (~67%) so generated timing reads
    // naturally, while occasional 2-3s/6-7s spacing keeps it varied.
    std::discrete_distribution<int> gapIndex({1, 2, 6, 6, 2, 1});
    double cursor = 0.0;
    for (auto& line : document.lines) {
        if (line.timestampSeconds >= 0.0) {
            // Anchor on any real timestamp so generated lines never drift from the
            // actual song timeline.
            cursor = line.timestampSeconds;
            continue;
        }
        cursor += static_cast<double>(gapIndex(generator) + 2);
        line.timestampSeconds = cursor;
    }
    document.synced = true;
    return document;
}

// Extracts the '#Song file:' header value from a lyrics file body.
std::wstring LyricsService::UserLyricsTrackPath(std::wstring_view text) {
    std::wistringstream stream{std::wstring(text)};
    std::wstring row;
    while (std::getline(stream, row)) {
        row = detail::Trim(std::move(row));
        if (row.empty()) continue;
        if (row.front() != L'#') return {};
        if (row.rfind(detail::kSongFileHeader, 0) == 0) {
            return detail::Trim(row.substr(detail::kSongFileHeader.size()));
        }
    }
    return {};
}

} // namespace rivan::lyrics