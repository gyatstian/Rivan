// PlaybackQueue.cpp
// Maintains resilient playback traversal over a mutable filesystem queue.
#include "PlaybackQueue.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <numeric>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace rivan::playlist {
namespace {

std::wstring PathKey(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        normalized = path.lexically_normal();
    }
    auto value = normalized.generic_wstring();
#ifdef _WIN32
    for (auto& character : value) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
#endif
    return value;
}

} // namespace

PlaybackQueue::PlaybackQueue()
    : random_(std::random_device{}()) {}

PlaybackQueue::PlaybackQueue(std::uint32_t randomSeed)
    : random_(randomSeed) {}

void PlaybackQueue::SetTracks(std::vector<library::Track> tracks,
                              std::optional<std::size_t> startIndex) {
    tracks_ = std::move(tracks);
    currentIndex_ = NoPosition;
    orderPosition_ = NoPosition;
    history_.clear();
    historyPosition_ = NoPosition;
    BuildOrder();

    if (startIndex && *startIndex < tracks_.size()) {
        (void)Play(*startIndex);
    }
}

void PlaybackQueue::AppendTracks(std::span<const library::Track> tracks) {
    const auto firstNewIndex = tracks_.size();
    tracks_.insert(tracks_.end(), tracks.begin(), tracks.end());
    if (!shuffle_) {
        for (std::size_t index = firstNewIndex; index < tracks_.size(); ++index) {
            order_.push_back(index);
        }
        return;
    }

    std::vector<std::size_t> additions(tracks_.size() - firstNewIndex);
    std::iota(additions.begin(), additions.end(), firstNewIndex);
    std::shuffle(additions.begin(), additions.end(), random_);
    const auto insertion = orderPosition_ == NoPosition
        ? order_.begin()
        : order_.begin() + static_cast<std::ptrdiff_t>(orderPosition_ + 1);
    order_.insert(insertion, additions.begin(), additions.end());
    LocateCurrentInOrder();
}

std::size_t PlaybackQueue::AppendDroppedFiles(
    std::span<const std::filesystem::path> paths) {
    std::vector<std::filesystem::path> files;
    std::unordered_set<std::wstring> seen;

    const auto addFile = [&files, &seen](const std::filesystem::path& path) {
        std::error_code ec;
        if (library::Track::IsSupportedFile(path) &&
            std::filesystem::is_regular_file(path, ec) && !ec &&
            seen.insert(PathKey(path)).second) {
            files.push_back(path);
        }
    };

    for (const auto& path : paths) {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec) && !ec) {
            std::filesystem::recursive_directory_iterator iterator(
                path, std::filesystem::directory_options::skip_permission_denied, ec);
            const std::filesystem::recursive_directory_iterator end;
            while (!ec && iterator != end) {
                addFile(iterator->path());
                iterator.increment(ec);
            }
        } else {
            addFile(path);
        }
    }

    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return PathKey(left) < PathKey(right);
    });
    std::vector<library::Track> imported;
    imported.reserve(files.size());
    for (const auto& file : files) {
        imported.push_back(library::Track::FromFile(file));
    }
    AppendTracks(imported);
    return imported.size();
}

void PlaybackQueue::Clear() noexcept {
    tracks_.clear();
    order_.clear();
    history_.clear();
    currentIndex_ = NoPosition;
    orderPosition_ = NoPosition;
    historyPosition_ = NoPosition;
}

bool PlaybackQueue::Empty() const noexcept {
    return tracks_.empty();
}

std::size_t PlaybackQueue::Size() const noexcept {
    return tracks_.size();
}

const std::vector<library::Track>& PlaybackQueue::Tracks() const noexcept {
    return tracks_;
}

const library::Track* PlaybackQueue::Current() const noexcept {
    return IsPlayable(currentIndex_) ? &tracks_[currentIndex_] : nullptr;
}

QueueNavigation PlaybackQueue::Play(std::size_t index) {
    if (index >= tracks_.size()) {
        return Exhausted();
    }
    if (IsPlayable(index)) {
        return Activate(index, QueueNavigationAction::Advanced, true);
    }

    const auto position = std::find(order_.begin(), order_.end(), index);
    if (position != order_.end()) {
        for (auto candidate = std::next(position); candidate != order_.end(); ++candidate) {
            if (IsPlayable(*candidate)) {
                return Activate(*candidate, QueueNavigationAction::Advanced, true);
            }
        }
    }
    return Exhausted();
}

QueueNavigation PlaybackQueue::Start() {
    for (const auto index : order_) {
        if (IsPlayable(index)) {
            return Activate(index, QueueNavigationAction::Advanced, true);
        }
    }
    return Exhausted();
}

QueueNavigation PlaybackQueue::Next() {
    if (tracks_.empty()) {
        return {QueueNavigationAction::Empty, nullptr};
    }
    if (currentIndex_ == NoPosition) {
        return Start();
    }

    if (historyPosition_ != NoPosition && historyPosition_ + 1 < history_.size()) {
        for (auto position = historyPosition_ + 1; position < history_.size(); ++position) {
            if (IsPlayable(history_[position])) {
                historyPosition_ = position;
                currentIndex_ = history_[position];
                LocateCurrentInOrder();
                return {QueueNavigationAction::Advanced, &tracks_[currentIndex_]};
            }
        }
        history_.resize(historyPosition_ + 1);
    }

    const auto start = orderPosition_ == NoPosition ? 0 : orderPosition_ + 1;
    for (auto position = start; position < order_.size(); ++position) {
        if (IsPlayable(order_[position])) {
            return Activate(order_[position], QueueNavigationAction::Advanced, true);
        }
    }

    if (repeat_ == RepeatMode::All) {
        const auto stop = orderPosition_ == NoPosition ? order_.size() : orderPosition_ + 1;
        for (std::size_t position = 0; position < stop; ++position) {
            if (IsPlayable(order_[position])) {
                return Activate(order_[position], QueueNavigationAction::Advanced, true);
            }
        }
    }
    return Exhausted();
}

QueueNavigation PlaybackQueue::Previous() {
    if (tracks_.empty()) {
        return {QueueNavigationAction::Empty, nullptr};
    }
    if (historyPosition_ == NoPosition || historyPosition_ == 0) {
        return Exhausted();
    }

    auto position = historyPosition_;
    while (position > 0) {
        --position;
        const auto index = history_[position];
        if (IsPlayable(index)) {
            historyPosition_ = position;
            currentIndex_ = index;
            LocateCurrentInOrder();
            return {QueueNavigationAction::Advanced, &tracks_[index]};
        }
    }
    return Exhausted();
}

QueueNavigation PlaybackQueue::OnEndOfStream() {
    if (Current() != nullptr && repeat_ == RepeatMode::One) {
        return {QueueNavigationAction::Restarted, Current()};
    }
    return Next();
}

void PlaybackQueue::SetShuffle(bool enabled) {
    if (shuffle_ == enabled) {
        return;
    }
    shuffle_ = enabled;
    BuildOrder();
}

bool PlaybackQueue::Shuffle() const noexcept {
    return shuffle_;
}

void PlaybackQueue::SetRepeat(RepeatMode mode) noexcept {
    repeat_ = mode;
}

RepeatMode PlaybackQueue::Repeat() const noexcept {
    return repeat_;
}

bool PlaybackQueue::IsPlayable(std::size_t index) const noexcept {
    return index < tracks_.size() && tracks_[index].IsAvailable();
}

QueueNavigation PlaybackQueue::Activate(std::size_t index,
                                         QueueNavigationAction action,
                                         bool recordHistory) {
    currentIndex_ = index;
    LocateCurrentInOrder();
    if (recordHistory) {
        if (historyPosition_ != NoPosition && historyPosition_ + 1 < history_.size()) {
            history_.resize(historyPosition_ + 1);
        }
        if (history_.empty() || history_.back() != index) {
            history_.push_back(index);
        }
        historyPosition_ = history_.empty() ? NoPosition : history_.size() - 1;
    }
    return {action, &tracks_[index]};
}

QueueNavigation PlaybackQueue::Exhausted() const noexcept {
    return {tracks_.empty() ? QueueNavigationAction::Empty : QueueNavigationAction::Exhausted,
            nullptr};
}

void PlaybackQueue::BuildOrder() {
    order_.resize(tracks_.size());
    std::iota(order_.begin(), order_.end(), std::size_t{0});
    if (shuffle_ && !order_.empty()) {
        if (currentIndex_ < tracks_.size()) {
            std::vector<std::size_t> prefix;
            std::unordered_set<std::size_t> visited;
            if (historyPosition_ != NoPosition) {
                const auto end = std::min(historyPosition_ + 1, history_.size());
                for (std::size_t position = 0; position < end; ++position) {
                    if (history_[position] < tracks_.size() && visited.insert(history_[position]).second) {
                        prefix.push_back(history_[position]);
                    }
                }
            }
            if (visited.insert(currentIndex_).second) {
                prefix.push_back(currentIndex_);
            }

            std::vector<std::size_t> remaining;
            for (const auto index : order_) {
                if (!visited.contains(index)) {
                    remaining.push_back(index);
                }
            }
            std::shuffle(remaining.begin(), remaining.end(), random_);
            order_ = std::move(prefix);
            order_.insert(order_.end(), remaining.begin(), remaining.end());
        } else {
            std::shuffle(order_.begin(), order_.end(), random_);
        }
    }
    LocateCurrentInOrder();
}

void PlaybackQueue::LocateCurrentInOrder() noexcept {
    if (currentIndex_ >= tracks_.size()) {
        orderPosition_ = NoPosition;
        return;
    }
    const auto position = std::find(order_.begin(), order_.end(), currentIndex_);
    orderPosition_ = position == order_.end()
        ? NoPosition
        : static_cast<std::size_t>(std::distance(order_.begin(), position));
}

} // namespace rivan::playlist
