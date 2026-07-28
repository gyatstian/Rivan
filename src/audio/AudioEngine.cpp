// Rivan command-driven native audio engine.
// The worker thread owns the command/render path; the backend may run decoder work separately.
#include "AudioEngine.h"

#include "NativeAudioBackend.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "avrt.lib")

namespace rivan::audio {
namespace {

enum class CommandType {
    Load,
    Play,
    Pause,
    Stop,
    Seek,
    SetVolume,
    Shutdown,
};

struct Command {
    CommandType type{};
    std::filesystem::path file;
    std::chrono::nanoseconds position{};
    float volume{};
};

[[nodiscard]] float NormalizeVolume(const float volume) noexcept {
    return std::isfinite(volume) ? std::clamp(volume, 0.0F, 1.0F) : 0.0F;
}

[[nodiscard]] std::int64_t NativeCode(const HRESULT result) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint32_t>(result));
}

[[nodiscard]] std::string Win32FailureMessage(const DWORD error, const char* operation) {
    std::ostringstream text;
    text << operation << " failed (Win32 error " << error << ')';
    return text.str();
}

} // namespace

class AudioEngine::Impl final {
private:
    struct SharedState final {
        explicit SharedState(AudioEngineOptions engineOptions)
            : options(std::move(engineOptions)) {
            commandEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (commandEvent == nullptr) {
                const auto error = GetLastError();
                throw std::system_error(static_cast<int>(error), std::system_category(),
                                        "CreateEventW(audio commands)");
            }
        }

        ~SharedState() {
            if (commandEvent != nullptr) {
                CloseHandle(commandEvent);
            }
        }

        SharedState(const SharedState&) = delete;
        SharedState& operator=(const SharedState&) = delete;

        AudioEngineOptions options;
        AudioAnalysisBuffer analysis;
        HANDLE commandEvent{};

        std::mutex commandMutex;
        std::deque<Command> commands;
        std::atomic<bool> acceptingCommands{true};
        std::atomic<bool> shutdownQueued{false};

        mutable std::mutex statusMutex;
        AudioStatus status;

        // Published lock-free for UI paints; full status still under statusMutex.
        std::atomic<std::int64_t> livePositionNs{0};
        std::atomic<std::int64_t> liveDurationNs{0};
        std::atomic<float> liveVolume{1.0F};
        std::atomic<std::uint8_t> liveState{
            static_cast<std::uint8_t>(PlaybackState::Initializing)};
        std::atomic<bool> liveHasMedia{false};

        mutable std::mutex callbackMutex;
        EventCallback callback;
    };

public:
    explicit Impl(AudioEngineOptions options)
        : state_(std::make_shared<SharedState>(std::move(options))),
          worker_([state = state_] { WorkerMain(std::move(state)); }) {}

    ~Impl() {
        QueueShutdown();
        if (!worker_.joinable()) {
            return;
        }

        // A callback is allowed to release the engine. The shared worker state outlives
        // Impl, so detaching here avoids a self-join without allowing use-after-free.
        if (worker_.get_id() == std::this_thread::get_id()) {
            worker_.detach();
        } else {
            worker_.join();
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void Enqueue(Command command) {
        const auto state = state_;
        if (!state->acceptingCommands.load(std::memory_order_acquire)) {
            return;
        }
        {
            std::scoped_lock lock(state->commandMutex);
            if (!state->acceptingCommands.load(std::memory_order_relaxed)) {
                return;
            }
            if (command.type == CommandType::Seek) {
                // Slider drags can generate hundreds of seeks. Only newest unprocessed target
                // matters; retaining each one stalls both audio and video behind old positions.
                for (auto iterator = state->commands.rbegin(); iterator != state->commands.rend();
                     ++iterator) {
                    if (iterator->type == CommandType::Seek) {
                        iterator->position = command.position;
                        SetEvent(state->commandEvent);
                        return;
                    }
                    if (iterator->type == CommandType::Load ||
                        iterator->type == CommandType::Shutdown) {
                        break;
                    }
                }
            }
            state->commands.push_back(std::move(command));
        }
        SetEvent(state->commandEvent);
    }

    [[nodiscard]] AudioStatus Status() const {
        std::scoped_lock lock(state_->statusMutex);
        return state_->status;
    }

    [[nodiscard]] LiveTransport Live() const noexcept {
        LiveTransport live;
        live.position = std::chrono::nanoseconds{
            state_->livePositionNs.load(std::memory_order_relaxed)};
        live.duration = std::chrono::nanoseconds{
            state_->liveDurationNs.load(std::memory_order_relaxed)};
        live.volume = state_->liveVolume.load(std::memory_order_relaxed);
        live.state = static_cast<PlaybackState>(
            state_->liveState.load(std::memory_order_relaxed));
        live.hasMedia = state_->liveHasMedia.load(std::memory_order_relaxed);
        return live;
    }

    [[nodiscard]] AudioAnalysisSnapshot Analysis(const std::size_t maximumFrames) const {
        return state_->analysis.Latest(maximumFrames);
    }

    void AnalysisInto(AudioAnalysisSnapshot& out, const std::size_t maximumFrames) const {
        state_->analysis.LatestInto(out, maximumFrames);
    }

    [[nodiscard]] std::uint64_t AnalysisGeneration() const noexcept {
        return state_->analysis.Generation();
    }

    void SetEventCallback(EventCallback callback) {
        std::scoped_lock lock(state_->callbackMutex);
        state_->callback = std::move(callback);
    }

private:
    void QueueShutdown() noexcept {
        const auto state = state_;
        state->acceptingCommands.store(false, std::memory_order_release);
        if (state->shutdownQueued.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            std::scoped_lock lock(state->commandMutex);
            state->commands.push_back(Command{CommandType::Shutdown});
        } catch (...) {
            // Allocation failure cannot prevent shutdown: the worker also observes this flag.
        }
        SetEvent(state->commandEvent);
    }

    static void Emit(const std::shared_ptr<SharedState>& state,
                     const AudioEventType type) noexcept {
        try {
            EventCallback callback;
            {
                std::scoped_lock lock(state->callbackMutex);
                callback = state->callback;
            }
            if (!callback) {
                return;
            }

            AudioEvent event;
            event.type = type;
            {
                std::scoped_lock lock(state->statusMutex);
                event.status = state->status;
            }
            try {
                callback(event);
            } catch (...) {
                // User callbacks cannot terminate the real-time audio worker.
            }
        } catch (...) {
            // Status/callback copies can allocate; audio processing must remain alive.
        }
    }

    static void PublishLive(const std::shared_ptr<SharedState>& state,
                            const AudioStatus& status) noexcept {
        state->livePositionNs.store(status.position.count(), std::memory_order_relaxed);
        state->liveDurationNs.store(status.duration.count(), std::memory_order_relaxed);
        state->liveVolume.store(status.volume, std::memory_order_relaxed);
        state->liveState.store(static_cast<std::uint8_t>(status.state),
                               std::memory_order_relaxed);
        state->liveHasMedia.store(status.hasMedia, std::memory_order_relaxed);
    }

    static void SetState(const std::shared_ptr<SharedState>& state,
                         PlaybackState& current,
                         const PlaybackState next,
                         const bool emit = true) {
        current = next;
        {
            std::scoped_lock lock(state->statusMutex);
            state->status.state = next;
            PublishLive(state, state->status);
        }
        if (emit) {
            Emit(state, AudioEventType::StateChanged);
        }
    }

    static void UpdatePosition(const std::shared_ptr<SharedState>& state,
                               const detail::NativeAudioBackend& backend,
                               const bool force = false) {
        // UI only paints ~20 Hz; skip most WASAPI-period status mutex writes.
        static thread_local std::chrono::steady_clock::time_point lastPublish{};
        const auto now = std::chrono::steady_clock::now();
        if (!force && lastPublish.time_since_epoch().count() != 0 &&
            now - lastPublish < std::chrono::milliseconds{40}) {
            return;
        }
        lastPublish = now;

        auto position = backend.PlaybackElapsed();
        const auto duration = backend.Duration();
        if (duration.count() > 0) {
            position = (std::min)(position, duration);
        }
        position = (std::max)(position, std::chrono::nanoseconds{0});
        std::scoped_lock lock(state->statusMutex);
        state->status.position = position;
        state->status.duration = duration;
        PublishLive(state, state->status);
    }

    static void RecordError(const std::shared_ptr<SharedState>& state,
                            PlaybackState& current,
                            detail::NativeAudioBackend& backend,
                            const std::int64_t code,
                            std::string message) noexcept {
        backend.Close();
        current = PlaybackState::Error;
        try {
            std::scoped_lock lock(state->statusMutex);
            state->status.state = PlaybackState::Error;
            state->status.position = {};
            state->status.duration = {};
            state->status.hasMedia = false;
            state->status.error.nativeCode = code;
            state->status.error.message = std::move(message);
            PublishLive(state, state->status);
        } catch (...) {
            // Preserve worker liveness even if assigning the diagnostic text fails.
        }
        Emit(state, AudioEventType::Error);
        Emit(state, AudioEventType::StateChanged);
    }

    static void RecordCurrentException(const std::shared_ptr<SharedState>& state,
                                       PlaybackState& current,
                                       detail::NativeAudioBackend& backend) noexcept {
        try {
            throw;
        } catch (const detail::NativeAudioException& exception) {
            RecordError(state, current, backend, exception.Code(), exception.what());
        } catch (const std::exception& exception) {
            RecordError(state, current, backend, NativeCode(E_FAIL), exception.what());
        } catch (...) {
            RecordError(state, current, backend, NativeCode(E_FAIL),
                        "Unknown native audio failure");
        }
    }

    static std::vector<Command> TakeCommands(const std::shared_ptr<SharedState>& state) {
        std::vector<Command> result;
        std::scoped_lock lock(state->commandMutex);
        result.reserve(state->commands.size());
        while (!state->commands.empty()) {
            result.push_back(std::move(state->commands.front()));
            state->commands.pop_front();
        }
        return result;
    }

    static bool FinishIfDrained(const std::shared_ptr<SharedState>& state,
                                PlaybackState& current,
                                detail::NativeAudioBackend& backend,
                                const detail::RenderResult result) {
        if (!result.drained) {
            return false;
        }

        backend.Pause();
        current = PlaybackState::Ended;
        {
            std::scoped_lock lock(state->statusMutex);
            state->status.state = PlaybackState::Ended;
            const auto duration = backend.Duration();
            state->status.position = duration.count() > 0
                                          ? duration
                                          : backend.PlaybackElapsed();
            PublishLive(state, state->status);
        }
        Emit(state, AudioEventType::EndOfStream);
        Emit(state, AudioEventType::StateChanged);
        return true;
    }

    static void HandleLoad(const std::shared_ptr<SharedState>& state,
                           PlaybackState& current,
                           detail::NativeAudioBackend& backend,
                           bool& initialized,
                           const std::filesystem::path& file) {
        backend.Close();
        current = PlaybackState::Loading;
        {
            std::scoped_lock lock(state->statusMutex);
            state->status.state = PlaybackState::Loading;
            state->status.source = file;
            state->status.position = {};
            state->status.duration = {};
            state->status.hasMedia = false;
            state->status.error = {};
            PublishLive(state, state->status);
        }
        Emit(state, AudioEventType::StateChanged);

        if (!initialized) {
            backend.Initialize();
            initialized = true;
        }
        backend.Open(file);

        float volume = 1.0F;
        {
            std::scoped_lock lock(state->statusMutex);
            volume = state->status.volume;
        }
        backend.SetVolume(volume);
        backend.PumpDecoded(32);

        current = PlaybackState::Stopped;
        {
            std::scoped_lock lock(state->statusMutex);
            state->status.state = PlaybackState::Stopped;
            state->status.duration = backend.Duration();
            state->status.position = {};
            state->status.hasMedia = true;
            state->status.error = {};
            PublishLive(state, state->status);
        }
        Emit(state, AudioEventType::Loaded);
        Emit(state, AudioEventType::StateChanged);
    }

    static void HandlePlay(const std::shared_ptr<SharedState>& state,
                           PlaybackState& current,
                           detail::NativeAudioBackend& backend) {
        if (!backend.HasMedia() || current == PlaybackState::Playing) {
            return;
        }
        if (current == PlaybackState::Ended) {
            backend.Seek(std::chrono::nanoseconds{0});
        }

        backend.PumpDecoded(32);
        if (FinishIfDrained(state, current, backend, backend.Render())) {
            return;
        }
        backend.Start();
        UpdatePosition(state, backend, true);
        SetState(state, current, PlaybackState::Playing);
    }

    static void HandlePause(const std::shared_ptr<SharedState>& state,
                            PlaybackState& current,
                            detail::NativeAudioBackend& backend) {
        if (!backend.HasMedia() || current != PlaybackState::Playing) {
            return;
        }
        backend.Pause();
        UpdatePosition(state, backend, true);
        SetState(state, current, PlaybackState::Paused);
    }

    static void HandleStop(const std::shared_ptr<SharedState>& state,
                           PlaybackState& current,
                           detail::NativeAudioBackend& backend) {
        if (!backend.HasMedia()) {
            return;
        }
        backend.Seek(std::chrono::nanoseconds{0});
        {
            std::scoped_lock lock(state->statusMutex);
            state->status.position = {};
            PublishLive(state, state->status);
        }
        SetState(state, current, PlaybackState::Stopped);
    }

    static void HandleSeek(const std::shared_ptr<SharedState>& state,
                           PlaybackState& current,
                           detail::NativeAudioBackend& backend,
                           const std::chrono::nanoseconds requestedPosition) {
        if (!backend.HasMedia()) {
            return;
        }

        const bool resume = current == PlaybackState::Playing;
        backend.Seek(requestedPosition);
        backend.PumpDecoded(32);

        if (resume) {
            if (FinishIfDrained(state, current, backend, backend.Render())) {
                return;
            }
            backend.Start();
        } else if (current == PlaybackState::Ended || current == PlaybackState::Error) {
            current = PlaybackState::Stopped;
        }

        UpdatePosition(state, backend, true);
        {
            std::scoped_lock lock(state->statusMutex);
            state->status.state = current;
            PublishLive(state, state->status);
        }
        Emit(state, AudioEventType::StateChanged);
    }

    static bool HandleCommand(const std::shared_ptr<SharedState>& state,
                              PlaybackState& current,
                              detail::NativeAudioBackend& backend,
                              bool& initialized,
                              const Command& command) {
        switch (command.type) {
        case CommandType::Load:
            HandleLoad(state, current, backend, initialized, command.file);
            break;
        case CommandType::Play:
            HandlePlay(state, current, backend);
            break;
        case CommandType::Pause:
            HandlePause(state, current, backend);
            break;
        case CommandType::Stop:
            HandleStop(state, current, backend);
            break;
        case CommandType::Seek:
            HandleSeek(state, current, backend, command.position);
            break;
        case CommandType::SetVolume: {
            const auto volume = NormalizeVolume(command.volume);
            backend.SetVolume(volume);
            std::scoped_lock lock(state->statusMutex);
            state->status.volume = volume;
            PublishLive(state, state->status);
            break;
        }
        case CommandType::Shutdown:
            SetState(state, current, PlaybackState::ShuttingDown);
            return false;
        }
        return true;
    }

    static void WorkerMain(std::shared_ptr<SharedState> state) noexcept {
        // MMCSS Pro Audio reduces preemption of the decode/render path under load.
        DWORD mmcssTaskIndex = 0;
        const HANDLE mmcssHandle =
            AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
        if (mmcssHandle != nullptr) {
            AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_HIGH);
        }

        detail::NativeAudioBackend backend(state->analysis,
                                           state->options.decodedBufferDuration,
                                           state->options.analysisDuration);
        PlaybackState current = PlaybackState::Initializing;
        bool initialized = false;
        bool running = true;

        try {
            backend.Initialize();
            initialized = true;
            SetState(state, current, PlaybackState::Idle);
        } catch (...) {
            RecordCurrentException(state, current, backend);
        }

        while (running) {
            HANDLE handles[2] = {state->commandEvent, nullptr};
            DWORD handleCount = 1;
            if (current == PlaybackState::Playing && backend.RenderEvent() != nullptr) {
                handles[1] = static_cast<HANDLE>(backend.RenderEvent());
                handleCount = 2;
            }

            const DWORD waitResult = WaitForMultipleObjects(handleCount, handles, FALSE, INFINITE);
            if (waitResult == WAIT_FAILED) {
                const auto error = GetLastError();
                RecordError(state, current, backend,
                            NativeCode(HRESULT_FROM_WIN32(error)),
                            Win32FailureMessage(error, "WaitForMultipleObjects(audio)"));
                break;
            }

            if (waitResult == WAIT_OBJECT_0) {
                std::vector<Command> commands;
                try {
                    commands = TakeCommands(state);
                } catch (...) {
                    RecordCurrentException(state, current, backend);
                    continue;
                }

                // If shutdown command allocation failed, the flag still guarantees exit.
                if (commands.empty() && state->shutdownQueued.load(std::memory_order_acquire)) {
                    SetState(state, current, PlaybackState::ShuttingDown);
                    break;
                }

                for (const auto& command : commands) {
                    try {
                        if (!HandleCommand(state, current, backend, initialized, command)) {
                            running = false;
                            break;
                        }
                    } catch (...) {
                        RecordCurrentException(state, current, backend);
                    }
                }
                continue;
            }

            if (handleCount == 2 && waitResult == WAIT_OBJECT_0 + 1 &&
                current == PlaybackState::Playing) {
                try {
                    const auto renderResult = backend.Render();
                    UpdatePosition(state, backend);
                    if (!FinishIfDrained(state, current, backend, renderResult)) {
                        backend.PumpDecoded(32);
                    }
                } catch (...) {
                    RecordCurrentException(state, current, backend);
                }
                continue;
            }

            RecordError(state, current, backend, NativeCode(E_UNEXPECTED),
                        "Unexpected audio wait result");
            break;
        }

        state->acceptingCommands.store(false, std::memory_order_release);
        backend.Shutdown();
        if (mmcssHandle != nullptr) {
            AvRevertMmThreadCharacteristics(mmcssHandle);
        }
    }

    std::shared_ptr<SharedState> state_;
    std::thread worker_;
};

AudioEngine::AudioEngine(AudioEngineOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

AudioEngine::~AudioEngine() = default;

void AudioEngine::Load(std::filesystem::path file) {
    impl_->Enqueue(Command{CommandType::Load, std::move(file)});
}

void AudioEngine::Play() {
    impl_->Enqueue(Command{CommandType::Play});
}

void AudioEngine::Pause() {
    impl_->Enqueue(Command{CommandType::Pause});
}

void AudioEngine::Stop() {
    impl_->Enqueue(Command{CommandType::Stop});
}

void AudioEngine::Seek(const std::chrono::nanoseconds position) {
    Command command{CommandType::Seek};
    command.position = position;
    impl_->Enqueue(std::move(command));
}

void AudioEngine::SetVolume(const float volume) {
    Command command{CommandType::SetVolume};
    command.volume = NormalizeVolume(volume);
    impl_->Enqueue(std::move(command));
}

AudioStatus AudioEngine::Status() const {
    return impl_->Status();
}

LiveTransport AudioEngine::Live() const noexcept {
    return impl_->Live();
}

AudioAnalysisSnapshot AudioEngine::Analysis(const std::size_t maximumFrames) const {
    return impl_->Analysis(maximumFrames);
}

void AudioEngine::AnalysisInto(AudioAnalysisSnapshot& out,
                               const std::size_t maximumFrames) const {
    impl_->AnalysisInto(out, maximumFrames);
}

std::uint64_t AudioEngine::AnalysisGeneration() const noexcept {
    return impl_->AnalysisGeneration();
}

void AudioEngine::SetEventCallback(EventCallback callback) {
    impl_->SetEventCallback(std::move(callback));
}

} // namespace rivan::audio
