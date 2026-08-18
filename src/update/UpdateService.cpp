// UpdateService.cpp
#include "UpdateService.h"

#include "../core/Json.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

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
    // Shared-parser semantic: duplicate keys resolve first-wins. The former streaming
    // reader rejected duplicates outright, which no release payload relied on.
    const auto root = core::ParseJson(json);
    if (!root || root->kind != core::JsonValue::Kind::Object) return std::nullopt;
    const auto* tag = core::JsonMember(*root, "tag_name");
    if (!tag || tag->kind != core::JsonValue::Kind::String || tag->string.empty()) {
        return std::nullopt;
    }
    const auto* url = core::JsonMember(*root, "html_url");
    if (!url || url->kind != core::JsonValue::Kind::String || !IsSafeReleaseUrl(url->string)) {
        return std::nullopt;
    }
    auto latestVersion = Utf8ToWide(tag->string);
    auto releaseUrl = Utf8ToWide(url->string);
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

    const HINTERNET session = WinHttpOpen(L"Rivan/1.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
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
