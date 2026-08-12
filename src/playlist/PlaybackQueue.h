// PlaybackQueue.h
// Playback order, history, repeat, shuffle, and dropped-file navigation.
#pragma once

#include "../library/Track.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <span>
#include <vector>

namespace rivan::playlist {

enum class RepeatMode : std::uint8_t { Off, All, One };
enum class QueueNavigationAction : std::uint8_t { Empty, Advanced, Restarted, Exhausted };

struct QueueNavigation final {
    QueueNavigationAction action{QueueNavigationAction::Empty};
    const library::Track* track{};

    [[nodiscard]] explicit operator bool() const noexcept { return track != nullptr; }
};

class PlaybackQueue final {
public:
    PlaybackQueue();
    explicit PlaybackQueue(std::uint32_t randomSeed);

    void SetTracks(std::vector<library::Track> tracks,
                   std::optional<std::size_t> startIndex = std::nullopt);
    void AppendTracks(std::span<const library::Track> tracks);
    [[nodiscard]] std::size_t AppendDroppedFiles(
        std::span<const std::filesystem::path> paths);
    void Clear() noexcept;

    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] const std::vector<library::Track>& Tracks() const noexcept;
    [[nodiscard]] const library::Track* Current() const noexcept;

    [[nodiscard]] QueueNavigation Play(std::size_t index);
    [[nodiscard]] QueueNavigation Start();
    [[nodiscard]] QueueNavigation Next();
    [[nodiscard]] QueueNavigation Previous();
    [[nodiscard]] QueueNavigation OnEndOfStream();

    void SetShuffle(bool enabled);
    [[nodiscard]] bool Shuffle() const noexcept;
    void SetRepeat(RepeatMode mode) noexcept;
    [[nodiscard]] RepeatMode Repeat() const noexcept;

private:
    static constexpr std::size_t NoPosition = static_cast<std::size_t>(-1);

    [[nodiscard]] bool IsPlayable(std::size_t index) const noexcept;
    [[nodiscard]] QueueNavigation Activate(std::size_t index,
                                           QueueNavigationAction action,
                                           bool recordHistory);
    [[nodiscard]] QueueNavigation Exhausted() const noexcept;
    void BuildOrder();
    void LocateCurrentInOrder() noexcept;

    std::vector<library::Track> tracks_;
    std::vector<std::size_t> order_;
    std::size_t orderPosition_{NoPosition};
    std::size_t currentIndex_{NoPosition};
    std::vector<std::size_t> history_;
    std::size_t historyPosition_{NoPosition};
    bool shuffle_{};
    RepeatMode repeat_{RepeatMode::Off};
    std::mt19937 random_;
};

} // namespace rivan::playlist
