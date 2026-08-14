// UpdateService.cpp
#include "UpdateService.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <limits>
#include <string_view>
#include <utility>

namespace rivan::update {
namespace {

constexpr wchar_t kGithubHost[] = L"api.github.com";
constexpr wchar_t kLatestReleasePath[] = L"/repos/gyatstian/Rivan/releases/latest";
constexpr wchar_t kRequestHeaders[] = L"Accept: application/vnd.github+json\r\n";
constexpr DWORD kRequestTimeoutMilliseconds = 4000;
constexpr std::size_t kMaximumResponseBytes = 64U * 1024U;
constexpr auto kRequestBudget = std::chrono::seconds{12};
constexpr std::string_view kReleaseUrlPrefix{
    "https://github.com/gyatstian/Rivan/releases/"};

[[nodiscard]] std::wstring_view WithoutOneVersionPrefix(std::wstring_view version) noexcept {
    if (!version.empty() && (version.front() == L'v' || version.front() == L'V')) {
        version.remove_prefix(1);
    }
    return version;
}

[[nodiscard]] bool AppendUtf8CodePoint(std::string& output, const std::uint32_t codePoint) {
    if (codePoint <= 0x7fU) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else if (codePoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else if (codePoint <= 0x10ffffU) {
        output.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else {
        return false;
    }
    return true;
}

class JsonReader final {
public:
    explicit JsonReader(const std::string_view text) : text_(text) {}

    [[nodiscard]] bool ParseLatestRelease(std::string& tagName, std::string& htmlUrl) {
        SkipWhitespace();
        if (!Consume('{')) return false;
        SkipWhitespace();
        if (Consume('}')) return false;

        bool hasTagName = false;
        bool hasHtmlUrl = false;
        for (;;) {
            std::string key;
            if (!ParseString(key)) return false;
            SkipWhitespace();
            if (!Consume(':')) return false;
            SkipWhitespace();

            if (key == "tag_name") {
                if (hasTagName || !ParseString(tagName)) return false;
                hasTagName = true;
            } else if (key == "html_url") {
                if (hasHtmlUrl || !ParseString(htmlUrl)) return false;
                hasHtmlUrl = true;
            } else if (!SkipValue(0)) {
                return false;
            }

            SkipWhitespace();
            if (Consume('}')) break;
            if (!Consume(',')) return false;
            SkipWhitespace();
        }
        SkipWhitespace();
        return position_ == text_.size() && hasTagName && hasHtmlUrl;
    }

private:
    void SkipWhitespace() noexcept {
        while (position_ < text_.size()) {
            const char value = text_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') return;
            ++position_;
        }
    }

    [[nodiscard]] bool Consume(const char expected) noexcept {
        if (position_ == text_.size() || text_[position_] != expected) return false;
        ++position_;
        return true;
    }

    [[nodiscard]] static int HexValue(const char value) noexcept {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    [[nodiscard]] bool ParseUnicodeEscape(std::uint32_t& value) {
        if (position_ + 4 > text_.size()) return false;
        value = 0;
        for (int index = 0; index < 4; ++index) {
            const int digit = HexValue(text_[position_++]);
            if (digit < 0) return false;
            value = (value << 4U) | static_cast<std::uint32_t>(digit);
        }
        return true;
    }

    [[nodiscard]] bool ParseString(std::string& output) {
        if (!Consume('"')) return false;
        output.clear();
        while (position_ < text_.size()) {
            const unsigned char value = static_cast<unsigned char>(text_[position_++]);
            if (value == '"') return true;
            if (value < 0x20U) return false;
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ == text_.size()) return false;
            switch (text_[position_++]) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t codePoint{};
                if (!ParseUnicodeEscape(codePoint)) return false;
                if (codePoint >= 0xd800U && codePoint <= 0xdbffU) {
                    if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
                        text_[position_ + 1] != 'u') {
                        return false;
                    }
                    position_ += 2;
                    std::uint32_t lowSurrogate{};
                    if (!ParseUnicodeEscape(lowSurrogate) || lowSurrogate < 0xdc00U ||
                        lowSurrogate > 0xdfffU) {
                        return false;
                    }
                    codePoint = 0x10000U + ((codePoint - 0xd800U) << 10U) +
                                (lowSurrogate - 0xdc00U);
                } else if (codePoint >= 0xdc00U && codePoint <= 0xdfffU) {
                    return false;
                }
                if (!AppendUtf8CodePoint(output, codePoint)) return false;
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool SkipLiteral(const std::string_view literal) noexcept {
        if (text_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        return true;
    }

    [[nodiscard]] bool SkipNumber() noexcept {
        const std::size_t start = position_;
        (void)Consume('-');
        if (Consume('0')) {
            // JSON disallows leading zeroes. The next parser stage rejects any trailing digit.
        } else {
            if (position_ == text_.size() || text_[position_] < '1' || text_[position_] > '9') {
                return false;
            }
            do {
                ++position_;
            } while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9');
        }
        if (Consume('.')) {
            const std::size_t fractionStart = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
            if (position_ == fractionStart) return false;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponentStart = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
            if (position_ == exponentStart) return false;
        }
        return position_ != start;
    }

    [[nodiscard]] bool SkipValue(const unsigned depth) {
        if (depth > 32U) return false;
        SkipWhitespace();
        if (position_ == text_.size()) return false;
        switch (text_[position_]) {
        case '"': {
            std::string ignored;
            return ParseString(ignored);
        }
        case '{':
            ++position_;
            SkipWhitespace();
            if (Consume('}')) return true;
            for (;;) {
                std::string ignored;
                if (!ParseString(ignored)) return false;
                SkipWhitespace();
                if (!Consume(':') || !SkipValue(depth + 1U)) return false;
                SkipWhitespace();
                if (Consume('}')) return true;
                if (!Consume(',')) return false;
                SkipWhitespace();
            }
        case '[':
            ++position_;
            SkipWhitespace();
            if (Consume(']')) return true;
            for (;;) {
                if (!SkipValue(depth + 1U)) return false;
                SkipWhitespace();
                if (Consume(']')) return true;
                if (!Consume(',')) return false;
                SkipWhitespace();
            }
        case 't': return SkipLiteral("true");
        case 'f': return SkipLiteral("false");
        case 'n': return SkipLiteral("null");
        default: return SkipNumber();
        }
    }

    std::string_view text_;
    std::size_t position_{};
};

[[nodiscard]] std::optional<std::wstring> Utf8ToWide(const std::string_view value) {
    if (value.empty()) return std::wstring{};
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return std::nullopt;
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return std::nullopt;
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), required) != required) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] bool IsSafeReleaseUrl(const std::string_view url) noexcept {
    if (!url.starts_with(kReleaseUrlPrefix) || url.size() == kReleaseUrlPrefix.size()) return false;
    for (const unsigned char value : url) {
        if (value < 0x21U || value > 0x7eU || value == '%' || value == '\\' ||
            value == '?' || value == '#') {
            return false;
        }
    }
    std::string_view remainder = url.substr(kReleaseUrlPrefix.size());
    while (!remainder.empty()) {
        const std::size_t slash = remainder.find('/');
        const std::string_view segment = remainder.substr(0, slash);
        if (segment.empty() || segment == "." || segment == "..") return false;
        if (slash == std::string_view::npos) break;
        remainder.remove_prefix(slash + 1U);
    }
    return true;
}

} // namespace

UpdateSnapshot::UpdateSnapshot(std::wstring latest, std::wstring url, const bool available)
    : currentVersion(kCurrentVersion),
      latestVersion(std::move(latest)),
      releaseUrl(std::move(url)),
      updateAvailable(available) {}

UpdateService::UpdateService() : snapshot_(std::make_shared<UpdateSnapshot>()) {}

UpdateService::~UpdateService() {
    Shutdown();
}

void UpdateService::StartCheck() {
    std::scoped_lock lock(lifecycleMutex_);
    if (started_ || shuttingDown_) return;
    started_ = true;
    worker_ = std::jthread([this](const std::stop_token stopToken) { Worker(stopToken); });
}

void UpdateService::Shutdown() noexcept {
    std::jthread worker;
    {
        std::scoped_lock lock(lifecycleMutex_);
        shuttingDown_ = true;
        if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id()) {
            worker_.request_stop();
            return;
        }
        worker_.request_stop();
        worker = std::move(worker_);
    }
    {
        std::scoped_lock lock(notifyMutex_);
        notify_ = {};
    }
    // WinHTTP handles use synchronous calls. Let worker leave WinHTTP before closing
    // any handles; closing one from another thread while it is active is unsupported.
    if (worker.joinable()) worker.join();
}

void UpdateService::SetNotify(NotifyCallback callback) {
    std::scoped_lock lock(notifyMutex_);
    notify_ = std::move(callback);
}

std::shared_ptr<const UpdateSnapshot> UpdateService::Snapshot() const {
    std::scoped_lock lock(snapshotMutex_);
    return snapshot_;
}

std::optional<UpdateSnapshot> UpdateService::ParseLatestReleaseJson(const std::string_view json) {
    if (json.empty() || json.size() > kMaximumResponseBytes) return std::nullopt;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, json.data(),
                            static_cast<int>(json.size()), nullptr, 0) <= 0) {
        return std::nullopt;
    }
    std::string tagName;
    std::string htmlUrl;
    JsonReader reader(json);
    if (!reader.ParseLatestRelease(tagName, htmlUrl) || tagName.empty() || !IsSafeReleaseUrl(htmlUrl)) {
        return std::nullopt;
    }
    auto latestVersion = Utf8ToWide(tagName);
    auto releaseUrl = Utf8ToWide(htmlUrl);
    if (!latestVersion || !releaseUrl) return std::nullopt;
    const bool updateAvailable = !VersionsMatch(kCurrentVersion, *latestVersion);
    return UpdateSnapshot(std::move(*latestVersion), std::move(*releaseUrl), updateAvailable);
}

bool UpdateService::VersionsMatch(const std::wstring_view current,
                                  const std::wstring_view latest) noexcept {
    return WithoutOneVersionPrefix(current) == WithoutOneVersionPrefix(latest);
}

void UpdateService::Worker(const std::stop_token stopToken) noexcept {
    try {
        const auto latest = FetchLatestRelease(stopToken);
        CloseRequestHandles();
        if (!latest || stopToken.stop_requested()) return;

        const auto published = std::make_shared<UpdateSnapshot>(*latest);
        {
            std::scoped_lock lock(snapshotMutex_);
            snapshot_ = published;
        }
        if (!published->updateAvailable) return;

        NotifyCallback callback;
        {
            std::scoped_lock lock(notifyMutex_);
            callback = notify_;
        }
        if (callback && !stopToken.stop_requested()) callback();
    } catch (...) {
        // Update availability is optional. A bad response or allocation failure must not
        // terminate the application worker thread.
    }
    CloseRequestHandles();
}

std::optional<UpdateSnapshot> UpdateService::FetchLatestRelease(
    const std::stop_token stopToken) {
    const auto deadline = std::chrono::steady_clock::now() + kRequestBudget;
    const auto withinBudget = [&stopToken, deadline]() noexcept {
        return !stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline;
    };
    const auto publishHandle = [this, stopToken](void*& slot, HINTERNET handle) {
        if (!handle) return false;
        std::scoped_lock lock(requestMutex_);
        if (stopToken.stop_requested()) {
            WinHttpCloseHandle(handle);
            return false;
        }
        slot = handle;
        return true;
    };

    const HINTERNET session = WinHttpOpen(L"Rivan/1.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!publishHandle(session_, session)) return std::nullopt;
    if (!WinHttpSetTimeouts(session, kRequestTimeoutMilliseconds, kRequestTimeoutMilliseconds,
                            kRequestTimeoutMilliseconds, kRequestTimeoutMilliseconds)) {
        return std::nullopt;
    }

    const HINTERNET connection = WinHttpConnect(session, kGithubHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!publishHandle(connection_, connection)) return std::nullopt;
    const HINTERNET request = WinHttpOpenRequest(connection, L"GET", kLatestReleasePath, nullptr,
                                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE);
    if (!publishHandle(request_, request)) return std::nullopt;
    const auto setRemainingTimeout = [request, stopToken, deadline]() {
        if (stopToken.stop_requested()) return false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return false;
        const auto timeout = static_cast<DWORD>(std::min<std::int64_t>(
            remaining, static_cast<std::int64_t>(kRequestTimeoutMilliseconds)));
        return WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout) != FALSE;
    };
    if (!setRemainingTimeout() ||
        !WinHttpSendRequest(request, kRequestHeaders, static_cast<DWORD>(-1L),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !setRemainingTimeout() || !WinHttpReceiveResponse(request, nullptr)) {
        return std::nullopt;
    }

    DWORD statusCode{};
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize,
                             WINHTTP_NO_HEADER_INDEX) ||
        statusCode != 200) {
        return std::nullopt;
    }

    std::string response;
    response.reserve(4096);
    while (withinBudget()) {
        DWORD available{};
        if (!setRemainingTimeout() ||
            !WinHttpQueryDataAvailable(request, &available)) return std::nullopt;
        if (!withinBudget()) return std::nullopt;
        if (available == 0) break;
        if (available > kMaximumResponseBytes - response.size()) return std::nullopt;
        const std::size_t priorSize = response.size();
        response.resize(priorSize + available);
        DWORD read{};
        if (!setRemainingTimeout() ||
            !WinHttpReadData(request, response.data() + priorSize, available, &read) ||
            read > available) {
            return std::nullopt;
        }
        response.resize(priorSize + read);
        if (read == 0) break;
    }
    if (!withinBudget()) return std::nullopt;
    return ParseLatestReleaseJson(response);
}

void UpdateService::CloseRequestHandles() noexcept {
    HINTERNET request{};
    HINTERNET connection{};
    HINTERNET session{};
    {
        std::scoped_lock lock(requestMutex_);
        request = static_cast<HINTERNET>(request_);
        connection = static_cast<HINTERNET>(connection_);
        session = static_cast<HINTERNET>(session_);
        request_ = nullptr;
        connection_ = nullptr;
        session_ = nullptr;
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
}

} // namespace rivan::update
