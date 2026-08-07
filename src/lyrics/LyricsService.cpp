#include "LyricsService.h"

#include "../core/Text.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>

#pragma comment(lib, "winhttp.lib")

namespace rivan::lyrics {
namespace {

constexpr std::size_t kMaximumResponseBytes = 1024U * 1024U;
constexpr DWORD kNetworkTimeoutMilliseconds = 4000;
constexpr auto kFetchBudget = std::chrono::seconds{20};
constexpr std::wstring_view kLrclibBaseUrl = L"https://lrclib.net/api/";

struct HttpResponse final {
    DWORD status{};
    std::string body;
};

std::wstring Trim(std::wstring value) {
    const auto notSpace = [](wchar_t character) { return !std::iswspace(character); };
    const auto first = std::find_if(value.begin(), value.end(), notSpace);
    const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
    if (first >= last) return {};
    return std::wstring(first, last);
}

std::wstring FoldCase(std::wstring value) {
    for (auto& character : value) character = static_cast<wchar_t>(std::towlower(character));
    return value;
}

std::wstring NormalizeSearchText(std::wstring value) {
    value = FoldCase(Trim(std::move(value)));
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
    return Trim(std::move(result));
}

void AppendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7fu) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else if (codePoint <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else if (codePoint <= 0x10ffffu) {
        output.push_back(static_cast<char>(0xf0u | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
}

std::optional<std::string> UnescapeJson(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\') {
            result.push_back(value[index]);
            continue;
        }
        if (++index >= value.size()) return std::nullopt;
        switch (value[index]) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': {
            if (index + 4 >= value.size()) return std::nullopt;
            std::uint32_t codePoint = 0;
            for (std::size_t digit = 1; digit <= 4; ++digit) {
                const char character = value[index + digit];
                codePoint <<= 4;
                if (character >= '0' && character <= '9') codePoint += static_cast<unsigned>(character - '0');
                else if (character >= 'a' && character <= 'f') codePoint += static_cast<unsigned>(character - 'a' + 10);
                else if (character >= 'A' && character <= 'F') codePoint += static_cast<unsigned>(character - 'A' + 10);
                else return std::nullopt;
            }
            index += 4;
            if (codePoint >= 0xd800u && codePoint <= 0xdbffu &&
                index + 6 < value.size() && value[index + 1] == '\\' && value[index + 2] == 'u') {
                std::uint32_t low = 0;
                for (std::size_t digit = 3; digit <= 6; ++digit) {
                    const char character = value[index + digit];
                    low <<= 4;
                    if (character >= '0' && character <= '9') low += static_cast<unsigned>(character - '0');
                    else if (character >= 'a' && character <= 'f') low += static_cast<unsigned>(character - 'a' + 10);
                    else if (character >= 'A' && character <= 'F') low += static_cast<unsigned>(character - 'A' + 10);
                    else return std::nullopt;
                }
                if (low < 0xdc00u || low > 0xdfffu) return std::nullopt;
                codePoint = 0x10000u + ((codePoint - 0xd800u) << 10) + (low - 0xdc00u);
                index += 6;
            } else if (codePoint >= 0xd800u && codePoint <= 0xdfffu) {
                return std::nullopt;
            }
            AppendUtf8(result, codePoint);
            break;
        }
        default: return std::nullopt;
        }
    }
    return result;
}

std::wstring RemoveBracketedSuffix(std::wstring value) {
    value = Trim(std::move(value));
    for (const auto marker : {L" (", L" [", L" {"}) {
        const auto position = value.find(marker);
        if (position != std::wstring::npos && position > 0) value.resize(position);
    }
    return Trim(std::move(value));
}

std::vector<std::wstring> QueryVariants(std::wstring value) {
    std::vector<std::wstring> result;
    const auto add = [&result](std::wstring candidate) {
        candidate = Trim(std::move(candidate));
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

std::optional<std::size_t> JsonFieldValueStart(std::string_view json, std::string_view field) {
    for (std::size_t index = 0; index < json.size();) {
        if (json[index] != '"') {
            ++index;
            continue;
        }
        const auto keyStart = index;
        bool escaped = false;
        ++index;
        while (index < json.size()) {
            if (!escaped && json[index] == '"') break;
            if (!escaped && json[index] == '\\') escaped = true;
            else escaped = false;
            ++index;
        }
        if (index >= json.size()) return std::nullopt;
        const auto key = UnescapeJson(json.substr(keyStart + 1, index - keyStart - 1));
        ++index;
        const auto colon = json.find(':', index);
        if (colon == std::string_view::npos) return std::nullopt;
        const auto valueStart = json.find_first_not_of(" \t\r\n", colon + 1);
        if (key && *key == field && valueStart != std::string_view::npos) return valueStart;
        index = valueStart == std::string_view::npos ? json.size() : valueStart;
        if (index < json.size() && json[index] == '"') {
            escaped = false;
            ++index;
            while (index < json.size()) {
                if (!escaped && json[index] == '"') {
                    ++index;
                    break;
                }
                if (!escaped && json[index] == '\\') escaped = true;
                else escaped = false;
                ++index;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> JsonStringAt(std::string_view json, const std::size_t valueStart) {
    if (valueStart >= json.size() || json[valueStart] != '"') return std::nullopt;
    std::string value;
    bool escaped = false;
    for (std::size_t index = valueStart + 1; index < json.size(); ++index) {
        const char character = json[index];
        if (!escaped && character == '"') return UnescapeJson(value);
        if (!escaped && character == '\\') {
            escaped = true;
            value.push_back(character);
            continue;
        }
        value.push_back(character);
        escaped = false;
    }
    return std::nullopt;
}

std::optional<std::string> JsonString(std::string_view json, std::string_view field) {
    const auto valueStart = JsonFieldValueStart(json, field);
    return valueStart && json.substr(*valueStart, 4) != "null"
               ? JsonStringAt(json, *valueStart)
               : std::nullopt;
}

std::optional<double> JsonNumber(std::string_view json, std::string_view field) {
    const auto valueStart = JsonFieldValueStart(json, field);
    if (!valueStart) return std::nullopt;
    const auto valueEnd = json.find_first_of(",}\r\n \t", *valueStart);
    const auto value = json.substr(*valueStart, valueEnd == std::string_view::npos
                                                     ? json.size() - *valueStart
                                                     : valueEnd - *valueStart);
    double number{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    return error == std::errc{} && end == value.data() + value.size() && std::isfinite(number)
               ? std::optional<double>(number)
               : std::nullopt;
}

std::vector<std::string_view> JsonArrayObjects(std::string_view json, std::string_view field) {
    std::size_t arrayStart = std::string_view::npos;
    if (field.empty()) {
        arrayStart = json.find('[');
    } else {
        const std::string needle = "\"" + std::string(field) + "\"";
        const auto fieldStart = json.find(needle);
        if (fieldStart == std::string_view::npos) return {};
        const auto colon = json.find(':', fieldStart + needle.size());
        arrayStart = colon == std::string_view::npos ? std::string_view::npos
                                                       : json.find('[', colon + 1);
    }
    if (arrayStart == std::string_view::npos) return {};
    std::vector<std::string_view> objects;
    std::size_t objectStart = std::string_view::npos;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t index = arrayStart + 1; index < json.size(); ++index) {
        const char character = json[index];
        if (quoted) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') quoted = false;
            continue;
        }
        if (character == '"') {
            quoted = true;
        } else if (character == '{') {
            if (depth++ == 0) objectStart = index;
        } else if (character == '}' && depth > 0 && --depth == 0 && objectStart != std::string_view::npos) {
            objects.push_back(json.substr(objectStart, index - objectStart + 1));
            objectStart = std::string_view::npos;
        } else if (character == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

std::wstring JsonWideString(std::string_view json, std::string_view field) {
    const auto value = JsonString(json, field);
    return value ? core::Utf8ToWide(*value) : std::wstring{};
}

std::uint64_t HashText(std::wstring_view value, std::uint64_t hash) noexcept {
    constexpr std::uint64_t prime = 1099511628211ull;
    for (const wchar_t character : value) {
        const auto code = static_cast<std::uint32_t>(character);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<unsigned char>((code >> shift) & 0xffu);
            hash *= prime;
        }
    }
    return hash;
}

std::uint64_t RequestFingerprint(std::wstring_view title, std::wstring_view artist,
                                 std::wstring_view album, const double durationSeconds) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ull;
    auto hash = HashText(title, offset);
    hash = HashText(artist, hash);
    hash = HashText(album, hash);
    const auto duration = static_cast<std::uint64_t>(std::max(0.0, durationSeconds) * 100.0);
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= static_cast<unsigned char>((duration >> shift) & 0xffu);
        hash *= 1099511628211ull;
    }
    return hash;
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
        if (body.size() + available > kMaximumResponseBytes) {
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

std::optional<double> ParseNumber(std::wstring_view value) {
    double number{};
    std::wistringstream stream{std::wstring(value)};
    stream >> number;
    return stream && std::isfinite(number) ? std::optional<double>(number) : std::nullopt;
}

} // namespace

std::wstring LyricsDocument::PlainText() const {
    std::wstring result;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) result += L'\n';
        result += lines[i].text;
    }
    return result;
}

LyricsDocument LyricsService::ParseLrc(std::wstring_view text) {
    LyricsDocument document;
    std::wistringstream stream{std::wstring(text)};
    std::wstring row;
    while (std::getline(stream, row)) {
        row = Trim(std::move(row));
        if (row.empty()) continue;
        std::vector<double> timestamps;
        std::size_t position = 0;
        while (position < row.size() && row[position] == L'[') {
            const auto close = row.find(L']', position + 1);
            if (close == std::wstring::npos) break;
            const auto colon = row.find(L':', position + 1);
            if (colon == std::wstring::npos || colon > close) break;
            const auto minutes = ParseNumber(std::wstring_view(row).substr(position + 1, colon - position - 1));
            const auto seconds = ParseNumber(std::wstring_view(row).substr(colon + 1, close - colon - 1));
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
        const auto lyric = Trim(row.substr(position));
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

LyricsDocument LyricsService::ParseLrclibResponse(std::string_view json) {
    LyricsDocument document;
    if (const auto synced = JsonString(json, "syncedLyrics"); synced && !synced->empty()) {
        document = ParseLrc(core::Utf8ToWide(*synced));
    }
    if (document.Empty()) {
        if (const auto plain = JsonString(json, "plainLyrics"); plain && !plain->empty()) {
            document = ParseLrc(core::Utf8ToWide(*plain));
            document.synced = false;
            for (auto& line : document.lines) line.timestampSeconds = -1.0;
        }
    }
    return document;
}

LyricsService::LyricsService(std::filesystem::path cacheDirectory)
    : cacheDirectory_(std::move(cacheDirectory)), worker_([this](std::stop_token stop) { Worker(stop); }) {}

LyricsService::~LyricsService() {
    Shutdown();
}

void LyricsService::Shutdown() {
    worker_.request_stop();
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void LyricsService::SetNotify(Notify notify) {
    std::scoped_lock lock(mutex_);
    notify_ = std::move(notify);
}

void LyricsService::SetCacheEnabled(bool enabled) {
    std::scoped_lock lock(mutex_);
    cacheEnabled_ = enabled;
}

void LyricsService::Request(std::uint64_t trackId, std::wstring title, std::wstring artist,
                            std::wstring album, double durationSeconds) {
    std::scoped_lock lock(mutex_);
    ++generation_;
    pending_ = RequestData{trackId, std::move(title), std::move(artist), std::move(album),
                           durationSeconds, generation_};
    snapshot_ = LyricsSnapshot{};
    snapshot_.trackId = trackId;
    snapshot_.loading = true;
    snapshot_.status = L"Loading lyrics...";
    snapshot_.revision = generation_;
    condition_.notify_one();
}

void LyricsService::Reset() {
    std::scoped_lock lock(mutex_);
    ++generation_;
    pending_.reset();
    snapshot_ = LyricsSnapshot{};
    snapshot_.status = L"No lyrics available";
}

LyricsSnapshot LyricsService::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return snapshot_;
}

std::filesystem::path LyricsService::CachePath(const RequestData& request) const {
    std::wostringstream name;
    name << request.trackId << L'.' << std::hex
         << RequestFingerprint(request.title, request.artist, request.album, request.durationSeconds)
         << L".lyrics";
    return cacheDirectory_ / name.str();
}

std::optional<LyricsDocument> LyricsService::LoadCache(const RequestData& request) const {
    if (cacheDirectory_.empty()) return std::nullopt;
    std::ifstream input(CachePath(request), std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() > kMaximumResponseBytes || bytes.rfind("RIVAN-LYRICS-1\n", 0) != 0) return std::nullopt;
    constexpr std::string_view header = "RIVAN-LYRICS-1\n";
    const auto separator = bytes.find('\n', header.size());
    if (separator == std::string::npos) return std::nullopt;
    LyricsDocument document;
    document.synced = bytes.substr(header.size(), separator - header.size()) == "synced=1";
    const auto wide = core::Utf8ToWide(bytes.substr(separator + 1));
    std::wistringstream stream{wide};
    std::wstring row;
    while (std::getline(stream, row)) {
        const auto tab = row.find(L'\t');
        if (tab == std::wstring::npos) continue;
        const auto timestamp = ParseNumber(row.substr(0, tab));
        if (timestamp) document.lines.push_back({*timestamp, row.substr(tab + 1)});
    }
    return document.Empty() ? std::nullopt : std::optional<LyricsDocument>(std::move(document));
}

void LyricsService::SaveCache(const RequestData& request, const LyricsDocument& document) const {
    if (cacheDirectory_.empty() || document.Empty()) return;
    std::error_code error;
    std::filesystem::create_directories(cacheDirectory_, error);
    if (error) return;
    const auto cachePath = CachePath(request);
    const auto temporary = cachePath.wstring() + L".partial";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    std::wstring content = L"RIVAN-LYRICS-1\n";
    content += document.synced ? L"synced=1\n" : L"synced=0\n";
    for (const auto& line : document.lines) {
        content += std::to_wstring(line.timestampSeconds) + L'\t' + line.text + L'\n';
    }
    const auto utf8 = core::WideToUtf8(content);
    output.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    output.close();
    if (output) {
        std::filesystem::remove(cachePath, error);
        error.clear();
        std::filesystem::rename(temporary, cachePath, error);
    }
    if (error) std::filesystem::remove(temporary, error);
}

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
        int bestScore = std::numeric_limits<int>::min();
        bool ambiguousBest = false;
        LyricsDocument best;
        for (const auto object : JsonArrayObjects(response->body, "")) {
            const auto candidateTitle = JsonWideString(object, "trackName");
            const auto candidateArtist = JsonWideString(object, "artistName");
            const auto candidateAlbum = JsonWideString(object, "albumName");
            if (NormalizeSearchText(candidateTitle) != requestedTitle) continue;
            if (!requestedArtist.empty() && NormalizeSearchText(candidateArtist) != requestedArtist) continue;
            const auto candidateDuration = JsonNumber(object, "duration");
            int score = 100;
            if (!requestedArtist.empty()) score += 100;
            if (!requestedAlbum.empty() && NormalizeSearchText(candidateAlbum) == requestedAlbum) score += 30;
            if (duration > 0.0 && candidateDuration) {
                const auto difference = std::abs(*candidateDuration - duration);
                if (difference > 8.0) continue;
                score += difference < 1.5 ? 30 : difference < 4.0 ? 15 : 0;
            }
            auto document = ParseLrclibResponse(object);
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
            if (const auto lyrics = JsonString(response->body, "lyrics"); lyrics && !lyrics->empty()) {
            auto document = ParseLrc(core::Utf8ToWide(*lyrics));
            document.synced = false;
            for (auto& line : document.lines) line.timestampSeconds = -1.0;
            if (!document.Empty()) return document;
            }
        }
    }
    return std::nullopt;
}

void LyricsService::Publish(const RequestData& request, LyricsDocument document, std::wstring status) {
    Notify notify;
    {
        std::scoped_lock lock(mutex_);
        if (request.generation != generation_) return;
        snapshot_.trackId = request.trackId;
        snapshot_.loading = false;
        snapshot_.available = !document.Empty();
        snapshot_.document = std::move(document);
        snapshot_.status = std::move(status);
        snapshot_.revision = request.generation;
        notify = notify_;
    }
    if (notify) notify();
}

void LyricsService::Worker(std::stop_token stop) {
    while (!stop.stop_requested()) {
        RequestData request;
        bool useCache = false;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, stop, [this] { return pending_.has_value(); });
            if (stop.stop_requested()) return;
            request = std::move(*pending_);
            pending_.reset();
            useCache = cacheEnabled_;
        }
        if (useCache) {
            if (auto document = LoadCache(request)) {
                Publish(request, std::move(*document), L"Cached lyrics");
                continue;
            }
        }
        auto document = Fetch(request, stop);
        bool saveCache = false;
        {
            std::scoped_lock lock(mutex_);
            saveCache = cacheEnabled_;
        }
        if (document && saveCache) SaveCache(request, *document);
        Publish(request, document ? std::move(*document) : LyricsDocument{},
                document ? L"Lyrics" : L"No lyrics available");
    }
}

} // namespace rivan::lyrics
