// Rivan native Windows audio backend.
// Media Foundation performs codec decoding; event-driven shared-mode WASAPI renders float PCM.
#include "NativeAudioBackend.h"

#include "PcmRingBuffer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace rivan::audio::detail {
namespace {

using Microsoft::WRL::ComPtr;

// Source Reader pseudo-stream IDs are negative enum values in older SDK headers.
// Express them as DWORDs once to avoid signed-conversion warnings at API boundaries.
constexpr DWORD kAllStreams = 0xFFFFFFFEu;
constexpr DWORD kFirstAudioStream = 0xFFFFFFFDu;
constexpr DWORD kMediaSource = 0xFFFFFFFFu;
constexpr REFERENCE_TIME kSharedRenderBufferDuration = 100LL * 10'000LL;

[[nodiscard]] std::int64_t NativeCode(const HRESULT value) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint32_t>(value));
}

[[noreturn]] void ThrowFailure(const HRESULT result, const char* operation) {
    std::ostringstream text;
    text << operation << " failed (HRESULT 0x" << std::hex << std::uppercase
         << std::setw(8) << std::setfill('0') << static_cast<std::uint32_t>(result) << ')';
    throw NativeAudioException(NativeCode(result), text.str());
}

void Check(const HRESULT result, const char* operation) {
    if (FAILED(result)) {
        ThrowFailure(result, operation);
    }
}

class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    void Reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return value_; }

private:
    HANDLE value_{};
};

struct CoTaskMemWaveFormatDeleter {
    void operator()(WAVEFORMATEX* value) const noexcept { CoTaskMemFree(value); }
};
using UniqueWaveFormat = std::unique_ptr<WAVEFORMATEX, CoTaskMemWaveFormatDeleter>;

class ScopedPropVariant final {
public:
    ScopedPropVariant() noexcept { PropVariantInit(&value_); }
    ~ScopedPropVariant() { PropVariantClear(&value_); }
    ScopedPropVariant(const ScopedPropVariant&) = delete;
    ScopedPropVariant& operator=(const ScopedPropVariant&) = delete;
    [[nodiscard]] PROPVARIANT* Get() noexcept { return &value_; }
    [[nodiscard]] PROPVARIANT& Value() noexcept { return value_; }

private:
    PROPVARIANT value_{};
};

class LockedMediaBuffer final {
public:
    explicit LockedMediaBuffer(IMFMediaBuffer* buffer) : buffer_(buffer) {
        Check(buffer_->Lock(&data_, nullptr, &length_), "IMFMediaBuffer::Lock");
    }
    ~LockedMediaBuffer() { buffer_->Unlock(); }
    LockedMediaBuffer(const LockedMediaBuffer&) = delete;
    LockedMediaBuffer& operator=(const LockedMediaBuffer&) = delete;

    [[nodiscard]] BYTE* Data() const noexcept { return data_; }
    [[nodiscard]] DWORD Length() const noexcept { return length_; }

private:
    IMFMediaBuffer* buffer_{};
    BYTE* data_{};
    DWORD length_{};
};

[[nodiscard]] std::chrono::milliseconds ClampDecodedDuration(
    const std::chrono::milliseconds value) noexcept {
    return std::clamp(value, std::chrono::milliseconds{250}, std::chrono::milliseconds{8000});
}

[[nodiscard]] std::chrono::milliseconds ClampAnalysisDuration(
    const std::chrono::milliseconds value) noexcept {
    return std::clamp(value, std::chrono::milliseconds{100}, std::chrono::milliseconds{30000});
}

[[nodiscard]] std::size_t CheckedSize(const std::uint64_t value, const char* operation) {
    if (value > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        ThrowFailure(E_OUTOFMEMORY, operation);
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] std::chrono::nanoseconds FromHundredNanoseconds(const std::uint64_t value) noexcept {
    constexpr auto maximum = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)() / 100);
    const auto clamped = (std::min)(value, maximum);
    return std::chrono::nanoseconds{static_cast<std::int64_t>(clamped * 100)};
}

[[nodiscard]] bool IsOpusPath(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    for (auto& character : extension) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return extension == L".opus";
}

[[nodiscard]] std::uint16_t ReadLittleEndian16(const unsigned char* data) noexcept {
    return static_cast<std::uint16_t>(data[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8);
}

[[nodiscard]] std::uint64_t ReadLittleEndian64(const unsigned char* data) noexcept {
    std::uint64_t value = 0;
    for (int shift = 7; shift >= 0; --shift) {
        value = (value << 8) | data[shift];
    }
    return value;
}

[[nodiscard]] bool MatchesBytes(const std::span<const unsigned char> data,
                                const std::size_t offset,
                                const std::string_view text) noexcept {
    if (offset + text.size() > data.size()) return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (data[offset + index] != static_cast<unsigned char>(text[index])) return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::uint16_t> ReadOpusPreSkip(
    const std::filesystem::path& path) noexcept {
    constexpr std::size_t kHeaderSearchBytes = 64 * 1024;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;

    std::vector<unsigned char> buffer(kHeaderSearchBytes);
    stream.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<std::size_t>(stream.gcount()));
    const std::span<const unsigned char> bytes{buffer};

    for (std::size_t offset = 0; offset + 12 <= bytes.size(); ++offset) {
        if (MatchesBytes(bytes, offset, "OpusHead")) {
            return ReadLittleEndian16(bytes.data() + offset + 10);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> ReadLastOggGranulePosition(
    const std::filesystem::path& path,
    const std::uintmax_t fileSize) noexcept {
    constexpr std::uintmax_t kSearchChunkBytes = 64 * 1024;
    constexpr auto kUnknownGranule = (std::numeric_limits<std::uint64_t>::max)();

    if (fileSize < 27) return std::nullopt;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;

    std::vector<unsigned char> buffer;
    std::uintmax_t chunkEnd = fileSize;
    while (chunkEnd >= 27) {
        const auto chunkStart = chunkEnd > kSearchChunkBytes ? chunkEnd - kSearchChunkBytes : 0;
        const auto bytesToRead = static_cast<std::size_t>(chunkEnd - chunkStart);
        buffer.assign(bytesToRead, 0);

        stream.clear();
        stream.seekg(static_cast<std::streamoff>(chunkStart), std::ios::beg);
        if (!stream) return std::nullopt;
        stream.read(reinterpret_cast<char*>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size()));
        const auto bytesRead = static_cast<std::size_t>(stream.gcount());
        if (bytesRead < 27) return std::nullopt;

        const std::span<const unsigned char> bytes{buffer.data(), bytesRead};
        for (std::size_t offset = bytes.size() - 27;; --offset) {
            if (MatchesBytes(bytes, offset, "OggS") && offset + 5 <= bytes.size() && bytes[offset + 4] == 0) {
                const auto pageOffset = chunkStart + offset;
                const auto segmentCount = static_cast<std::uintmax_t>(bytes[offset + 26]);
                if (pageOffset + 27 + segmentCount <= fileSize) {
                    const auto granule = ReadLittleEndian64(bytes.data() + offset + 6);
                    if (granule != kUnknownGranule) return granule;
                }
            }
            if (offset == 0) break;
        }

        if (chunkStart == 0) break;
        chunkEnd = chunkStart + 3;
    }
    return std::nullopt;
}

[[nodiscard]] std::chrono::nanoseconds DurationFromOpusSamples(
    const std::uint64_t samples) noexcept {
    const long double nanoseconds = static_cast<long double>(samples) * 1'000'000'000.0L / 48000.0L;
    constexpr auto maximum = static_cast<long double>((std::numeric_limits<std::int64_t>::max)());
    return std::chrono::nanoseconds{
        static_cast<std::int64_t>((std::min)(nanoseconds, maximum))};
}

[[nodiscard]] std::chrono::nanoseconds ProbeOggOpusDuration(
    const std::filesystem::path& path) noexcept {
    if (!IsOpusPath(path)) return {};

    std::error_code ec;
    const auto fileSize = std::filesystem::file_size(path, ec);
    if (ec) return {};

    const auto preSkip = ReadOpusPreSkip(path);
    if (!preSkip) return {};
    const auto granule = ReadLastOggGranulePosition(path, fileSize);
    if (!granule || *granule <= *preSkip) return {};
    return DurationFromOpusSamples(*granule - *preSkip);
}

} // namespace

class NativeAudioBackend::Impl final {
public:
    Impl(AudioAnalysisBuffer& analysis,
         const std::chrono::milliseconds decodedDuration,
         const std::chrono::milliseconds analysisDuration)
        : analysis_(analysis),
          decodedDuration_(ClampDecodedDuration(decodedDuration)),
          analysisDuration_(ClampAnalysisDuration(analysisDuration)) {}

    ~Impl() { Shutdown(); }

    void Initialize() {
        if (comInitialized_) {
            return;
        }

        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comResult)) {
            ThrowFailure(comResult, "CoInitializeEx");
        }
        comInitialized_ = true;

        const HRESULT mfResult = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(mfResult)) {
            CoUninitialize();
            comInitialized_ = false;
            ThrowFailure(mfResult, "MFStartup");
        }
        mfStarted_ = true;
    }

    void Shutdown() noexcept {
        Close();
        if (mfStarted_) {
            MFShutdown();
            mfStarted_ = false;
        }
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
    }

    void Open(const std::filesystem::path& file) {
        if (!mfStarted_) {
            ThrowFailure(CO_E_NOTINITIALIZED, "NativeAudioBackend::Open");
        }

        Close();
        try {
            ComPtr<IMFAttributes> attributes;
            Check(MFCreateAttributes(&attributes, 1), "MFCreateAttributes");
            Check(attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE),
                  "IMFAttributes::SetUINT32(MF_READWRITE_DISABLE_CONVERTERS)");

            // Open byte stream with write-sharing so metadata/cover edits can
            // replace the file while playing (copy overwrite works; rename
            // still needs delete-share, so callers fall back to copy).
            ComPtr<IMFByteStream> stream;
            Check(MFCreateFile(MF_ACCESSMODE_READ,
                               MF_OPENMODE_FAIL_IF_NOT_EXIST,
                               MF_FILEFLAGS_ALLOW_WRITE_SHARING,
                               file.c_str(),
                               stream.GetAddressOf()),
                  "MFCreateFile");
            Check(MFCreateSourceReaderFromByteStream(stream.Get(), attributes.Get(), &reader_),
                  "MFCreateSourceReaderFromByteStream");
            Check(reader_->SetStreamSelection(kAllStreams, FALSE),
                  "IMFSourceReader::SetStreamSelection(all)");
            Check(reader_->SetStreamSelection(kFirstAudioStream, TRUE),
                  "IMFSourceReader::SetStreamSelection(audio)");

            ReadDuration(file);
            // Activate the endpoint and read its shared-mode mix format first so the
            // decoder can be told to output that exact sample rate / channel count.
            // Matching the mix format keeps WASAPI shared mode from invoking its own
            // low-quality resampler / channel matrix (the cause of the "bass-lifted",
            // uneven timbre versus Winamp/Spotify). MF resamples during decode instead.
            ActivateEndpoint();
            ConfigureDecoder();
            ConfigureRenderer();
            StartDecoder();
            hasMedia_ = true;
        } catch (...) {
            Close();
            throw;
        }
    }

    void Close() noexcept {
        if (audioClient_) {
            if (started_) {
                audioClient_->Stop();
            }
            audioClient_->Reset();
        }
        started_ = false;
        elapsedBeforeStart_ = {};
        startClockPosition_ = 0;
        StopDecoder();

        clock_.Reset();
        volumeControl_.Reset();
        renderClient_.Reset();
        audioClient_.Reset();
        renderEvent_.Reset();

        if (reader_) {
            reader_->Flush(kFirstAudioStream);
        }
        reader_.Reset();

        {
            std::scoped_lock lock(bufferMutex_);
            decoded_.ResetCapacity(0);
            pendingSize_ = 0;
            pendingOffset_ = 0;
            decodeEos_ = false;
            decoderError_ = nullptr;
            ++decodeGeneration_;
        }
        decodeCv_.notify_all();
        hasMedia_ = false;
        duration_ = {};
        sampleRate_ = 0;
        channels_ = 0;
        blockAlign_ = 0;
        mixSampleRate_ = 0;
        mixChannels_ = 0;
        endpointBufferFrames_ = 0;
        clockFrequency_ = 0;
        analysis_.Clear();
    }

    void PumpDecoded() {
        EnsureMedia("NativeAudioBackend::PumpDecoded");
        if (started_) {
            RethrowDecoderErrorIfAvailable();
        } else {
            RethrowDecoderError();
        }
        decodeCv_.notify_all();
        if (!started_ && blockAlign_ != 0) {
            const auto targetFrames = endpointBufferFrames_ != 0
                                          ? static_cast<std::size_t>(endpointBufferFrames_)
                                          : static_cast<std::size_t>(sampleRate_ / 20);
            const auto targetBytes = (std::max)(static_cast<std::size_t>(blockAlign_),
                                               targetFrames * blockAlign_);
            std::unique_lock lock(bufferMutex_);
            decodeCv_.wait_for(lock, std::chrono::milliseconds{150}, [this, targetBytes] {
                return decoderError_ != nullptr || decodeEos_ || decoded_.Size() >= targetBytes;
            });
        }
        if (started_) {
            RethrowDecoderErrorIfAvailable();
        } else {
            RethrowDecoderError();
        }
    }

    [[nodiscard]] RenderResult Render() {
        EnsureMedia("NativeAudioBackend::Render");
        RethrowDecoderErrorIfAvailable();

        UINT32 padding = 0;
        Check(audioClient_->GetCurrentPadding(&padding), "IAudioClient::GetCurrentPadding");
        if (padding > endpointBufferFrames_) {
            ThrowFailure(E_UNEXPECTED, "Invalid WASAPI padding");
        }

        const UINT32 availableFrames = endpointBufferFrames_ - padding;
        if (availableFrames == 0) {
            return {};
        }

        bool decodingComplete = false;
        std::size_t queuedFrames = decoded_.Size() / blockAlign_;
        {
            std::unique_lock lock(bufferMutex_, std::try_to_lock);
            if (lock.owns_lock()) {
                decodingComplete = decodeEos_ && pendingSize_ == 0;
                queuedFrames = decoded_.Size() / blockAlign_;
            }
        }

        UINT32 requestedFrames = availableFrames;
        if (decodingComplete) {
            requestedFrames = static_cast<UINT32>((std::min)(
                static_cast<std::size_t>(availableFrames), queuedFrames));
            if (requestedFrames == 0) {
                return {padding == 0};
            }
        }

        BYTE* destination = nullptr;
        Check(renderClient_->GetBuffer(requestedFrames, &destination),
              "IAudioRenderClient::GetBuffer");

        const std::size_t requestedBytes = static_cast<std::size_t>(requestedFrames) * blockAlign_;
        std::size_t copiedBytes = 0;
        {
            std::unique_lock lock(bufferMutex_);
            copiedBytes = decoded_.Read(
                std::span<std::byte>{reinterpret_cast<std::byte*>(destination), requestedBytes});
            if (copiedBytes != 0) {
                decodeCv_.notify_all();
            }
        }
        if (copiedBytes < requestedBytes) {
            std::memset(destination + copiedBytes, 0, requestedBytes - copiedBytes);
        }

        // Push only real PCM; silence-padded underrun frames would pollute the spectrum.
        if (copiedBytes != 0 && channels_ != 0 && blockAlign_ != 0) {
            const auto realFrames = copiedBytes / blockAlign_;
            analysis_.Push(std::span<const float>{
                reinterpret_cast<const float*>(destination),
                realFrames * channels_});
        }

        const HRESULT releaseResult = renderClient_->ReleaseBuffer(requestedFrames, 0);
        if (FAILED(releaseResult)) {
            ThrowFailure(releaseResult, "IAudioRenderClient::ReleaseBuffer");
        }
        return {};
    }

    void Start() {
        EnsureMedia("NativeAudioBackend::Start");
        if (started_) {
            return;
        }

        UINT64 position = 0;
        Check(clock_->GetPosition(&position, nullptr), "IAudioClock::GetPosition");
        Check(audioClient_->Start(), "IAudioClient::Start");
        startClockPosition_ = position;
        started_ = true;
    }

    void Pause() {
        EnsureMedia("NativeAudioBackend::Pause");
        if (!started_) {
            return;
        }

        const auto elapsed = PlaybackElapsed();
        Check(audioClient_->Stop(), "IAudioClient::Stop");
        elapsedBeforeStart_ = elapsed;
        started_ = false;
    }

    void Seek(const std::chrono::nanoseconds position) {
        EnsureMedia("NativeAudioBackend::Seek");
        if (started_) {
            Check(audioClient_->Stop(), "IAudioClient::Stop(seek)");
            started_ = false;
        }
        Check(audioClient_->Reset(), "IAudioClient::Reset(seek)");
        StopDecoder();

        {
            std::scoped_lock lock(bufferMutex_);
            decoded_.Clear();
            pendingSize_ = 0;
            pendingOffset_ = 0;
            decodeEos_ = false;
            decoderError_ = nullptr;
            ++decodeGeneration_;
        }
        startClockPosition_ = 0;
        analysis_.Clear();

        const auto upperBound = duration_.count() > 0
                                    ? duration_
                                    : (std::chrono::nanoseconds::max)();
        const auto clamped = std::clamp(position, std::chrono::nanoseconds{0}, upperBound);
        ScopedPropVariant target;
        target.Value().vt = VT_I8;
        target.Value().hVal.QuadPart = clamped.count() / 100;
        Check(reader_->SetCurrentPosition(GUID_NULL, target.Value()),
              "IMFSourceReader::SetCurrentPosition");
        elapsedBeforeStart_ = clamped;
        StartDecoder();
    }

    void SetVolume(const float volume) {
        volume_ = std::clamp(volume, 0.0F, 1.0F);
        if (volumeControl_) {
            // A transient SetMasterVolume failure must not kill the session; volume is
            // cosmetic state. The render path reports genuine device failures.
            const HRESULT result = volumeControl_->SetMasterVolume(volume_, nullptr);
            (void)result;
        }
    }

    [[nodiscard]] std::chrono::nanoseconds PlaybackElapsed() const noexcept {
        if (!started_ || !clock_ || clockFrequency_ == 0) {
            return elapsedBeforeStart_;
        }

        UINT64 position = 0;
        if (FAILED(clock_->GetPosition(&position, nullptr)) || position < startClockPosition_) {
            return elapsedBeforeStart_;
        }
        const long double deltaSeconds = static_cast<long double>(position - startClockPosition_) /
                                         static_cast<long double>(clockFrequency_);
        const long double deltaNanoseconds = deltaSeconds * 1'000'000'000.0L;
        const auto maximum = static_cast<long double>((std::numeric_limits<std::int64_t>::max)());
        const auto delta = std::chrono::nanoseconds{
            static_cast<std::int64_t>((std::min)(deltaNanoseconds, maximum))};
        return elapsedBeforeStart_ + delta;
    }

    [[nodiscard]] std::chrono::nanoseconds Duration() const noexcept { return duration_; }
    [[nodiscard]] bool HasMedia() const noexcept { return hasMedia_; }
    [[nodiscard]] HANDLE RenderEvent() const noexcept { return renderEvent_.Get(); }

private:
    void EnsureMedia(const char* operation) const {
        if (!hasMedia_) {
            ThrowFailure(MF_E_NOT_INITIALIZED, operation);
        }
    }

    void ReadDuration(const std::filesystem::path& file) {
        duration_ = {};
        ScopedPropVariant duration;
        const HRESULT result = reader_->GetPresentationAttribute(
            kMediaSource, MF_PD_DURATION, duration.Get());
        if (SUCCEEDED(result)) {
            if (duration.Value().vt == VT_UI8) {
                duration_ = FromHundredNanoseconds(duration.Value().uhVal.QuadPart);
            } else if (duration.Value().vt == VT_I8 && duration.Value().hVal.QuadPart > 0) {
                duration_ = FromHundredNanoseconds(
                    static_cast<std::uint64_t>(duration.Value().hVal.QuadPart));
            }
        }
        if (duration_.count() <= 0) duration_ = ProbeOggOpusDuration(file);
    }

    // Activates the default render endpoint and captures its shared-mode mix format.
    // The decoder is later asked to produce this exact float layout so no WASAPI
    // resampling / remixing occurs.
    void ActivateEndpoint() {
        ComPtr<IMMDeviceEnumerator> enumerator;
        Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               IID_PPV_ARGS(&enumerator)),
              "CoCreateInstance(MMDeviceEnumerator)");
        ComPtr<IMMDevice> device;
        Check(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device),
              "IMMDeviceEnumerator::GetDefaultAudioEndpoint");
        Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(audioClient_.GetAddressOf())),
              "IMMDevice::Activate(IAudioClient)");

        WAVEFORMATEX* mixRaw = nullptr;
        Check(audioClient_->GetMixFormat(&mixRaw), "IAudioClient::GetMixFormat");
        UniqueWaveFormat mix{mixRaw};
        // Mix format is float in shared mode; capture its rate and channel count as the
        // decode target. Fall back to the raw values if extensible parsing is not needed.
        mixSampleRate_ = mix->nSamplesPerSec;
        mixChannels_ = mix->nChannels;
    }

    void ConfigureDecoder() {
        ComPtr<IMFMediaType> requestedType;
        Check(MFCreateMediaType(&requestedType), "MFCreateMediaType");
        Check(requestedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio),
              "IMFMediaType::SetGUID(MF_MT_MAJOR_TYPE)");
        Check(requestedType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float),
              "IMFMediaType::SetGUID(MF_MT_SUBTYPE)");
        // Pin the decoder output to the endpoint mix rate and channel count. This makes
        // Media Foundation's resampler do any sample-rate / channel conversion (high
        // quality), so WASAPI shared mode receives a byte-exact match and passes it
        // through untouched.
        if (mixSampleRate_ != 0 && mixChannels_ != 0) {
            const UINT32 bytesPerFrame = mixChannels_ * static_cast<UINT32>(sizeof(float));
            Check(requestedType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, mixChannels_),
                  "IMFMediaType::SetUINT32(NUM_CHANNELS)");
            Check(requestedType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, mixSampleRate_),
                  "IMFMediaType::SetUINT32(SAMPLES_PER_SECOND)");
            Check(requestedType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32),
                  "IMFMediaType::SetUINT32(BITS_PER_SAMPLE)");
            Check(requestedType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, bytesPerFrame),
                  "IMFMediaType::SetUINT32(BLOCK_ALIGNMENT)");
            Check(requestedType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                           mixSampleRate_ * bytesPerFrame),
                  "IMFMediaType::SetUINT32(AVG_BYTES_PER_SECOND)");
            Check(requestedType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE),
                  "IMFMediaType::SetUINT32(ALL_SAMPLES_INDEPENDENT)");
        }
        Check(reader_->SetCurrentMediaType(kFirstAudioStream, nullptr,
                                           requestedType.Get()),
              "IMFSourceReader::SetCurrentMediaType(float PCM)");

        ComPtr<IMFMediaType> actualType;
        Check(reader_->GetCurrentMediaType(kFirstAudioStream, &actualType),
              "IMFSourceReader::GetCurrentMediaType");

        WAVEFORMATEX* rawFormat = nullptr;
        UINT32 formatSize = 0;
        Check(MFCreateWaveFormatExFromMFMediaType(actualType.Get(), &rawFormat, &formatSize,
                                                   MFWaveFormatExConvertFlag_Normal),
              "MFCreateWaveFormatExFromMFMediaType");
        UniqueWaveFormat format{rawFormat};

        bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            formatSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
            const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format.get());
            isFloat = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
        }
        if (!isFloat || format->wBitsPerSample != 32 || format->nChannels == 0 ||
            format->nSamplesPerSec == 0 ||
            format->nBlockAlign != format->nChannels * sizeof(float)) {
            ThrowFailure(MF_E_INVALIDMEDIATYPE, "Media Foundation float PCM negotiation");
        }

        sampleRate_ = format->nSamplesPerSec;
        channels_ = format->nChannels;
        blockAlign_ = format->nBlockAlign;
        waveFormat_ = std::move(format);
    }

    void ConfigureRenderer() {
        // Endpoint already activated in ActivateEndpoint(); reuse audioClient_.
        renderEvent_.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (renderEvent_.Get() == nullptr) {
            ThrowFailure(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW(render)");
        }

        constexpr DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        // Music playback needs underrun margin more than ultra-low latency. The default
        // shared buffer can be too short when UI or disk activity delays the audio thread.
        Check(audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                       kSharedRenderBufferDuration, 0,
                                       waveFormat_.get(), nullptr),
              "IAudioClient::Initialize(shared event mode)");
        Check(audioClient_->SetEventHandle(renderEvent_.Get()),
              "IAudioClient::SetEventHandle");
        Check(audioClient_->GetBufferSize(&endpointBufferFrames_),
              "IAudioClient::GetBufferSize");
        Check(audioClient_->GetService(IID_PPV_ARGS(&renderClient_)),
              "IAudioClient::GetService(IAudioRenderClient)");
        Check(audioClient_->GetService(IID_PPV_ARGS(&volumeControl_)),
              "IAudioClient::GetService(ISimpleAudioVolume)");
        Check(audioClient_->GetService(IID_PPV_ARGS(&clock_)),
              "IAudioClient::GetService(IAudioClock)");
        Check(clock_->GetFrequency(&clockFrequency_), "IAudioClock::GetFrequency");
        Check(volumeControl_->SetMasterVolume(volume_, nullptr),
              "ISimpleAudioVolume::SetMasterVolume");

        const auto requestedBytes = static_cast<std::uint64_t>(sampleRate_) * blockAlign_ *
                                    static_cast<std::uint64_t>(decodedDuration_.count()) / 1000;
        const auto endpointBytes = static_cast<std::uint64_t>(endpointBufferFrames_) *
                                   blockAlign_ * 2;
        auto capacity = CheckedSize((std::max)(requestedBytes, endpointBytes),
                                    "Decoded PCM capacity");
        capacity = (std::max)(capacity, static_cast<std::size_t>(blockAlign_));
        decoded_.ResetCapacity(capacity, blockAlign_);

        // FFT consumers need ~1024 frames; keep a small floor so short analysisDuration
        // still has enough history at high sample rates.
        constexpr std::size_t kMinAnalysisFrames = 2048;
        const auto analysisFramesFromDuration = CheckedSize(
            static_cast<std::uint64_t>(sampleRate_) *
                static_cast<std::uint64_t>(analysisDuration_.count()) / 1000,
            "Analysis capacity");
        const auto analysisFrames =
            (std::max)({analysisFramesFromDuration, kMinAnalysisFrames, std::size_t{1}});
        analysis_.Configure(sampleRate_, channels_, analysisFrames);

        // Spill buffer sized to one full ring write (covers a single oversized MF sample).
        pending_.assign(decoded_.Capacity(), std::byte{});
        pendingSize_ = 0;
        pendingOffset_ = 0;

        waveFormat_.reset();
    }

    void StartDecoder() {
        if (decoderThread_.joinable()) {
            return;
        }

        std::uint64_t generation = 0;
        {
            std::scoped_lock lock(bufferMutex_);
            decoderStop_ = false;
            decoderError_ = nullptr;
            generation = decodeGeneration_;
        }
        decoderThread_ = std::jthread([this, generation](std::stop_token stop) {
            DecoderMain(stop, generation);
        });
    }

    void StopDecoder() noexcept {
        {
            std::scoped_lock lock(bufferMutex_);
            decoderStop_ = true;
            ++decodeGeneration_;
        }
        decodeCv_.notify_all();
        if (decoderThread_.joinable()) {
            CancelSynchronousIo(decoderThread_.native_handle());
        }
        if (decoderThread_.joinable()) {
            decoderThread_.request_stop();
            decoderThread_.join();
        }
    }

    void DecoderMain(std::stop_token stop, const std::uint64_t generation) noexcept {
        bool comInitialized = false;
        try {
            const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(comResult)) {
                ThrowFailure(comResult, "CoInitializeEx(decoder)");
            }
            comInitialized = true;

            while (!stop.stop_requested()) {
                {
                    std::unique_lock lock(bufferMutex_);
                    decodeCv_.wait(lock, [this, &stop, generation] {
                        return stop.stop_requested() || decoderStop_ ||
                               generation != decodeGeneration_ || decoderError_ != nullptr ||
                               decodeEos_ ||
                               (blockAlign_ != 0 && decoded_.Free() >= blockAlign_);
                    });

                    if (stop.stop_requested() || decoderStop_ ||
                        generation != decodeGeneration_ || decoderError_ != nullptr ||
                        decodeEos_) {
                        break;
                    }

                    FlushPending();
                    if (pendingSize_ != 0 || decoded_.Free() < blockAlign_) {
                        continue;
                    }
                }

                DWORD streamIndex = 0;
                DWORD flags = 0;
                LONGLONG timestamp = 0;
                ComPtr<IMFSample> sample;
                Check(reader_->ReadSample(kFirstAudioStream, 0, &streamIndex,
                                          &flags, &timestamp, &sample),
                      "IMFSourceReader::ReadSample");

                if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
                    ThrowFailure(E_FAIL, "Media Foundation source stream");
                }
                if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                    ThrowFailure(MF_E_TRANSFORM_STREAM_CHANGE,
                                 "Unexpected decoded audio format change");
                }

                if (sample) {
                    AppendSample(sample.Get(), generation);
                }
                if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                    std::unique_lock lock(bufferMutex_);
                    while (!stop.stop_requested() && !decoderStop_ &&
                           generation == decodeGeneration_ && pendingSize_ != 0) {
                        decodeCv_.wait(lock, [this, &stop, generation] {
                            return stop.stop_requested() || decoderStop_ ||
                                   generation != decodeGeneration_ ||
                                   (blockAlign_ != 0 && decoded_.Free() >= blockAlign_);
                        });
                        FlushPending();
                    }
                    if (!stop.stop_requested() && !decoderStop_ && generation == decodeGeneration_) {
                        decodeEos_ = true;
                    }
                    lock.unlock();
                    decodeCv_.notify_all();
                    break;
                }
            }
        } catch (...) {
            StoreDecoderError(std::current_exception(), generation);
        }

        if (comInitialized) {
            CoUninitialize();
        }
    }

    void StoreDecoderError(std::exception_ptr error, const std::uint64_t generation) noexcept {
        try {
            std::scoped_lock lock(bufferMutex_);
            if (generation == decodeGeneration_ && decoderError_ == nullptr) {
                decoderError_ = std::move(error);
                decodeEos_ = true;
            }
        } catch (...) {
        }
        decodeCv_.notify_all();
    }

    void RethrowDecoderError() {
        std::exception_ptr error;
        {
            std::scoped_lock lock(bufferMutex_);
            error = decoderError_;
        }
        if (error) {
            std::rethrow_exception(error);
        }
    }

    void RethrowDecoderErrorIfAvailable() {
        std::exception_ptr error;
        {
            std::unique_lock lock(bufferMutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                return;
            }
            error = decoderError_;
        }
        if (error) {
            std::rethrow_exception(error);
        }
    }

    void AppendSample(IMFSample* sample, const std::uint64_t generation) {
        ComPtr<IMFMediaBuffer> buffer;
        Check(sample->ConvertToContiguousBuffer(&buffer),
              "IMFSample::ConvertToContiguousBuffer");
        LockedMediaBuffer locked{buffer.Get()};
        if (locked.Length() == 0) {
            return;
        }
        if (locked.Length() % blockAlign_ != 0) {
            ThrowFailure(MF_E_INVALIDMEDIATYPE, "Unaligned decoded PCM sample");
        }
        if (locked.Length() > decoded_.Capacity()) {
            ThrowFailure(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW),
                         "Decoded PCM sample exceeds bounded buffer");
        }

        const auto source = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(locked.Data()), locked.Length()};
        std::scoped_lock lock(bufferMutex_);
        if (generation != decodeGeneration_ || decoderStop_) {
            return;
        }
        const auto written = decoded_.Write(source);
        if (written < source.size()) {
            const auto remainder = source.size() - written;
            if (remainder > pending_.size()) {
                ThrowFailure(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW),
                             "Decoded PCM pending spill exceeds capacity");
            }
            std::memcpy(pending_.data(), source.data() + written, remainder);
            pendingSize_ = remainder;
            pendingOffset_ = 0;
        }
        decodeCv_.notify_all();
    }

    void FlushPending() {
        if (pendingSize_ == 0) {
            return;
        }
        const auto remainder =
            std::span<const std::byte>{pending_.data() + pendingOffset_,
                                       pendingSize_ - pendingOffset_};
        pendingOffset_ += decoded_.Write(remainder);
        if (pendingOffset_ == pendingSize_) {
            pendingSize_ = 0;
            pendingOffset_ = 0;
        }
    }

    AudioAnalysisBuffer& analysis_;
    std::chrono::milliseconds decodedDuration_;
    std::chrono::milliseconds analysisDuration_;

    bool comInitialized_{};
    bool mfStarted_{};
    bool hasMedia_{};
    bool started_{};
    bool decodeEos_{};
    float volume_{1.0F};

    ComPtr<IMFSourceReader> reader_;
    ComPtr<IAudioClient> audioClient_;
    ComPtr<IAudioRenderClient> renderClient_;
    ComPtr<ISimpleAudioVolume> volumeControl_;
    ComPtr<IAudioClock> clock_;
    UniqueHandle renderEvent_;
    UniqueWaveFormat waveFormat_;

    PcmRingBuffer decoded_;
    std::vector<std::byte> pending_;
    std::size_t pendingSize_{};
    std::size_t pendingOffset_{};
    std::mutex bufferMutex_;
    std::condition_variable decodeCv_;
    std::jthread decoderThread_;
    bool decoderStop_{};
    std::uint64_t decodeGeneration_{};
    std::exception_ptr decoderError_;
    std::chrono::nanoseconds duration_{};
    std::chrono::nanoseconds elapsedBeforeStart_{};
    UINT64 startClockPosition_{};
    UINT64 clockFrequency_{};
    UINT32 endpointBufferFrames_{};
    std::uint32_t sampleRate_{};
    std::uint16_t channels_{};
    std::uint16_t blockAlign_{};
    // Endpoint shared-mode mix format captured at Open to steer decode conversion.
    std::uint32_t mixSampleRate_{};
    std::uint16_t mixChannels_{};
};

NativeAudioBackend::NativeAudioBackend(AudioAnalysisBuffer& analysis,
                                       const std::chrono::milliseconds decodedDuration,
                                       const std::chrono::milliseconds analysisDuration)
    : impl_(std::make_unique<Impl>(analysis, decodedDuration, analysisDuration)) {}

NativeAudioBackend::~NativeAudioBackend() = default;

void NativeAudioBackend::Initialize() { impl_->Initialize(); }
void NativeAudioBackend::Shutdown() noexcept { impl_->Shutdown(); }
void NativeAudioBackend::Open(const std::filesystem::path& file) { impl_->Open(file); }
void NativeAudioBackend::Close() noexcept { impl_->Close(); }
void NativeAudioBackend::PumpDecoded() {
    impl_->PumpDecoded();
}
RenderResult NativeAudioBackend::Render() { return impl_->Render(); }
void NativeAudioBackend::Start() { impl_->Start(); }
void NativeAudioBackend::Pause() { impl_->Pause(); }
void NativeAudioBackend::Seek(const std::chrono::nanoseconds position) { impl_->Seek(position); }
void NativeAudioBackend::SetVolume(const float volume) { impl_->SetVolume(volume); }
std::chrono::nanoseconds NativeAudioBackend::Duration() const noexcept { return impl_->Duration(); }
std::chrono::nanoseconds NativeAudioBackend::PlaybackElapsed() const noexcept {
    return impl_->PlaybackElapsed();
}
bool NativeAudioBackend::HasMedia() const noexcept { return impl_->HasMedia(); }
void* NativeAudioBackend::RenderEvent() const noexcept { return impl_->RenderEvent(); }

} // namespace rivan::audio::detail
