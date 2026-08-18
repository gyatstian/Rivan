// LyricsService.Network.cpp
// Online lyric fetching: query building, WinHTTP transport, and search scoring.
#include "LyricsService.Internal.h"

#include "../core/Text.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace rivan::lyrics {
namespace {

constexpr DWORD kNetworkTimeoutMilliseconds = 4000;
constexpr auto kFetchBudget = std::chrono::seconds{20};
constexpr std::wstring_view kLrclibBaseUrl = L"https://lrclib.net/api/";

struct HttpResponse final {
    DWORD status{};
    std::string body;
};

std::wstring NormalizeSearchText(std::wstring value) {
    value = detail::FoldCase(detail::Trim(std::move(value)));
    std::wstring result;
    result.reserve(value.size());
    bool previousSpace = false;
    for (const wchar_t character : value) {
        if (std::iswspace(character)) {
            if (!previousSpace) result.push_back(L' ');
            previousSpace = true;
        } else {
            result.push_back(character);
            previousSpace = false;
        }
    }
    while (!result.empty() && (result.back() == L'.' || result.back() == L',' ||
                               result.back() == L'!' || result.back() == L'?' ||
                               result.back() == L';' || result.back() == L':')) {
        result.pop_back();
    }
    return detail::Trim(std::move(result));
}

std::wstring RemoveBracketedSuffix(std::wstring value) {
    value = detail::Trim(std::move(value));
    for (const auto marker : {L" (", L" [", L" {"}) {
        const auto position = value.find(marker);
        if (position != std::wstring::npos && position > 0) value.resize(position);
    }
    return detail::Trim(std::move(value));
}

std::vector<std::wstring> QueryVariants(std::wstring value) {
    std::vector<std::wstring> result;
    const auto add = [&result](std::wstring candidate) {
        candidate = detail::Trim(std::move(candidate));
        if (candidate.empty()) return;
        if (std::find(result.begin(), result.end(), candidate) == result.end()) {
            result.push_back(std::move(candidate));
        }
    };
    add(value);
    add(RemoveBracketedSuffix(value));
    auto normalized = NormalizeSearchText(value);
    add(normalized);
    add(RemoveBracketedSuffix(normalized));
    return result;
}

std::wstring UrlEncode(std::wstring_view value) {
    const auto utf8 = core::WideToUtf8(value);
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const unsigned char character : utf8) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            encoded << static_cast<char>(character);
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(character);
        }
    }
    return core::Utf8ToWide(encoded.str());
}

std::optional<HttpResponse> HttpGet(std::wstring_view url, std::stop_token stop) {
    if (stop.stop_requested()) return std::nullopt;
    URL_COMPONENTS components{sizeof(components)};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    wchar_t extra[2048]{};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    components.lpszExtraInfo = extra;
    components.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(url.data(), static_cast<DWORD>(url.size()), 0, &components)) return std::nullopt;

    HINTERNET session = WinHttpOpen(L"Rivan Lyrics/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session, kNetworkTimeoutMilliseconds, kNetworkTimeoutMilliseconds,
                       kNetworkTimeoutMilliseconds, kNetworkTimeoutMilliseconds);
    HINTERNET connection = WinHttpConnect(session, host, components.nPort, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    const std::wstring objectName = std::wstring(path, components.dwUrlPathLength) +
                                    std::wstring(extra, components.dwExtraInfoLength);
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", objectName.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        if (request) WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    DWORD status{};
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX) || status < 200 || status >= 300) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    std::string body;
    DWORD available{};
    bool responseTooLarge = false;
    while (!stop.stop_requested() && WinHttpQueryDataAvailable(request, &available) && available != 0) {
        if (body.size() + available > detail::kMaximumResponseBytes) {
            responseTooLarge = true;
            break;
        }
        const auto oldSize = body.size();
        body.resize(oldSize + available);
        DWORD read{};
        if (!WinHttpReadData(request, body.data() + oldSize, available, &read)) {
            body.clear();
            break;
        }
        body.resize(oldSize + read);
        if (read == 0) break;
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return stop.stop_requested() || responseTooLarge
               ? std::nullopt
               : std::optional<HttpResponse>(HttpResponse{status, std::move(body)});
}

} // namespace

std::optional<LyricsDocument> LyricsService::Fetch(const RequestData& request,
                                                   std::stop_token stop) const {
    if (request.title.empty()) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + kFetchBudget;
    const auto canTry = [&] {
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) return false;
        std::scoped_lock lock(mutex_);
        return request.generation == generation_;
    };

    const auto titles = QueryVariants(request.title);
    const auto albums = QueryVariants(request.album);
    const auto duration = std::max(0.0, request.durationSeconds);
    const auto durationText = duration > 0.0
                                  ? std::to_wstring(static_cast<long long>(std::llround(duration)))
                                  : std::wstring{};
    const auto fetchExact = [&](std::wstring_view title, std::wstring_view album)
        -> std::optional<LyricsDocument> {
        auto query = std::wstring(kLrclibBaseUrl) + L"get?track_name=" + UrlEncode(title) +
                     L"&artist_name=" + UrlEncode(request.artist) +
                     L"&album_name=" + UrlEncode(album);
        if (!durationText.empty()) query += L"&duration=" + durationText;
        if (const auto response = HttpGet(query, stop); response && response->status >= 200 &&
            response->status < 300) {
            auto document = ParseLrclibResponse(response->body);
            if (!document.Empty()) return document;
        }
        return std::nullopt;
    };
    for (const auto& title : titles) {
        if (!canTry()) return std::nullopt;
        if (const auto document = fetchExact(title, request.album)) return document;
        if (!request.album.empty()) {
            for (const auto& album : albums) {
                if (!canTry()) return std::nullopt;
                if (album == request.album) continue;
                if (const auto document = fetchExact(title, album)) return document;
            }
        }
    }

    const auto fetchSearch = [&](std::wstring_view title, std::wstring_view artist)
        -> std::optional<LyricsDocument> {
        if (!canTry()) return std::nullopt;
        auto query = std::wstring(kLrclibBaseUrl) + L"search?track_name=" + UrlEncode(title);
        if (!artist.empty()) query += L"&artist_name=" + UrlEncode(artist);
        const auto response = HttpGet(query, stop);
        if (!response || response->status < 200 || response->status >= 300) return std::nullopt;

        const auto requestedTitle = NormalizeSearchText(std::wstring(title));
        const auto requestedArtist = NormalizeSearchText(std::wstring(artist));
        const auto requestedAlbum = NormalizeSearchText(request.album);
        const auto root = core::ParseJson(response->body);
        if (!root || root->kind != core::JsonValue::Kind::Array) return std::nullopt;
        int bestScore = std::numeric_limits<int>::min();
        bool ambiguousBest = false;
        LyricsDocument best;
        for (const auto& object : root->array) {
            if (object.kind != core::JsonValue::Kind::Object) continue;
            const auto candidateTitle = detail::JsonWideString(object, "trackName");
            const auto candidateArtist = detail::JsonWideString(object, "artistName");
            const auto candidateAlbum = detail::JsonWideString(object, "albumName");
            if (NormalizeSearchText(candidateTitle) != requestedTitle) continue;
            if (!requestedArtist.empty() && NormalizeSearchText(candidateArtist) != requestedArtist) continue;
            const auto candidateDuration = detail::JsonNumber(object, "duration");
            int score = 100;
            if (!requestedArtist.empty()) score += 100;
            if (!requestedAlbum.empty() && NormalizeSearchText(candidateAlbum) == requestedAlbum) score += 30;
            if (duration > 0.0 && candidateDuration) {
                const auto difference = std::abs(*candidateDuration - duration);
                if (difference > 8.0) continue;
                score += difference < 1.5 ? 30 : difference < 4.0 ? 15 : 0;
            }
            auto document = detail::ParseLrclibObject(object);
            if (!document.Empty()) {
                if (score > bestScore) {
                    bestScore = score;
                    ambiguousBest = false;
                    best = std::move(document);
                } else if (score == bestScore) {
                    ambiguousBest = true;
                }
            }
        }
        if (best.Empty() || (requestedArtist.empty() && requestedAlbum.empty() && duration <= 0.0 && ambiguousBest)) {
            return std::nullopt;
        }
        return std::optional<LyricsDocument>(std::move(best));
    };
    for (const auto& title : titles) {
        if (!canTry()) return std::nullopt;
        if (const auto document = fetchSearch(title, request.artist)) return document;
    }
    if (!request.artist.empty()) {
        for (const auto& title : titles) {
            if (!canTry()) return std::nullopt;
            if (const auto document = fetchSearch(title, {})) return document;
        }
    }

    if (!request.artist.empty()) {
        const auto fallback = L"https://api.lyrics.ovh/v1/" + UrlEncode(request.artist) +
                              L"/" + UrlEncode(request.title);
        if (const auto response = HttpGet(fallback, stop); response && response->status >= 200 &&
            response->status < 300) {
            const auto root = core::ParseJson(response->body);
            if (root && root->kind == core::JsonValue::Kind::Object) {
                if (const auto lyrics = detail::JsonString(*root, "lyrics"); lyrics && !lyrics->empty()) {
                    auto document = ParseLrc(core::Utf8ToWide(*lyrics));
                    document.synced = false;
                    for (auto& line : document.lines) line.timestampSeconds = -1.0;
                    if (!document.Empty()) return document;
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace rivan::lyrics