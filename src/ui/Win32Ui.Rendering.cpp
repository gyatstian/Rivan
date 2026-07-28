// Win32Ui.Rendering.cpp
#include "Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::ClearFilePreview() noexcept {
        {
            std::lock_guard lock(previewRequestMutex);
            requestedPreviewPath.clear();
            ++requestedPreviewGeneration;
        }
        previewWake.notify_all();
        previewBitmap.Reset();
        previewPath.clear();
        previewIsVideo = false;
        previewHasPresentedFrame = false;
        previewWantedSeconds.store(0.0, std::memory_order_relaxed);
        pendingPreviewFrameVersion.store(0, std::memory_order_relaxed);
        uploadedPreviewFrameVersion = 0;
        {
            std::lock_guard lock(previewFrameMutex);
            pendingPreviewPixels.clear();
            pendingPreviewWidth = 0;
            pendingPreviewHeight = 0;
            pendingPreviewStride = 0;
        }
        previewFullscreen = false;
        previewFullscreenCloseBounds = {};
    }

void Win32Ui::Impl::StopPreviewWorker() noexcept {
        previewWorker.request_stop();
        previewWake.notify_all();
        previewWorker = {};
    }

[[nodiscard]] bool Win32Ui::Impl::IsVideoPath(const std::wstring& path) {
        auto extension = std::filesystem::path(path).extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        return extension == L".mp4";
    }

[[nodiscard]] bool Win32Ui::Impl::CreatePreviewBitmapFromBgra(UINT width, UINT height, const BYTE* data,
                                                    UINT stride) {
        if (!target || !data || width == 0 || height == 0) return false;
        if (previewBitmap) {
            const auto size = previewBitmap->GetPixelSize();
            if (size.width == width && size.height == height &&
                SUCCEEDED(previewBitmap->CopyFromMemory(nullptr, data, stride))) {
                return true;
            }
        }
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(target->CreateBitmap(D2D1::SizeU(width, height), data, stride, props,
                                        bitmap.ReleaseAndGetAddressOf()))) {
            return false;
        }
        previewBitmap = std::move(bitmap);
        return true;
    }

[[nodiscard]] bool Win32Ui::Impl::CreatePreviewBitmapFromHBitmap(HBITMAP bitmap) {
        if (!target || !wicFactory || !bitmap) return false;
        ComPtr<IWICBitmap> wicBitmap;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wicFactory->CreateBitmapFromHBITMAP(
                bitmap, nullptr, WICBitmapUsePremultipliedAlpha,
                wicBitmap.ReleaseAndGetAddressOf())) ||
            FAILED(wicFactory->CreateFormatConverter(converter.ReleaseAndGetAddressOf())) ||
            FAILED(converter->Initialize(wicBitmap.Get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeMedianCut))) {
            return false;
        }
        previewBitmap.Reset();
        return SUCCEEDED(target->CreateBitmapFromWicBitmap(
            converter.Get(), nullptr, previewBitmap.ReleaseAndGetAddressOf()));
    }

[[nodiscard]] bool Win32Ui::Impl::CreatePreviewBitmapFromEncoded(const BYTE* data, std::size_t size) {
        if (!target || !wicFactory || !data || size == 0) return false;
        std::vector<BYTE> owned(data, data + size);
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wicFactory->CreateStream(stream.ReleaseAndGetAddressOf())) ||
            FAILED(stream->InitializeFromMemory(owned.data(), static_cast<DWORD>(owned.size()))) ||
            FAILED(wicFactory->CreateDecoderFromStream(
                stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                decoder.ReleaseAndGetAddressOf())) ||
            FAILED(decoder->GetFrame(0, frame.ReleaseAndGetAddressOf())) ||
            FAILED(wicFactory->CreateFormatConverter(converter.ReleaseAndGetAddressOf())) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeMedianCut))) {
            return false;
        }
        previewBitmap.Reset();
        return SUCCEEDED(target->CreateBitmapFromWicBitmap(
            converter.Get(), nullptr, previewBitmap.ReleaseAndGetAddressOf()));
    }

// Pulls embedded album art from ID3v2 APIC frames when Shell thumbnails fail.
[[nodiscard]] bool Win32Ui::Impl::LoadEmbeddedId3Cover(const std::wstring& path) {
        std::ifstream stream(std::filesystem::path(path), std::ios::binary);
        if (!stream) return false;
        char header[10]{};
        stream.read(header, 10);
        if (stream.gcount() != 10 || std::memcmp(header, "ID3", 3) != 0) return false;
        const int version = static_cast<unsigned char>(header[3]);
        if (version < 2 || version > 4) return false;
        const auto synchsafe = [](const unsigned char* bytes) {
            return (static_cast<std::uint32_t>(bytes[0] & 0x7F) << 21) |
                   (static_cast<std::uint32_t>(bytes[1] & 0x7F) << 14) |
                   (static_cast<std::uint32_t>(bytes[2] & 0x7F) << 7) |
                   static_cast<std::uint32_t>(bytes[3] & 0x7F);
        };
        const auto rawSize = [](const unsigned char* bytes) {
            return (static_cast<std::uint32_t>(bytes[0]) << 24) |
                   (static_cast<std::uint32_t>(bytes[1]) << 16) |
                   (static_cast<std::uint32_t>(bytes[2]) << 8) |
                   static_cast<std::uint32_t>(bytes[3]);
        };
        const std::uint32_t tagSize = synchsafe(
            reinterpret_cast<const unsigned char*>(header + 6));
        if (tagSize == 0 || tagSize > 16 * 1024 * 1024) return false;
        std::vector<unsigned char> tag(tagSize);
        stream.read(reinterpret_cast<char*>(tag.data()),
                    static_cast<std::streamsize>(tag.size()));
        if (static_cast<std::size_t>(stream.gcount()) != tag.size()) return false;

        std::size_t offset = 0;
        while (offset + 10 <= tag.size()) {
            if (tag[offset] == 0) break;
            const char* id = reinterpret_cast<const char*>(tag.data() + offset);
            const bool isApic = version == 2
                                    ? std::memcmp(id, "PIC", 3) == 0
                                    : std::memcmp(id, "APIC", 4) == 0;
            const std::size_t headerSize = version == 2 ? 6u : 10u;
            if (offset + headerSize > tag.size()) break;
            std::uint32_t frameSize = 0;
            if (version == 2) {
                frameSize = (static_cast<std::uint32_t>(tag[offset + 3]) << 16) |
                            (static_cast<std::uint32_t>(tag[offset + 4]) << 8) |
                            static_cast<std::uint32_t>(tag[offset + 5]);
            } else if (version == 4) {
                frameSize = synchsafe(tag.data() + offset + 4);
            } else {
                frameSize = rawSize(tag.data() + offset + 4);
            }
            if (frameSize == 0 || offset + headerSize + frameSize > tag.size()) break;
            if (isApic) {
                const unsigned char* body = tag.data() + offset + headerSize;
                const unsigned char* end = body + frameSize;
                if (body >= end) break;
                const unsigned char encoding = *body++;
                if (version == 2) {
                    if (end - body < 4) break;
                    body += 3;  // image format
                    if (body >= end) break;
                    ++body;  // picture type
                } else {
                    while (body < end && *body != 0) ++body;
                    if (body >= end) break;
                    ++body;  // skip mime NUL
                    if (body >= end) break;
                    ++body;  // picture type
                }
                // Description terminator is 1 byte (latin1/utf8) or 2 bytes (utf16).
                if (encoding == 1 || encoding == 2) {
                    while (body + 1 < end && !(body[0] == 0 && body[1] == 0)) body += 2;
                    if (body + 1 >= end) break;
                    body += 2;
                } else {
                    while (body < end && *body != 0) ++body;
                    if (body >= end) break;
                    ++body;
                }
                if (body >= end) break;
                return CreatePreviewBitmapFromEncoded(body, static_cast<std::size_t>(end - body));
            }
            offset += headerSize + frameSize;
        }
        return false;
    }

[[nodiscard]] bool Win32Ui::Impl::LoadCoverArt(const std::wstring& path) {
        if (!target || !wicFactory) return false;
        ComPtr<IShellItem> item;
        ComPtr<IShellItemImageFactory> thumbnails;
        HBITMAP bitmap{};
        // Prefer real thumbnail/cover; fall back to any shell image; then ID3 APIC.
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
            SUCCEEDED(item.As(&thumbnails))) {
            const SIZE sizes[] = {{512, 512}, {256, 256}, {128, 128}};
            for (const auto size : sizes) {
                if (SUCCEEDED(thumbnails->GetImage(
                        size, SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT, &bitmap)) &&
                    CreatePreviewBitmapFromHBitmap(bitmap)) {
                    DeleteObject(bitmap);
                    return true;
                }
                if (bitmap) {
                    DeleteObject(bitmap);
                    bitmap = nullptr;
                }
            }
        }
        return LoadEmbeddedId3Cover(path);
    }

[[nodiscard]] bool Win32Ui::Impl::OpenVideoPreview(const std::wstring& path) {
        previewIsVideo = true;
        previewWantedSeconds.store(std::max(0.0, model.positionSeconds), std::memory_order_relaxed);
        {
            std::lock_guard lock(previewRequestMutex);
            requestedPreviewPath = path;
            ++requestedPreviewGeneration;
        }
        if (previewWorker.joinable()) {
            previewWake.notify_all();
            return true;
        }
        previewWorker = std::jthread([this](std::stop_token stop) {
            const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool comInitialized = SUCCEEDED(comResult);
            if (!comInitialized || FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
                if (comInitialized) CoUninitialize();
                return;
            }
            std::uint64_t activeGeneration = 0;
            double synced = -1.0;
            ComPtr<IMFSourceReader> reader;
            UINT width = 0;
            UINT height = 0;
            UINT packedStride = 0;
            LONG defaultStride = 0;
            while (!stop.stop_requested()) {
                std::wstring path;
                std::uint64_t generation = 0;
                if (!reader) {
                    std::unique_lock lock(previewRequestMutex);
                    previewWake.wait_for(lock, stop, std::chrono::milliseconds(20), [this, activeGeneration] {
                        return requestedPreviewGeneration != activeGeneration;
                    });
                    path = requestedPreviewPath;
                    generation = requestedPreviewGeneration;
                } else {
                    std::lock_guard lock(previewRequestMutex);
                    path = requestedPreviewPath;
                    generation = requestedPreviewGeneration;
                }
                if (stop.stop_requested()) break;
                if (generation != activeGeneration) {
                    reader.Reset();
                    activeGeneration = generation;
                    synced = -1.0;
                    if (path.empty()) continue;
                    ComPtr<IMFAttributes> attributes;
                    if (FAILED(MFCreateAttributes(attributes.ReleaseAndGetAddressOf(), 1)) ||
                        FAILED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
                        FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(),
                                                           reader.ReleaseAndGetAddressOf()))) {
                        reader.Reset();
                        continue;
                    }
                    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
                    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
                    ComPtr<IMFMediaType> nativeType;
                    if (FAILED(reader->GetNativeMediaType(
                            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                            nativeType.ReleaseAndGetAddressOf())) ||
                        FAILED(MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
                        width == 0 || height == 0) {
                        reader.Reset();
                        continue;
                    }
                    // Do not ask Source Reader to resize decoded frames. Several decoder
                    // paths report scaled RGB32 stride/layout inconsistently, producing
                    // duplicated image fields or horizontal corruption. D2D scales safely.
                    ComPtr<IMFMediaType> type;
                    if (FAILED(MFCreateMediaType(type.ReleaseAndGetAddressOf())) ||
                        FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
                        FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
                        FAILED(reader->SetCurrentMediaType(
                            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get()))) {
                        reader.Reset();
                        continue;
                    }
                    ComPtr<IMFMediaType> outputType;
                    if (FAILED(reader->GetCurrentMediaType(
                            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                            outputType.ReleaseAndGetAddressOf())) ||
                        FAILED(MFGetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
                        width == 0 || height == 0) {
                        reader.Reset();
                        continue;
                    }
                    packedStride = width * 4;
                    defaultStride = static_cast<LONG>(packedStride);
                    UINT32 defaultStrideAttribute = 0;
                    if (outputType && SUCCEEDED(outputType->GetUINT32(MF_MT_DEFAULT_STRIDE,
                                                                       &defaultStrideAttribute))) {
                        defaultStride = static_cast<LONG>(defaultStrideAttribute);
                    }
                    continue;
                }
                if (!reader) continue;
                double want = previewWantedSeconds.load(std::memory_order_relaxed);
                if (synced >= 0.0 && want + kPreviewFrameLeadSeconds < synced) {
                    std::unique_lock lock(previewFrameMutex);
                    previewWake.wait_for(lock, stop, std::chrono::milliseconds(20), [this, synced] {
                        return previewWantedSeconds.load(std::memory_order_relaxed) +
                                   kPreviewFrameLeadSeconds >= synced;
                    });
                    continue;
                }
                if (synced < 0.0 || std::abs(want - synced) > kPreviewSeekThresholdSeconds) {
                    PROPVARIANT position{};
                    PropVariantInit(&position);
                    position.vt = VT_I8;
                    position.hVal.QuadPart = static_cast<LONGLONG>(want * 10'000'000.0);
                    reader->SetCurrentPosition(GUID_NULL, position);
                    PropVariantClear(&position);
                    synced = want;
                }
                DWORD flags = 0;
                LONGLONG timestamp = 0;
                ComPtr<IMFSample> sample;
                if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                               nullptr, &flags, &timestamp, sample.ReleaseAndGetAddressOf())) ||
                    (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) != 0) {
                    reader.Reset();
                    continue;
                }
                if (!sample) continue;
                synced = static_cast<double>(timestamp) / 10'000'000.0;
                if (synced + kPreviewFrameLeadSeconds < want) continue;
                ComPtr<IMFMediaBuffer> buffer;
                BYTE* data = nullptr;
                LONG stride = defaultStride;
                DWORD maxLength = 0;
                DWORD length = 0;
                if (FAILED(sample->ConvertToContiguousBuffer(buffer.ReleaseAndGetAddressOf()))) continue;
                if (FAILED(buffer->Lock(&data, &maxLength, &length)) || !data) {
                    continue;
                }
                const UINT sourceStride = static_cast<UINT>(std::abs(stride));
                const std::size_t requiredBytes = static_cast<std::size_t>(sourceStride) * height;
                if (stride > 0 && sourceStride >= packedStride && length >= requiredBytes) {
                    std::lock_guard lock(previewFrameMutex);
                    pendingPreviewPixels.resize(static_cast<std::size_t>(packedStride) * height);
                    for (UINT row = 0; row < height; ++row) {
                        const UINT sourceRow = stride < 0 ? height - 1 - row : row;
                        std::memcpy(pendingPreviewPixels.data() + static_cast<std::size_t>(row) * packedStride,
                                    data + static_cast<std::size_t>(sourceRow) * sourceStride,
                                    packedStride);
                    }
                    pendingPreviewWidth = width;
                    pendingPreviewHeight = height;
                    pendingPreviewStride = packedStride;
                    pendingPreviewFrameVersion.fetch_add(1, std::memory_order_release);
                }
                buffer->Unlock();
                if (window) InvalidateRect(window, nullptr, FALSE);
            }
            MFShutdown();
            if (comInitialized) CoUninitialize();
        });
        return true;
    }

[[nodiscard]] ComPtr<ID2D1Bitmap> Win32Ui::Impl::CreateTrackCoverBitmapFromHBitmap(HBITMAP bitmap) {
        ComPtr<ID2D1Bitmap> result;
        if (!target || !wicFactory || !bitmap) return result;
        ComPtr<IWICBitmap> source;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wicFactory->CreateBitmapFromHBITMAP(bitmap, nullptr,
                                                        WICBitmapUsePremultipliedAlpha,
                                                        source.ReleaseAndGetAddressOf())) ||
            FAILED(wicFactory->CreateBitmapScaler(scaler.ReleaseAndGetAddressOf())) ||
            FAILED(scaler->Initialize(source.Get(), kTrackCoverSize, kTrackCoverSize,
                                      WICBitmapInterpolationModeFant)) ||
            FAILED(wicFactory->CreateFormatConverter(converter.ReleaseAndGetAddressOf())) ||
            FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeMedianCut)) ||
            FAILED(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr,
                                                     result.ReleaseAndGetAddressOf()))) {
            result.Reset();
        }
        return result;
    }

[[nodiscard]] ComPtr<ID2D1Bitmap> Win32Ui::Impl::CreateTrackCoverBitmapFromEncoded(const BYTE* data,
                                                                          std::size_t size) {
        ComPtr<ID2D1Bitmap> result;
        if (!target || !wicFactory || !data || size == 0 || size > MAXDWORD) return result;
        std::vector<BYTE> owned(data, data + size);
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wicFactory->CreateStream(stream.ReleaseAndGetAddressOf())) ||
            FAILED(stream->InitializeFromMemory(owned.data(), static_cast<DWORD>(owned.size()))) ||
            FAILED(wicFactory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                       WICDecodeMetadataCacheOnDemand,
                                                       decoder.ReleaseAndGetAddressOf())) ||
            FAILED(decoder->GetFrame(0, frame.ReleaseAndGetAddressOf())) ||
            FAILED(wicFactory->CreateBitmapScaler(scaler.ReleaseAndGetAddressOf())) ||
            FAILED(scaler->Initialize(frame.Get(), kTrackCoverSize, kTrackCoverSize,
                                      WICBitmapInterpolationModeFant)) ||
            FAILED(wicFactory->CreateFormatConverter(converter.ReleaseAndGetAddressOf())) ||
            FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeMedianCut)) ||
            FAILED(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr,
                                                     result.ReleaseAndGetAddressOf()))) {
            result.Reset();
        }
        return result;
    }

[[nodiscard]] ComPtr<ID2D1Bitmap> Win32Ui::Impl::LoadEmbeddedId3TrackCover(const std::wstring& path) {
        std::ifstream stream(std::filesystem::path(path), std::ios::binary);
        if (!stream) return {};
        char header[10]{};
        stream.read(header, 10);
        if (stream.gcount() != 10 || std::memcmp(header, "ID3", 3) != 0) return {};
        const int version = static_cast<unsigned char>(header[3]);
        if (version < 2 || version > 4) return {};
        const auto synchsafe = [](const unsigned char* bytes) {
            return (static_cast<std::uint32_t>(bytes[0] & 0x7F) << 21) |
                   (static_cast<std::uint32_t>(bytes[1] & 0x7F) << 14) |
                   (static_cast<std::uint32_t>(bytes[2] & 0x7F) << 7) |
                   static_cast<std::uint32_t>(bytes[3] & 0x7F);
        };
        const auto rawSize = [](const unsigned char* bytes) {
            return (static_cast<std::uint32_t>(bytes[0]) << 24) |
                   (static_cast<std::uint32_t>(bytes[1]) << 16) |
                   (static_cast<std::uint32_t>(bytes[2]) << 8) |
                   static_cast<std::uint32_t>(bytes[3]);
        };
        const std::uint32_t tagSize = synchsafe(
            reinterpret_cast<const unsigned char*>(header + 6));
        if (tagSize == 0 || tagSize > 16 * 1024 * 1024) return {};
        std::vector<unsigned char> tag(tagSize);
        stream.read(reinterpret_cast<char*>(tag.data()), static_cast<std::streamsize>(tag.size()));
        if (static_cast<std::size_t>(stream.gcount()) != tag.size()) return {};

        std::size_t offset = 0;
        while (offset + 10 <= tag.size()) {
            if (tag[offset] == 0) break;
            const char* id = reinterpret_cast<const char*>(tag.data() + offset);
            const bool isApic = version == 2 ? std::memcmp(id, "PIC", 3) == 0
                                             : std::memcmp(id, "APIC", 4) == 0;
            const std::size_t headerSize = version == 2 ? 6u : 10u;
            if (offset + headerSize > tag.size()) break;
            const std::uint32_t frameSize = version == 2
                ? (static_cast<std::uint32_t>(tag[offset + 3]) << 16) |
                      (static_cast<std::uint32_t>(tag[offset + 4]) << 8) |
                      static_cast<std::uint32_t>(tag[offset + 5])
                : version == 4 ? synchsafe(tag.data() + offset + 4)
                               : rawSize(tag.data() + offset + 4);
            if (frameSize == 0 || offset + headerSize + frameSize > tag.size()) break;
            if (isApic) {
                const unsigned char* body = tag.data() + offset + headerSize;
                const unsigned char* end = body + frameSize;
                if (body >= end) break;
                const unsigned char encoding = *body++;
                if (version == 2) {
                    if (end - body < 4) break;
                    body += 3;  // image format
                    if (body >= end) break;
                    ++body;  // picture type
                } else {
                    while (body < end && *body != 0) ++body;
                    if (body >= end) break;
                    ++body;  // mime NUL
                    if (body >= end) break;
                    ++body;  // picture type
                }
                if (encoding == 1 || encoding == 2) {
                    while (body + 1 < end && !(body[0] == 0 && body[1] == 0)) body += 2;
                    if (body + 1 >= end) break;
                    body += 2;
                } else {
                    while (body < end && *body != 0) ++body;
                    if (body >= end) break;
                    ++body;
                }
                if (body < end) {
                    return CreateTrackCoverBitmapFromEncoded(
                        body, static_cast<std::size_t>(end - body));
                }
                break;
            }
            offset += headerSize + frameSize;
        }
        return {};
    }

void Win32Ui::Impl::TrimTrackCoverCache() {
        while (trackCoverCache.size() > kMaximumTrackCoverCacheEntries) {
            const auto oldest = std::min_element(
                trackCoverCache.begin(), trackCoverCache.end(),
                [](const auto& left, const auto& right) {
                    return left.second.lastUsed < right.second.lastUsed;
                });
            if (oldest == trackCoverCache.end()) break;
            trackCoverCache.erase(oldest);
        }
    }

[[nodiscard]] ID2D1Bitmap* Win32Ui::Impl::TrackCoverBitmap(const std::wstring& path) {
        if (!model.trackCoverArtEnabled || path.empty() || !target || !wicFactory) return nullptr;
        const auto cached = trackCoverCache.find(path);
        if (cached != trackCoverCache.end()) {
            cached->second.lastUsed = ++trackCoverUseCounter;
            return cached->second.bitmap.Get();
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < nextTrackCoverLookup) return nullptr;
        nextTrackCoverLookup = now + std::chrono::milliseconds(100);

        TrackCoverCacheEntry entry;
        entry.lastUsed = ++trackCoverUseCounter;
        ComPtr<IShellItem> item;
        ComPtr<IShellItemImageFactory> thumbnails;
        HBITMAP bitmap{};
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
            SUCCEEDED(item.As(&thumbnails)) &&
            SUCCEEDED(thumbnails->GetImage({static_cast<LONG>(kTrackCoverSize),
                                            static_cast<LONG>(kTrackCoverSize)},
                                           SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT |
                                               SIIGBF_THUMBNAILONLY,
                                           &bitmap))) {
            entry.bitmap = CreateTrackCoverBitmapFromHBitmap(bitmap);
            DeleteObject(bitmap);
        }
        if (!entry.bitmap) entry.bitmap = LoadEmbeddedId3TrackCover(path);
        // A null entry is intentional: unsupported/no-art files are remembered too.
        auto [inserted, ignored] = trackCoverCache.emplace(path, std::move(entry));
        (void)ignored;
        TrimTrackCoverCache();
        return inserted->second.bitmap.Get();
    }

void Win32Ui::Impl::DrawTrackCover(const TrackView& track, const D2D1_RECT_F& row) {
        if (auto* bitmap = TrackCoverBitmap(track.filePath)) {
            const auto cover = Rect(row.right - 24.0F, row.top + 1.0F,
                                    row.right - 6.0F, row.bottom - 1.0F);
            target->DrawBitmap(bitmap, cover, 1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }

void Win32Ui::Impl::UpdateVideoPreviewFrame() {
        if (!previewIsVideo || !target) return;
        previewWantedSeconds.store(std::max(0.0, model.positionSeconds), std::memory_order_relaxed);
        previewWake.notify_all();
        if (pendingPreviewFrameVersion.load(std::memory_order_acquire) == uploadedPreviewFrameVersion) return;
        std::vector<BYTE> pixels;
        UINT width = 0;
        UINT height = 0;
        UINT stride = 0;
        std::uint64_t version = 0;
        {
            std::lock_guard lock(previewFrameMutex);
            version = pendingPreviewFrameVersion.load(std::memory_order_relaxed);
            if (version == uploadedPreviewFrameVersion) return;
            pixels.swap(pendingPreviewPixels);
            width = pendingPreviewWidth;
            height = pendingPreviewHeight;
            stride = pendingPreviewStride;
        }
        if (pixels.empty() || !CreatePreviewBitmapFromBgra(width, height, pixels.data(), stride)) return;
        uploadedPreviewFrameVersion = version;
        previewHasPresentedFrame = true;
    }

void Win32Ui::Impl::DrawPreviewBitmap(const D2D1_RECT_F& bounds) {
        if (!previewBitmap || Width(bounds) <= 1.0F || Height(bounds) <= 1.0F) return;
        const auto bitmapSize = previewBitmap->GetSize();
        if (bitmapSize.width <= 0.0F || bitmapSize.height <= 0.0F) return;
        const float scale = std::min(Width(bounds) / bitmapSize.width,
                                     Height(bounds) / bitmapSize.height);
        const float width = bitmapSize.width * scale;
        const float height = bitmapSize.height * scale;
        const auto destination = Rect(
            bounds.left + (Width(bounds) - width) * 0.5F,
            bounds.top + (Height(bounds) - height) * 0.5F,
            bounds.left + (Width(bounds) + width) * 0.5F,
            bounds.top + (Height(bounds) + height) * 0.5F);
        target->DrawBitmap(previewBitmap.Get(), destination, 1.0F,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

void Win32Ui::Impl::DrawPreviewFullscreenOverlay(const D2D1_SIZE_F size,
                                      std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        previewFullscreenCloseBounds = {};
        if (!previewFullscreen || !previewIsVideo) return;
        const auto full = Rect(0, 0, size.width, size.height);
        target->FillRectangle(full, b[0].Get());
        // Slightly darker matte so letterbox edges read as cinema, not chrome.
        ComPtr<ID2D1SolidColorBrush> matte;
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.0F, 0.0F, 0.0F, 1.0F),
                                                    matte.ReleaseAndGetAddressOf()))) {
            target->FillRectangle(full, matte.Get());
        }
        DrawPreviewBitmap(full);
        // Exit affordance only when the cursor is near any window edge (no persistent guide).
        const float edge = 28.0F;
        const float mx = static_cast<float>(mouse.x);
        const float my = static_cast<float>(mouse.y);
        const bool nearEdge = mx >= 0.0F && my >= 0.0F &&
                              (mx <= edge || my <= edge || mx >= size.width - edge ||
                               my >= size.height - edge);
        if (nearEdge) {
            previewFullscreenCloseBounds =
                Rect(size.width - 42.0F, 10.0F, size.width - 10.0F, 42.0F);
            const bool hot = Contains(previewFullscreenCloseBounds, mx, my);
            DrawBevel(previewFullscreenCloseBounds, hot ? b[7].Get() : b[2].Get(), b[3].Get(),
                      b[4].Get(), false);
            DrawText(L"X", previewFullscreenCloseBounds, b[9].Get(), headingFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            AddSimpleHit(previewFullscreenCloseBounds, HitKind::FilePreviewExitFullscreen);
        }
        // Full-area hit so double-click / single-click routing stays in overlay mode.
        AddSimpleHit(full, HitKind::FilePreviewFullscreen);
    }

void Win32Ui::Impl::LoadFilePreview(const std::wstring& path) {
        const bool keepFullscreen = previewFullscreen && IsVideoPath(path);
        ClearFilePreview();
        if (path.empty() || !filePreviewExpanded || !model.filePreviewEnabled ||
            windowKind != WindowKind::Main) return;
        previewPath = path;
        previewFullscreen = keepFullscreen;
        if (IsVideoPath(path)) {
            if (!OpenVideoPreview(path)) {
                ClearFilePreview();
                previewPath = path;
                (void)LoadCoverArt(path);
            }
            return;
        }
        (void)LoadCoverArt(path);
    }

[[nodiscard]] std::wstring Win32Ui::Impl::ActivePreviewPath() const {
        if (!model.filePreviewEnabled) return {};
        // Always the transport track — never the browsed playlist selection.
        return model.nowPlayingPath;
    }

void Win32Ui::Impl::ExitPreviewFullscreen() noexcept {
        if (!previewFullscreen) return;
        previewFullscreen = false;
        previewFullscreenCloseBounds = {};
        InvalidateRect(window, nullptr, FALSE);
    }

void Win32Ui::Impl::EnterPreviewFullscreen() noexcept {
        if (!previewIsVideo || !filePreviewExpanded || !model.filePreviewEnabled) return;
        previewFullscreen = true;
        InvalidateRect(window, nullptr, FALSE);
    }

void Win32Ui::Impl::SyncFilePreview(bool advanceVideo) {
        if (!model.filePreviewEnabled) {
            filePreviewExpanded = false;
            previewFullscreen = false;
            if (!previewPath.empty() || previewBitmap) ClearFilePreview();
            return;
        }
        if (!filePreviewExpanded || model.miniPlayer) {
            previewFullscreen = false;
            if (!previewPath.empty() || previewBitmap) ClearFilePreview();
            return;
        }
        // Preview follows now-playing even while the Youtube browser or another folder is open.
        const auto active = ActivePreviewPath();
        if (active != previewPath || (IsVideoPath(active) && !previewWorker.joinable())) LoadFilePreview(active);
        else if (advanceVideo && previewIsVideo) UpdateVideoPreviewFrame();
        if (!previewIsVideo) previewFullscreen = false;
    }

void Win32Ui::Impl::SelectStudioSection(StudioSection section) {
        studioSection = section;
        studioHexEditing = false;
        studioHexSelectAll = false;
        if (section == StudioSection::Colors) {
            studioColorTarget = StudioColorTarget::Palette;
        } else if (section != StudioSection::Elements) {
            studioColorPickerVisible = false;
            studioColorTarget = StudioColorTarget::Palette;
        } else if (studioColorTarget == StudioColorTarget::Palette) {
            studioColorPickerVisible = false;
        }
        draggingStudioColor = false;
        draggingStudioHue = false;
        pickingScreenColor = false;
        eyedropperSkipUp = false;
        ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
    }

[[nodiscard]] skin::Color* Win32Ui::Impl::ActiveStudioColor() {
        switch (studioColorTarget) {
        case StudioColorTarget::Shape:
            if (studioDraft.shapes.empty()) return nullptr;
            studioShapeIndex = std::min(studioShapeIndex, studioDraft.shapes.size() - 1);
            return &studioDraft.shapes[studioShapeIndex].color;
        case StudioColorTarget::ImageTint:
            if (studioDraft.images.empty()) return nullptr;
            studioImageIndex = std::min(studioImageIndex, studioDraft.images.size() - 1);
            return &studioDraft.images[studioImageIndex].tint;
        case StudioColorTarget::Palette:
        default:
            return &(studioDraft.colors.*(StudioColorFields()[studioColorIndex].member));
        }
    }

void Win32Ui::Impl::OpenElementColorPicker(StudioColorTarget colorTarget) {
        studioColorTarget = colorTarget;
        studioColorPickerVisible = true;
        studioHexEditing = false;
        studioHexSelectAll = false;
        draggingStudioColor = false;
        draggingStudioHue = false;
        if (skin::Color* color = ActiveStudioColor()) {
            if (colorTarget == StudioColorTarget::Shape) {
                color->alpha = 255;
            } else if (colorTarget == StudioColorTarget::ImageTint && color->alpha == 0) {
                // Start with a visible accent so the first pick is not a no-op wash.
                color->red = 147;
                color->green = 87;
                color->blue = 255;
                color->alpha = 160;
            }
            studioHex = ToHexW(*color);
        }
        SelectStudioSection(StudioSection::Elements);
        InvalidateRect(window, nullptr, FALSE);
    }

void Win32Ui::Impl::DrawStudioColorPicker(float left, float right, float& y, const std::function<D2D1_RECT_F()>& row,
                               std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b,
                               std::uint64_t eyedropperAction, std::uint64_t hexAction) {
        skin::Color* selectedPtr = ActiveStudioColor();
        if (!selectedPtr) {
            studioColorPickerBounds = {};
            studioHueBounds = {};
            return;
        }
        skin::Color& selected = *selectedPtr;
        float hue{}, saturation{}, value{};
        ColorToHsv(selected, hue, saturation, value);
        studioColorPickerBounds = Rect(left, y, right, y + 68.0F);
        studioHueBounds = Rect(left, y + 73.0F, right, y + 85.0F);

        ComPtr<ID2D1GradientStopCollection> saturationStops;
        const D2D1_GRADIENT_STOP saturationGradient[] = {
            {0.0F, D2D1::ColorF(1.0F, 1.0F, 1.0F)},
            {1.0F, ToD2D(HsvColor(hue, 1.0F, 1.0F))},
        };
        ComPtr<ID2D1LinearGradientBrush> saturationBrush;
        if (SUCCEEDED(target->CreateGradientStopCollection(saturationGradient, 2,
                saturationStops.ReleaseAndGetAddressOf()))) {
            target->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    {studioColorPickerBounds.left, studioColorPickerBounds.top},
                    {studioColorPickerBounds.right, studioColorPickerBounds.top}),
                saturationStops.Get(), saturationBrush.ReleaseAndGetAddressOf());
        }
        if (saturationBrush) target->FillRectangle(studioColorPickerBounds, saturationBrush.Get());

        ComPtr<ID2D1GradientStopCollection> valueStops;
        const D2D1_GRADIENT_STOP valueGradient[] = {
            {0.0F, D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F)},
            {1.0F, D2D1::ColorF(0.0F, 0.0F, 0.0F, 1.0F)},
        };
        ComPtr<ID2D1LinearGradientBrush> valueBrush;
        if (SUCCEEDED(target->CreateGradientStopCollection(valueGradient, 2,
                D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                valueStops.ReleaseAndGetAddressOf()))) {
            target->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    {studioColorPickerBounds.left, studioColorPickerBounds.top},
                    {studioColorPickerBounds.left, studioColorPickerBounds.bottom}),
                valueStops.Get(), valueBrush.ReleaseAndGetAddressOf());
        }
        if (valueBrush) target->FillRectangle(studioColorPickerBounds, valueBrush.Get());
        target->DrawEllipse(D2D1::Ellipse(
            {studioColorPickerBounds.left + saturation * Width(studioColorPickerBounds),
             studioColorPickerBounds.top + (1.0F - value) * Height(studioColorPickerBounds)},
            4.0F, 4.0F), b[9].Get(), 1.5F);

        constexpr std::array<skin::Color, 7> hueColors{{
            {255, 0, 0}, {255, 255, 0}, {0, 255, 0}, {0, 255, 255},
            {0, 0, 255}, {255, 0, 255}, {255, 0, 0}}};
        std::array<D2D1_GRADIENT_STOP, hueColors.size()> hueGradient{};
        for (std::size_t index = 0; index < hueColors.size(); ++index) {
            hueGradient[index] = {static_cast<float>(index) /
                                  static_cast<float>(hueColors.size() - 1), ToD2D(hueColors[index])};
        }
        ComPtr<ID2D1GradientStopCollection> hueStops;
        ComPtr<ID2D1LinearGradientBrush> hueBrush;
        if (SUCCEEDED(target->CreateGradientStopCollection(hueGradient.data(),
                static_cast<UINT32>(hueGradient.size()), hueStops.ReleaseAndGetAddressOf()))) {
            target->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    {studioHueBounds.left, studioHueBounds.top},
                    {studioHueBounds.right, studioHueBounds.top}),
                hueStops.Get(), hueBrush.ReleaseAndGetAddressOf());
        }
        if (hueBrush) target->FillRectangle(studioHueBounds, hueBrush.Get());
        const float hueX = studioHueBounds.left + hue * Width(studioHueBounds);
        target->DrawRectangle(Rect(hueX - 2.0F, studioHueBounds.top - 1.0F,
                                   hueX + 2.0F, studioHueBounds.bottom + 1.0F), b[9].Get(), 1.0F);
        y += 92.0F;

        {
            const auto r = row();
            StudioButton(r, pickingScreenColor ? L"CLICK SCREEN..." : L"EYEDROPPER",
                         eyedropperAction, b, pickingScreenColor);
        }
        {
            const auto r = row();
            const auto box = r;
            DrawBevel(box, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
            std::wstring shown;
            if (studioHexEditing) {
                shown = studioHex;
                if ((GetTickCount64() / 500ULL) % 2ULL == 0ULL) shown += L"_";
            } else {
                const std::string hex = skin::FormatColor(selected);
                shown = std::wstring(hex.begin(), hex.end());
            }
            if (studioHexEditing && studioHexSelectAll) {
                target->FillRectangle(Rect(box.left + 3, box.top + 2, box.right - 3, box.bottom - 2),
                                      b[11].Get());
            }
            DrawText(shown, Rect(box.left + 5, box.top, box.right - 4, box.bottom),
                      studioHexSelectAll ? b[12].Get() : b[6].Get(),
                      regularFormat.Get());
            HitRegion hit;
            hit.bounds = box;
            hit.kind = HitKind::Studio;
            hit.id = hexAction;
            hits.push_back(hit);
        }
    }

[[nodiscard]] bool Win32Ui::Impl::CreateDeviceIndependentResources() {
        if (!d2dFactory) {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                         d2dFactory.ReleaseAndGetAddressOf()))) return false;
        }
        if (!writeFactory) {
            if (FAILED(DWriteCreateFactory(
                    DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(writeFactory.ReleaseAndGetAddressOf())))) return false;
        }
        if (!wicFactory) {
            // Non-fatal: skins without images still render. CoCreateInstance requires COM
            // initialized on this (window) thread, which App does at startup.
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(wicFactory.ReleaseAndGetAddressOf()));
        }
        if (!regularFormat) {
            auto create = [this](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                 ComPtr<IDWriteTextFormat>& format) {
                return writeFactory->CreateTextFormat(
                    family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    size, L"en-us", format.ReleaseAndGetAddressOf());
            };
            if (FAILED(create(L"Lucida Console", 11.0F, DWRITE_FONT_WEIGHT_NORMAL, regularFormat)) ||
                FAILED(create(L"MS Sans Serif", 9.0F, DWRITE_FONT_WEIGHT_NORMAL, smallFormat)) ||
                FAILED(create(L"Small Fonts", 8.0F, DWRITE_FONT_WEIGHT_NORMAL, tinyFormat)) ||
                 FAILED(create(L"MS Sans Serif", 11.0F, DWRITE_FONT_WEIGHT_BOLD, headingFormat)) ||
                 FAILED(create(L"Consolas", 30.0F, DWRITE_FONT_WEIGHT_BOLD, digitalFormat)) ||
                 FAILED(create(L"Segoe UI Symbol", 19.0F, DWRITE_FONT_WEIGHT_BOLD, studioIconFormat))) {
                return false;
            }
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            for (IDWriteTextFormat* format : {regularFormat.Get(), smallFormat.Get(), tinyFormat.Get(),
                                             headingFormat.Get(), digitalFormat.Get(), studioIconFormat.Get()}) {
                format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                format->SetTrimming(&trimming, nullptr);
            }
        }
        return true;
    }

[[nodiscard]] ComPtr<IDWriteFontCollection1> Win32Ui::Impl::CustomFontCollection(
        const std::filesystem::path& file) {
        ComPtr<IDWriteFontCollection1> collection;
        ComPtr<IDWriteFactory5> factory;
        ComPtr<IDWriteFontFile> fontFile;
        ComPtr<IDWriteFontSetBuilder1> builder;
        ComPtr<IDWriteFontSet> fontSet;
        if (file.empty() || FAILED(writeFactory.As(&factory)) ||
            FAILED(factory->CreateFontFileReference(file.c_str(), nullptr,
                                                    fontFile.ReleaseAndGetAddressOf())) ||
            FAILED(factory->CreateFontSetBuilder(builder.ReleaseAndGetAddressOf())) ||
            FAILED(builder->AddFontFile(fontFile.Get())) ||
            FAILED(builder->CreateFontSet(fontSet.ReleaseAndGetAddressOf())) ||
            FAILED(factory->CreateFontCollectionFromFontSet(
                fontSet.Get(), collection.ReleaseAndGetAddressOf()))) {
            return {};
        }
        return collection;
    }

[[nodiscard]] ComPtr<IDWriteTextFormat> Win32Ui::Impl::BuildTextFormat(
    const std::wstring& family, float size, DWRITE_FONT_WEIGHT weight,
    const std::filesystem::path& customFile) {
    ComPtr<IDWriteTextFormat> format;
    auto collection = CustomFontCollection(customFile);
    if (FAILED(writeFactory->CreateTextFormat(
            family.c_str(), collection.Get(), weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", format.ReleaseAndGetAddressOf()))) {
        return {};
    }
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetTrimming(&trimming, nullptr);
    return format;
}

[[nodiscard]] std::optional<std::wstring> Win32Ui::Impl::FontFamilyFromFile(
        const std::filesystem::path& file) {
        auto collection = CustomFontCollection(file);
        if (!collection || collection->GetFontFamilyCount() == 0) return std::nullopt;
        ComPtr<IDWriteFontFamily> fontFamily;
        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(collection->GetFontFamily(0, fontFamily.ReleaseAndGetAddressOf())) ||
            FAILED(fontFamily->GetFamilyNames(names.ReleaseAndGetAddressOf())) ||
            names->GetCount() == 0) return std::nullopt;
        UINT32 index = 0;
        BOOL exists = FALSE;
        names->FindLocaleName(L"en-us", &index, &exists);
        if (!exists) index = 0;
        UINT32 length = 0;
        if (FAILED(names->GetStringLength(index, &length))) return std::nullopt;
        std::wstring family(length + 1, L'\0');
        if (FAILED(names->GetString(index, family.data(), length + 1))) return std::nullopt;
        family.resize(length);
        return family;
    }

// Rebuilds UI text formats from active skin typography. Custom files use a private
    // DirectWrite collection because FR_PRIVATE fonts are absent from its system collection.
    void Win32Ui::Impl::ApplySkinFonts() {
        const auto& type = model.activeSkin.typography;
        std::wstring family(type.fontFamily.begin(), type.fontFamily.end());
        if (family.empty()) family = L"Segoe UI";
        std::wstring customFile;
        if (!type.customFontFile.empty() && !model.activeSkin.directory.empty()) {
            customFile = (model.activeSkin.directory / type.customFontFile).wstring();
        }
        const std::wstring signature =
            family + L"|" + customFile + L"|" + std::to_wstring(static_cast<int>(type.baseSize * 4.0F));
        if (signature == fontSignature && regularFormat) return;
        fontSignature = signature;

        const float base = std::clamp(type.baseSize, 8.0F, 32.0F);
        const std::filesystem::path customPath(customFile);
        auto create = [this, &family, &customPath](float size, DWRITE_FONT_WEIGHT weight,
                                      ComPtr<IDWriteTextFormat>& format) {
            if (auto built = BuildTextFormat(family, size, weight, customPath)) format = std::move(built);
        };
        create(base, DWRITE_FONT_WEIGHT_NORMAL, regularFormat);
        create(base - 2.0F, DWRITE_FONT_WEIGHT_NORMAL, smallFormat);
        create(std::max(7.0F, base - 3.0F), DWRITE_FONT_WEIGHT_NORMAL, tinyFormat);
        create(base, DWRITE_FONT_WEIGHT_BOLD, headingFormat);
        // Time readout uses the skin font family but a fixed size: skin baseSize must not
        // scale the elapsed-time text, which is sized to fit the scope box (~22pt).
        create(27.0F, DWRITE_FONT_WEIGHT_BOLD, digitalFormat);
    }

[[nodiscard]] bool Win32Ui::Impl::CreateTarget() {
        if (target) return true;
        if (!CreateDeviceIndependentResources() || !window) return false;
        RECT client{};
        GetClientRect(window, &client);
        const auto size = D2D1::SizeU(
            static_cast<UINT32>(std::max<LONG>(1, client.right - client.left)),
            static_cast<UINT32>(std::max<LONG>(1, client.bottom - client.top)));
        return SUCCEEDED(d2dFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(), D2D1::HwndRenderTargetProperties(window, size),
            target.ReleaseAndGetAddressOf()));
    }

void Win32Ui::Impl::DiscardTarget() noexcept {
        imageCache.clear();
        trackCoverCache.clear();
        trackCoverUseCounter = 0;
        nextTrackCoverLookup = {};
        previewBitmap.Reset();
        // D2D bitmaps are device-dependent. Force latest decoded frame to re-upload.
        uploadedPreviewFrameVersion = 0;
        for (auto& brush : solidBrushes) brush.Reset();
        decorBrush.Reset();
        visualizationRenderer.DiscardDeviceResources();
        target.Reset();
    }

void Win32Ui::Impl::SyncRefreshTimer() noexcept {
        if (!window || windowKind != WindowKind::Main) return;
        if (!IsWindowVisible(window) || IsIconic(window)) {
            if (currentTimerMs != 0) KillTimer(window, kRefreshTimer);
            currentTimerMs = 0;
            return;
        }
        // Video preview needs ~30 Hz while expanded even if transport is idle.
        const UINT desired = (model.playback == PlaybackState::Playing ||
                              (filePreviewExpanded && previewIsVideo) || previewFullscreen)
            ? kRefreshPlayingMilliseconds
            : kRefreshIdleMilliseconds;
        if (desired == currentTimerMs) return;
        currentTimerMs = desired;
        SetTimer(window, kRefreshTimer, currentTimerMs, nullptr);
    }

void Win32Ui::Impl::AddHit(const D2D1_RECT_F& bounds, Command command) {
        HitRegion hit;
        hit.bounds = bounds;
        hit.kind = HitKind::Command;
        hit.command = command;
        hits.push_back(hit);
    }

void Win32Ui::Impl::AddIdHit(const D2D1_RECT_F& bounds, HitKind kind, std::uint64_t id) {
        HitRegion hit;
        hit.bounds = bounds;
        hit.kind = kind;
        hit.id = id;
        hits.push_back(hit);
    }

void Win32Ui::Impl::AddSimpleHit(const D2D1_RECT_F& bounds, HitKind kind) {
        HitRegion hit;
        hit.bounds = bounds;
        hit.kind = kind;
        hits.push_back(hit);
    }

void Win32Ui::Impl::AddSettingHit(const D2D1_RECT_F& bounds, SettingCategory category) {
        HitRegion hit;
        hit.bounds = bounds;
        hit.kind = HitKind::Setting;
        hit.category = category;
        hits.push_back(hit);
    }

[[nodiscard]] const Win32Ui::Impl::HitRegion* Win32Ui::Impl::HitTest(float x, float y) const noexcept {
        for (auto iterator = hits.rbegin(); iterator != hits.rend(); ++iterator) {
            if (Contains(iterator->bounds, x, y)) return &*iterator;
        }
        return nullptr;
    }

void Win32Ui::Impl::DrawText(const std::wstring& textValue, const D2D1_RECT_F& bounds,
                  ID2D1Brush* brush, IDWriteTextFormat* format,
                   DWRITE_TEXT_ALIGNMENT alignment,
                   DWRITE_PARAGRAPH_ALIGNMENT vertical) {
        if (textValue.empty() || Width(bounds) <= 0.0F || Height(bounds) <= 0.0F) return;
        if (deferTexts) {
            deferredTexts.push_back({textValue, bounds, brush, format, alignment, vertical});
            return;
        }
        format->SetTextAlignment(alignment);
        format->SetParagraphAlignment(vertical);
        const float border = std::clamp(model.activeSkin.typography.borderSize, 0.0F, 8.0F);
        if (border > 0.0F && currentBrushes[3] != nullptr) {
            // 4 cardinal offsets: ~4x cheaper than 16-sample ring; outline still readable.
            static constexpr D2D1_POINT_2F kOutlineDirs[] = {
                {1.0F, 0.0F}, {-1.0F, 0.0F}, {0.0F, 1.0F}, {0.0F, -1.0F}};
            for (const auto& dir : kOutlineDirs) {
                const auto outline = Rect(bounds.left + dir.x * border, bounds.top + dir.y * border,
                                          bounds.right + dir.x * border, bounds.bottom + dir.y * border);
                target->DrawTextW(textValue.data(), static_cast<UINT32>(textValue.size()), format, outline,
                                  currentBrushes[3], D2D1_DRAW_TEXT_OPTIONS_CLIP,
                                  DWRITE_MEASURING_MODE_NATURAL);
            }
        }
        target->DrawTextW(textValue.data(), static_cast<UINT32>(textValue.size()), format, bounds,
                          brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        if (windowKind == WindowKind::Main && model.skinStudioVisible) {
            constexpr std::array<int, 14> brushToColor{
                0, 1, 2, 7, 7, 9, 5, 6, 5, 3, 4, 8, 3, 10};
            for (std::size_t index = 0; index < currentBrushes.size(); ++index) {
                if (brush == currentBrushes[index] && brushToColor[index] >= 0) {
                    colorFocusRegions.push_back({bounds, static_cast<std::size_t>(brushToColor[index])});
                    break;
                }
            }
        }
    }

// Horizontally scrolling single-line text, like an HTML <marquee> moving to the
    // right. The glyphs travel from the left edge toward the right and wrap around.
    void Win32Ui::Impl::DrawMarquee(const D2D1_RECT_F& bounds, const std::wstring& textValue, ID2D1Brush* brush) {
        if (textValue.empty() || Width(bounds) <= 0.0F || Height(bounds) <= 0.0F) return;
        float textWidth = Width(bounds);
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(writeFactory->CreateTextLayout(
                textValue.data(), static_cast<UINT32>(textValue.size()), regularFormat.Get(),
                100000.0F, Height(bounds), &layout))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) textWidth = metrics.width;
        }
        // Full travel span: text enters from left of view and exits at the right.
        const float span = Width(bounds) + textWidth;
        // Use millisecond phase directly. Dividing by 25 previously moved the text in
        // 25 ms steps, visibly quantizing it even when presentation reached 60 Hz.
        constexpr float kPixelsPerMillisecond = 0.04F;
        const float phase = static_cast<float>(GetTickCount64() % 10'000'000ULL) *
                            kPixelsPerMillisecond;
        const float travel = std::fmod(phase, span);
        const float x = bounds.left - textWidth + travel;
        target->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
        // Draw immediately: deferred text flushes after the clip is popped, letting
        // the scrolling glyphs spill over the marquee rectangle.
        const bool wasDeferred = deferTexts;
        deferTexts = false;
        DrawText(textValue, Rect(x, bounds.top, x + textWidth, bounds.bottom), brush,
                 regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING);
        deferTexts = wasDeferred;
        target->PopAxisAlignedClip();
    }

void Win32Ui::Impl::FlushDeferredTexts() {
        deferTexts = false;
        auto texts = std::move(deferredTexts);
        deferredTexts.clear();
        for (const auto& text : texts) {
            DrawText(text.value, text.bounds, text.brush, text.format, text.alignment, text.vertical);
        }
    }

void Win32Ui::Impl::DrawBevel(const D2D1_RECT_F& bounds, ID2D1Brush* fill, ID2D1Brush* light,
                   ID2D1Brush* dark, bool inset, float thickness) {
        const bool screen = registerScreenBounds && fill == currentBrushes[5];
        if (screen) {
            screenBounds.push_back(bounds);
        }
        target->FillRectangle(bounds, fill);
        ID2D1Brush* topLeft = inset ? dark : light;
        ID2D1Brush* bottomRight = inset ? light : dark;
        target->DrawLine({bounds.left, bounds.bottom}, {bounds.left, bounds.top}, topLeft, thickness);
        target->DrawLine({bounds.left, bounds.top}, {bounds.right, bounds.top}, topLeft, thickness);
        target->DrawLine({bounds.right, bounds.top}, {bounds.right, bounds.bottom}, bottomRight, thickness);
        target->DrawLine({bounds.right, bounds.bottom}, {bounds.left, bounds.bottom}, bottomRight, thickness);
    }

// Panels honor appearance toggles:
    //  * panelOpacity < 1 lets skin decor (images/shapes) show through the panel fill.
    //  * showTitleBars=false drops the raised metallic bar behind titles; the title text
    //    then sits directly on the panel background.
    //  * showPanelBorders=false removes the magnetic raised frame around each panel.
[[nodiscard]] D2D1_RECT_F Win32Ui::Impl::DrawPanel(const D2D1_RECT_F& bounds, const std::wstring& titleValue,
                                        ID2D1Brush* metal, ID2D1Brush* raised, ID2D1Brush* light,
                                         ID2D1Brush* dark, ID2D1Brush* green, ID2D1Brush* /*stripe*/) {
        panelBounds.push_back(bounds);
        const float opacity = std::clamp(model.activeSkin.appearance.panelOpacity, 0.0F, 1.0F);
        metal->SetOpacity(opacity);
        if (model.activeSkin.appearance.showPanelBorders) {
            DrawBevel(bounds, metal, light, dark, false, 2.0F);
        } else {
            target->FillRectangle(bounds, metal);
        }
        metal->SetOpacity(1.0F);
        const auto titleBar = Rect(bounds.left + 4, bounds.top + 4, bounds.right - 4, bounds.top + 22);
        if (model.activeSkin.appearance.showTitleBars) {
            DrawBevel(titleBar, raised, light, dark);
        }
        target->FillRectangle(Rect(titleBar.left + 4, titleBar.top + 5, titleBar.left + 9,
                                   titleBar.bottom - 5), green);
        const bool centered = model.activeSkin.appearance.centeredTitles;
        DrawText(titleValue, centered ? Rect(titleBar.left + 13, titleBar.top, titleBar.right - 13, titleBar.bottom)
                                      : Rect(titleBar.left + 13, titleBar.top, titleBar.right - 4, titleBar.bottom),
                 green, headingFormat.Get(), centered ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                      : DWRITE_TEXT_ALIGNMENT_LEADING);
        return Rect(bounds.left + 5, bounds.top + 25, bounds.right - 5, bounds.bottom - 5);
    }

// Transparent buttons drop the beveled metal background and use the larger regular
    // font so they read as plain text; the label brightens on hover / active. Classic
    // beveled buttons remain available when the skin disables transparent buttons.
    void Win32Ui::Impl::DrawButton(const D2D1_RECT_F& bounds, const wchar_t* label, Command command,
                     ID2D1Brush* fill, ID2D1Brush* hotFill, ID2D1Brush* light,
                     ID2D1Brush* dark, ID2D1Brush* textBrush, bool active) {
        const bool hot = Contains(bounds, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
        ID2D1Brush* stateFill = active ? currentBrushes[11] : currentBrushes[7];
        if (model.activeSkin.appearance.transparentButtons) {
            if (hot || active) target->FillRectangle(bounds, stateFill);
            const float previous = textBrush->GetOpacity();
            textBrush->SetOpacity((hot || active) ? 1.0F : 0.72F);
            DrawText(label, Rect(bounds.left + 2, bounds.top, bounds.right - 2, bounds.bottom),
                     textBrush, regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            textBrush->SetOpacity(previous);
        } else {
            DrawBevel(bounds, (hot || active) ? stateFill : fill, light, dark, active);
            DrawText(label, Rect(bounds.left + 2, bounds.top + 1, bounds.right - 2, bounds.bottom - 1),
                     textBrush, tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        (void)hotFill;
        AddHit(bounds, command);
    }

void Win32Ui::Impl::DrawStaticButton(const D2D1_RECT_F& bounds, const wchar_t* label,
                          ID2D1Brush* fill, ID2D1Brush* light, ID2D1Brush* dark,
                          ID2D1Brush* textBrush) {
        if (model.activeSkin.appearance.transparentButtons) {
            DrawText(label, bounds, textBrush, regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            DrawBevel(bounds, fill, light, dark);
            DrawText(label, bounds, textBrush, tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

void Win32Ui::Impl::DrawWindowButton(const D2D1_RECT_F& bounds, const wchar_t* label, std::uint64_t action,
                          ID2D1Brush* fill, ID2D1Brush* light, ID2D1Brush* dark,
                          ID2D1Brush* textBrush) {
        const bool hot = Contains(bounds, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
        DrawBevel(bounds, fill, hot ? textBrush : light, dark);
        DrawText(label, bounds, textBrush, tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        AddIdHit(bounds, HitKind::WindowControl, action);
    }

void Win32Ui::Impl::DrawSlider(const D2D1_RECT_F& bounds, float value, HitKind kind,
                    ID2D1Brush* screen, ID2D1Brush* green, ID2D1Brush* silver,
                    ID2D1Brush* light, ID2D1Brush* dark) {
        value = std::clamp(value, 0.0F, 1.0F);
        const float center = (bounds.top + bounds.bottom) * 0.5F;
        const auto trough = Rect(bounds.left, center - 3.0F, bounds.right, center + 3.0F);
        decorControlBounds.push_back(bounds);
        registerScreenBounds = false;
        DrawBevel(trough, screen, light, dark, true);
        registerScreenBounds = true;
        target->FillRectangle(Rect(trough.left + 2, center - 1, trough.left + 2 +
                                   std::max(0.0F, Width(trough) - 4) * value, center + 1), green);
        const float knobX = bounds.left + Width(bounds) * value;
        const auto knob = Rect(knobX - 5, bounds.top, knobX + 5, bounds.bottom);
        DrawBevel(knob, silver, light, dark);
        AddSimpleHit(bounds, kind);
    }

[[nodiscard]] bool Win32Ui::Impl::IsYoutubeBrowsingNow() {
        // Paint-time model can lag right after selecting Youtube; re-snapshot cheap fields.
        try {
            host.SnapshotUiModel(model);
        } catch (...) {
        }
        return model.youtubeBrowsing;
    }

void Win32Ui::Impl::ArmYoutubeSearchDebounce() {
        if (!window || windowKind != WindowKind::Main) return;
        if (!IsYoutubeBrowsingNow()) {
            KillTimer(window, kYoutubeSearchDebounceTimer);
            return;
        }
        KillTimer(window, kYoutubeSearchDebounceTimer);
        SetTimer(window, kYoutubeSearchDebounceTimer, kYoutubeSearchDebounceMs, nullptr);
    }

void Win32Ui::Impl::FlushYoutubeSearchDebounce() {
        if (!window) return;
        KillTimer(window, kYoutubeSearchDebounceTimer);
        if (playlistQuery.empty() || !IsYoutubeBrowsingNow()) return;
        try {
            host.SubmitYoutubeQuery(playlistQuery);
        } catch (...) {
        }
    }

void Win32Ui::Impl::DrawSearch(const D2D1_RECT_F& bounds, const std::wstring& query, SearchTarget search,
                    ID2D1Brush* screen, ID2D1Brush* light, ID2D1Brush* dark,
                    ID2D1Brush* green, ID2D1Brush* dim) {
        // SCREEN: Search text field.
        DrawBevel(bounds, screen, light, dark, true, 2.0F);
        const bool active = activeSearch == search;
        const bool selectAll = active && playlistQuerySelectAll && !query.empty();
        if (selectAll) {
            target->FillRectangle(Rect(bounds.left + 3, bounds.top + 2, bounds.right - 3,
                                       bounds.bottom - 2),
                                  currentBrushes[11]);
        }
        std::wstring shown = query;
        if (shown.empty() && !active) shown = L"Search title / artist / album...";
        if (active && !selectAll && ((GetTickCount64() / 500ULL) % 2ULL == 0ULL)) shown += L"_";
        DrawText(shown, Rect(bounds.left + 5, bounds.top, bounds.right - 4, bounds.bottom),
                 query.empty() && !active ? dim : (selectAll ? currentBrushes[12] : green),
                 regularFormat.Get());
        (void)search;
        AddSimpleHit(bounds, HitKind::PlaylistSearch);
    }

[[nodiscard]] const std::vector<const TrackView*>& Win32Ui::Impl::Filtered(const std::vector<TrackView>& source,
                                                                  const std::wstring& query) {
        if (cachedTrackRowsRevision == model.revision && cachedTrackRowsQuery == query) {
            return cachedTrackRows;
        }
        cachedTrackRowsRevision = model.revision;
        cachedTrackRowsQuery = query;
        cachedTrackRows.clear();
        cachedTrackRows.reserve(source.size());
        for (const auto& track : source) {
            if (Matches(track, query)) cachedTrackRows.push_back(&track);
        }
        return cachedTrackRows;
    }

// Position of a TrackView (borrowed from model.tracks) within that vector. Selection
    // and drag reorder key off this stable model index, not the filtered row index, so
    // duplicate entries and search filtering stay unambiguous.
[[nodiscard]] std::size_t Win32Ui::Impl::ModelTrackIndex(const TrackView* track) const noexcept {
        if (model.tracks.empty()) return static_cast<std::size_t>(-1);
        const auto* base = model.tracks.data();
        if (track < base || track >= base + model.tracks.size()) {
            return static_cast<std::size_t>(-1);
        }
        return static_cast<std::size_t>(track - base);
    }

void Win32Ui::Impl::DrawTrackRenameField(const D2D1_RECT_F& bounds, ID2D1Brush* textBrush) {
        Win32Ui::Impl::DrawBevel(bounds, currentBrushes[3], currentBrushes[4], currentBrushes[5], true);
        const auto textBounds = Rect(bounds.left + 4, bounds.top, bounds.right - 4, bounds.bottom);
        if (trackNameSelectAll) target->FillRectangle(textBounds, currentBrushes[11]);
        Win32Ui::Impl::DrawText(trackNameBuffer, textBounds, textBrush, regularFormat.Get());
        if (trackNameSelectAll) return;

        const std::size_t cursor = std::min(trackNameCursor, trackNameBuffer.size());
        float caretX = textBounds.left;
        if (writeFactory && regularFormat && !trackNameBuffer.empty()) {
            regularFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            regularFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(writeFactory->CreateTextLayout(
                    trackNameBuffer.data(), static_cast<UINT32>(trackNameBuffer.size()),
                    regularFormat.Get(), Width(textBounds), Height(textBounds), &layout))) {
                FLOAT x{};
                FLOAT y{};
                DWRITE_HIT_TEST_METRICS metrics{};
                if (SUCCEEDED(layout->HitTestTextPosition(static_cast<UINT32>(cursor), FALSE,
                                                          &x, &y, &metrics))) {
                    caretX = textBounds.left + x;
                }
            }
        }
        caretX = std::clamp(caretX, textBounds.left, textBounds.right - 1.0F);
        target->DrawLine({caretX, textBounds.top + 3.0F}, {caretX, textBounds.bottom - 3.0F},
                         textBrush, 1.0F);
    }

// Thin horizontal insertion bar drawn between rows while a track drag is active.
    void Win32Ui::Impl::DrawTrackDropIndicator(const D2D1_RECT_F& row, bool below, ID2D1Brush* brush) {
        const float y = below ? row.bottom : row.top;
        target->FillRectangle(Rect(row.left + 2, y - 1.0F, row.right - 2, y + 1.0F), brush);
    }

void Win32Ui::Impl::DrawTrackRows(const D2D1_RECT_F& bounds, const std::vector<const TrackView*>& tracks,
                       std::size_t& scroll, std::size_t& visibleRows,
                       ID2D1Brush* screen, ID2D1Brush* green, ID2D1Brush* greenDim,
                       ID2D1Brush* selection, ID2D1Brush* white, ID2D1Brush* dim,
                         bool showArtist) {
        (void)dim;  // Track lengths now share the title color; kept for signature parity.
        // SCREEN: Track list, including the < NO MATCHING TRACKS > state.
        if (screen == currentBrushes[5]) screenBounds.push_back(bounds);
        target->FillRectangle(bounds, screen);
        visibleRows = static_cast<std::size_t>(std::max(0.0F, std::floor(Height(bounds) / kTrackRowHeight)));
        const std::size_t maximum = tracks.size() > visibleRows ? tracks.size() - visibleRows : 0;
        scroll = std::min(scroll, maximum);
        const bool dragging = dragActive && dragKind == DragKind::Track;
        target->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
        for (std::size_t rowIndex = 0; rowIndex < visibleRows; ++rowIndex) {
            const std::size_t index = scroll + rowIndex;
            if (index >= tracks.size()) break;
            const auto& track = *tracks[index];
            const std::size_t modelIndex = ModelTrackIndex(tracks[index]);
            const bool selected = trackSelection.contains(modelIndex);
            const float top = bounds.top + static_cast<float>(rowIndex) * kTrackRowHeight;
            const auto row = Rect(bounds.left, top, bounds.right, top + kTrackRowHeight);
            const bool hot = Contains(row, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            if (selected || track.playing || hot) {
                target->FillRectangle(row, selected || track.playing ? selection
                                                                     : currentBrushes[7]);
            }
            if (track.playing) {
                Win32Ui::Impl::DrawText(L">", Rect(row.left + 2, row.top, row.left + 13, row.bottom), green,
                         regularFormat.Get());
            }
            const std::wstring number = std::to_wstring(index + 1) + L".";
            Win32Ui::Impl::DrawText(number, Rect(row.left + 13, row.top, row.left + 45, row.bottom),
                     track.playing ? white : greenDim, regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            std::wstring name = track.title.empty() ? L"Untitled" : track.title;
            if (showArtist && !track.artist.empty()) name = track.artist + L" - " + name;
            const auto nameBounds = Rect(row.left + 50, row.top, row.right - 82, row.bottom);
            if (trackNameEditing && trackRenameIndex == modelIndex) {
                DrawTrackRenameField(nameBounds, white);
            } else {
                Win32Ui::Impl::DrawText(name, nameBounds, selected || track.playing ? white : green,
                           regularFormat.Get());
            }
            Win32Ui::Impl::DrawText(FormatTime(track.durationSeconds), Rect(row.right - 78, row.top, row.right - 28, row.bottom),
                      selected || track.playing ? white : green, regularFormat.Get(),
                      DWRITE_TEXT_ALIGNMENT_TRAILING);
            Win32Ui::Impl::DrawTrackCover(track, row);
            HitRegion hit;
            hit.bounds = row;
            hit.kind = HitKind::Track;
            hit.id = track.id;
            hit.index = modelIndex;
            hits.push_back(hit);
            if (dragging && dropTrackIndex != static_cast<std::size_t>(-1)) {
                if (dropTrackIndex == modelIndex) DrawTrackDropIndicator(row, false, white);
                else if (dropTrackIndex == modelIndex + 1) DrawTrackDropIndicator(row, true, white);
            }
        }
        target->PopAxisAlignedClip();
        if (tracks.empty()) {
            Win32Ui::Impl::DrawText(L"< NO MATCHING TRACKS >", bounds, greenDim, regularFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

// Renders the current folder view: the selected folder's loose tracks first (no
    // header) followed by one separator header per subfolder section. Falls back to a
    // plain flat list when the model provides no sections. Search filters tracks by
    // title/artist/album and hides sections left empty.
    void Win32Ui::Impl::DrawSectionedTracks(const D2D1_RECT_F& bounds, std::size_t& scroll,
                             std::size_t& visibleRows,
                             ID2D1Brush* screen, ID2D1Brush* green, ID2D1Brush* greenDim,
                             ID2D1Brush* selection, ID2D1Brush* white, ID2D1Brush* dim) {
        if (model.trackSections.empty()) {
            const auto& filtered = Filtered(model.tracks, playlistQuery);
            Win32Ui::Impl::DrawTrackRows(bounds, filtered, scroll, visibleRows, screen, green, greenDim,
                          selection, white, dim, true);
            return;
        }
        // Flatten into a header/track row stream, honoring the active search filter.
        if (cachedSectionRowsRevision != model.revision || cachedSectionRowsQuery != playlistQuery) {
            cachedSectionRowsRevision = model.revision;
            cachedSectionRowsQuery = playlistQuery;
            cachedSectionRows.clear();
            cachedSectionRows.reserve(model.tracks.size() + model.trackSections.size());
            for (const auto& section : model.trackSections) {
                const std::size_t first = cachedSectionRows.size();
                const std::size_t last = std::min(section.first + section.count, model.tracks.size());
                for (std::size_t i = section.first; i < last; ++i) {
                    if (Matches(model.tracks[i], playlistQuery)) {
                        cachedSectionRows.push_back({false, {}, &model.tracks[i]});
                    }
                }
                if (cachedSectionRows.size() != first && !section.label.empty()) {
                    cachedSectionRows.insert(cachedSectionRows.begin() + static_cast<std::ptrdiff_t>(first),
                                             {true, section.label, nullptr});
                }
            }
        }
        const auto& rows = cachedSectionRows;

        if (screen == currentBrushes[5]) screenBounds.push_back(bounds);
        target->FillRectangle(bounds, screen);
        visibleRows = static_cast<std::size_t>(std::max(0.0F, std::floor(Height(bounds) / kTrackRowHeight)));
        const std::size_t maximum = rows.size() > visibleRows ? rows.size() - visibleRows : 0;
        scroll = std::min(scroll, maximum);
        target->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
        std::size_t trackNumber = 0;
        for (std::size_t prior = 0; prior < scroll; ++prior) {
            if (!rows[prior].header) ++trackNumber;
        }
        for (std::size_t rowIndex = 0; rowIndex < visibleRows; ++rowIndex) {
            const std::size_t index = scroll + rowIndex;
            if (index >= rows.size()) break;
            const auto& row = rows[index];
            const float top = bounds.top + static_cast<float>(rowIndex) * kTrackRowHeight;
            const auto rect = Rect(bounds.left, top, bounds.right, top + kTrackRowHeight);
            if (row.header) {
                const float mid = (rect.top + rect.bottom) * 0.5F;
                target->DrawLine({rect.left + 4, mid}, {rect.left + 20, mid}, greenDim, 1.0F);
                Win32Ui::Impl::DrawText(row.label, Rect(rect.left + 24, rect.top, rect.right - 24, rect.bottom),
                         greenDim, tinyFormat.Get());
                const float textWidth = std::min(Width(rect) * 0.5F, 8.0F * row.label.size());
                target->DrawLine({rect.left + 28 + textWidth, mid}, {rect.right - 4, mid}, greenDim, 1.0F);
                continue;
            }
            const auto& track = *row.track;
            const std::size_t modelIndex = ModelTrackIndex(row.track);
            const bool selected = trackSelection.contains(modelIndex);
            const bool hot = Contains(rect, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            if (selected || track.playing || hot) {
                target->FillRectangle(rect, selected || track.playing ? selection
                                                                      : currentBrushes[7]);
            }
            if (track.playing) {
                Win32Ui::Impl::DrawText(L">", Rect(rect.left + 2, rect.top, rect.left + 13, rect.bottom), green,
                         regularFormat.Get());
            }
            const std::wstring number = std::to_wstring(++trackNumber) + L".";
            Win32Ui::Impl::DrawText(number, Rect(rect.left + 13, rect.top, rect.left + 45, rect.bottom),
                     track.playing ? white : greenDim, regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            std::wstring name = track.title.empty() ? L"Untitled" : track.title;
            if (!track.artist.empty()) name = track.artist + L" - " + name;
            const auto nameBounds = Rect(rect.left + 50, rect.top, rect.right - 82, rect.bottom);
            if (trackNameEditing && trackRenameIndex == modelIndex) {
                DrawTrackRenameField(nameBounds, white);
            } else {
                Win32Ui::Impl::DrawText(name, nameBounds, selected || track.playing ? white : green,
                           regularFormat.Get());
            }
            Win32Ui::Impl::DrawText(FormatTime(track.durationSeconds), Rect(rect.right - 78, rect.top, rect.right - 28, rect.bottom),
                      selected || track.playing ? white : green, regularFormat.Get(),
                      DWRITE_TEXT_ALIGNMENT_TRAILING);
            Win32Ui::Impl::DrawTrackCover(track, rect);
            HitRegion hit;
            hit.bounds = rect;
            hit.kind = HitKind::Track;
            hit.id = track.id;
            hit.index = modelIndex;
            hits.push_back(hit);
            if (dragActive && dragKind == DragKind::Track &&
                dropTrackIndex != static_cast<std::size_t>(-1)) {
                if (dropTrackIndex == modelIndex) DrawTrackDropIndicator(rect, false, white);
                else if (dropTrackIndex == modelIndex + 1) DrawTrackDropIndicator(rect, true, white);
            }
        }
        target->PopAxisAlignedClip();
        if (rows.empty()) {
            Win32Ui::Impl::DrawText(L"< NO MATCHING TRACKS >", bounds, greenDim, regularFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

[[nodiscard]] std::wstring Win32Ui::Impl::SelectedPlaylistName() const {
        for (const auto& playlist : model.playlists) {
            if (playlist.selected) return playlist.name;
        }
        return L"PLAYLIST EDITOR";
    }

void Win32Ui::Impl::DrawPlayer(const D2D1_RECT_F& bounds,
                    std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        auto content = DrawPanel(bounds, L"RIVAN", b[1].Get(), b[2].Get(), b[3].Get(), b[4].Get(),
                                 b[13].Get(), b[7].Get());
        const auto titleBar = Rect(bounds.left + 4, bounds.top + 4, bounds.right - 4, bounds.top + 22);
        // Settings gear on the left of the RIVAN panel title bar opens the preferences window.
        const auto gear = Rect(titleBar.left + 2, titleBar.top + 2, titleBar.left + 22, titleBar.bottom - 2);
        {
            const bool hot = Contains(gear, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            Win32Ui::Impl::DrawText(L"\u2699", gear, hot ? b[8].Get() : b[13].Get(), headingFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            Win32Ui::Impl::AddHit(gear, Command::ToggleSettings);
        }
        float right = titleBar.right - 3;
        Win32Ui::Impl::DrawWindowButton(Rect(right - 16, titleBar.top + 2, right, titleBar.bottom - 2), L"X", 3,
                         b[2].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        right -= 19;
        Win32Ui::Impl::DrawWindowButton(Rect(right - 16, titleBar.top + 2, right, titleBar.bottom - 2), L"^", 2,
                         b[2].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        right -= 19;
        Win32Ui::Impl::DrawWindowButton(Rect(right - 16, titleBar.top + 2, right, titleBar.bottom - 2), L"_", 1,
                         b[2].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        // The strip between the gear and the window buttons is the window drag handle.
        captionRect = Rect(gear.right + 2, titleBar.top, right - 19, titleBar.bottom);

        const float bandTop = content.top + 3;
        const float bandBottom = std::min(content.top + 88, content.bottom - 88);
        const float bandHeight = std::max(1.0F, bandBottom - bandTop);
        // Two separate screens: a near-square scope box on the left (time + visualizer)
        // and a thin marquee strip on the right showing only the song title.
        const float scopeWidth = std::min(bandHeight * 1.25F, Width(content) * 0.42F);
        const auto scopeBox = Rect(content.left + 3, bandTop, content.left + 3 + scopeWidth, bandBottom);
        // SCREEN: Player scope (time + visualizer).
        Win32Ui::Impl::DrawBevel(scopeBox, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);

        // Clickable time readout; click toggles elapsed vs. remaining.
        const double remaining = std::max(0.0, model.durationSeconds - model.positionSeconds);
        const std::wstring timeText =
            (showRemaining ? L"-" + FormatTime(remaining) : FormatTime(model.positionSeconds));
        const auto timeRect = Rect(scopeBox.left + 6, scopeBox.top + 3, scopeBox.right - 6, scopeBox.top + 34);
        Win32Ui::Impl::DrawText(timeText, timeRect, b[6].Get(), digitalFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        Win32Ui::Impl::AddSimpleHit(timeRect, HitKind::TimeToggle);

        visualization::VisualizationPalette palette;
        // Scope DrawBevel already painted the screen fill. Re-filling here at the same
        // opacity double-blends the lower half and makes the visualizer brighter/more opaque
        // than the elapsed-time strip above it.
        palette.panel = D2D1::ColorF(0, 0, 0, 0);
        palette.grid = ToD2D(model.activeSkin.colors.accent);
        palette.waveform = ToD2D(model.activeSkin.colors.visualizationPrimary);
        palette.spectrum = ToD2D(model.activeSkin.colors.visualizationSecondary);
        const auto scope = Rect(scopeBox.left + 6, scopeBox.top + 36, scopeBox.right - 6, scopeBox.bottom - 4);
        visualizationRenderer.Draw(*target.Get(), scope, model.visualization, palette);

        // Thin marquee strip: song title scrolling to the right, like <marquee>.
        const float stripHeight = std::min(26.0F, bandHeight);
        const auto marqueeStrip = Rect(scopeBox.right + 6, bandTop, content.right - 3,
                                       bandTop + stripHeight);
        // SCREEN: Song-title marquee.
        Win32Ui::Impl::DrawBevel(marqueeStrip, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
        Win32Ui::Impl::DrawMarquee(Rect(marqueeStrip.left + 4, marqueeStrip.top + 2, marqueeStrip.right - 4,
                         marqueeStrip.bottom - 2),
                    model.nowTitle.empty() ? L"RIVAN - READY" : model.nowTitle, b[6].Get());

        const float seekTop = bandBottom + 4;
        Win32Ui::Impl::DrawText(L"POS", Rect(content.left + 4, seekTop, content.left + 30, seekTop + 17),
                 b[13].Get(), tinyFormat.Get());
        const float progress = model.durationSeconds > 0.0
            ? static_cast<float>(model.positionSeconds / model.durationSeconds) : 0.0F;
        Win32Ui::Impl::DrawSlider(Rect(content.left + 32, seekTop + 1, content.right - 4, seekTop + 16), progress,
                   HitKind::Seek, b[5].Get(), b[13].Get(), b[2].Get(), b[3].Get(), b[4].Get());

        const float volumeTop = seekTop + 20;
        Win32Ui::Impl::DrawText(L"VOL", Rect(content.left + 4, volumeTop, content.left + 31, volumeTop + 16),
                 b[13].Get(), tinyFormat.Get());
        Win32Ui::Impl::DrawSlider(Rect(content.left + 32, volumeTop, content.left + Width(content) * 0.55F,
                        volumeTop + 15), model.volume, HitKind::Volume, b[5].Get(), b[13].Get(),
                   b[2].Get(), b[3].Get(), b[4].Get());
        Win32Ui::Impl::DrawText(L"BAL", Rect(content.left + Width(content) * 0.57F, volumeTop,
                              content.left + Width(content) * 0.64F, volumeTop + 16),
                 b[13].Get(), tinyFormat.Get());
        const auto balance = Rect(content.left + Width(content) * 0.65F, volumeTop,
                                  content.right - 4, volumeTop + 15);
        decorControlBounds.push_back(balance);
        registerScreenBounds = false;
        Win32Ui::Impl::DrawBevel(Rect(balance.left, volumeTop + 5, balance.right, volumeTop + 10),
                   b[5].Get(), b[3].Get(), b[4].Get(), true);
        registerScreenBounds = true;
        Win32Ui::Impl::DrawBevel(Rect((balance.left + balance.right) * 0.5F - 5, volumeTop,
                       (balance.left + balance.right) * 0.5F + 5, volumeTop + 15),
                  b[2].Get(), b[3].Get(), b[4].Get());

        const float buttonTop = content.bottom - 27;
        const float buttonWidth = std::clamp((Width(content) - 150.0F) / 4.0F, 31.0F, 44.0F);
        float x = content.left + 3;
        Win32Ui::Impl::DrawButton(Rect(x, buttonTop, x + buttonWidth, content.bottom - 2), L"|<<", Command::Previous,
                   b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        x += buttonWidth + 3;
        Win32Ui::Impl::DrawButton(Rect(x, buttonTop, x + buttonWidth, content.bottom - 2),
                   model.playback == PlaybackState::Playing ? L"||" : L">", Command::PlayPause,
                   b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get(),
                   model.playback == PlaybackState::Playing);
        x += buttonWidth + 3;
        Win32Ui::Impl::DrawButton(Rect(x, buttonTop, x + buttonWidth, content.bottom - 2), L"[]", Command::Stop,
                   b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        x += buttonWidth + 3;
        Win32Ui::Impl::DrawButton(Rect(x, buttonTop, x + buttonWidth, content.bottom - 2), L">>|", Command::Next,
                   b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawButton(Rect(content.right - 139, buttonTop, content.right - 72, content.bottom - 2),
                   L"SHUFFLE", Command::ToggleShuffle, b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(),
                   b[13].Get(), model.shuffle);
        Win32Ui::Impl::DrawButton(Rect(content.right - 68, buttonTop, content.right - 2, content.bottom - 2),
                   RepeatLabel(model.repeat), Command::CycleRepeat, b[2].Get(), b[1].Get(), b[3].Get(),
                   b[4].Get(), b[13].Get(),
                   model.repeat != RepeatMode::Off);
    }

void Win32Ui::Impl::DrawPlaylistEditor(const D2D1_RECT_F& bounds,
                            std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        auto content = DrawPanel(bounds, SelectedPlaylistName(), b[1].Get(), b[2].Get(), b[3].Get(),
                                 b[4].Get(), b[13].Get(), b[7].Get());
        const auto controls = Rect(content.left + 2, std::max(content.top, content.bottom - 28),
                                   content.right - 2, content.bottom - 2);
        playlistListBounds = Rect(content.left + 2, content.top + 2, content.right - 2,
                                  std::max(content.top + 2, controls.top - 2));
        auto tracks = Filtered(model.tracks, L"");
        // SCREEN: Playlist Editor track list.
        Win32Ui::Impl::DrawTrackRows(playlistListBounds, tracks, playlistScroll, playlistRows, b[5].Get(), b[6].Get(),
                       b[6].Get(), b[11].Get(), b[12].Get(), b[10].Get(), true);

        // ADD imports files into user playlists and library folders. REM removes entries
        // only from user playlists.
        // Hits always register so a click on a grayed button can show the hint dialog
        // instead of feeling dead; handlers gate the real work.
        const bool editable = model.selectedPlaylistIsUser;
        const float buttonW = 42.0F;
        float x = controls.left;
        const auto editorButton = [&](const wchar_t* label, HitKind kind, bool enabled) {
            const auto rect = Rect(x, controls.top + 2, x + buttonW, controls.bottom);
            const bool hot =
                Contains(rect, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            Win32Ui::Impl::DrawBevel(rect, (enabled && hot) ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get());
            Win32Ui::Impl::DrawText(label, rect, enabled ? b[9].Get() : b[10].Get(), tinyFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            Win32Ui::Impl::AddSimpleHit(rect, kind);
            x += buttonW + 3;
        };
        editorButton(L"ADD", HitKind::EditorAdd, model.selectedPlaylistCanAdd);
        editorButton(L"REM", HitKind::EditorRemove, editable && !trackSelection.empty());
        if (cachedPlaylistDurationRevision != model.revision) {
            cachedPlaylistDurationRevision = model.revision;
            cachedPlaylistDuration = 0.0;
            for (const auto& track : model.tracks) {
                cachedPlaylistDuration += std::max(0.0, track.durationSeconds);
            }
        }
        const std::wstring status = FormatTime(model.positionSeconds) + L" / " +
                                    FormatTime(cachedPlaylistDuration);
        // SCREEN: Playlist playback-time status.
        Win32Ui::Impl::DrawBevel(Rect(std::max(x, controls.right - 104), controls.top + 2, controls.right, controls.bottom),
                  b[5].Get(), b[3].Get(), b[4].Get(), true);
        Win32Ui::Impl::DrawText(status, Rect(std::max(x, controls.right - 101), controls.top + 2,
                              controls.right - 3, controls.bottom), b[6].Get(), tinyFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }

void Win32Ui::Impl::DrawEqualizer(const D2D1_RECT_F& bounds,
                       std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        auto content = DrawPanel(bounds, L"GRAPHIC EQUALIZER", b[1].Get(), b[2].Get(), b[3].Get(),
                                 b[4].Get(), b[13].Get(), b[7].Get());
        Win32Ui::Impl::DrawStaticButton(Rect(content.left + 2, content.top + 2, content.left + 31, content.top + 20),
                         L"ON", b[2].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawStaticButton(Rect(content.left + 35, content.top + 2, content.left + 77, content.top + 20),
                         L"AUTO", b[2].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawText(L"PREAMP", Rect(content.left + 2, content.top + 22, content.left + 54, content.top + 36),
                 b[10].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);

        constexpr std::array<const wchar_t*, 10> bands{
            L"60", L"170", L"310", L"600", L"1K", L"3K", L"6K", L"12K", L"14K", L"16K"};
        const float plotLeft = content.left + 56;
        const float slot = std::max(18.0F, Width(Rect(plotLeft, 0, content.right - 3, 0)) / 10.0F);
        const float trackTop = content.top + 5;
        const float trackBottom = content.bottom - 17;
        for (std::size_t index = 0; index < bands.size(); ++index) {
            const float center = plotLeft + slot * (static_cast<float>(index) + 0.5F);
            const auto trough = Rect(center - 2, trackTop, center + 2, trackBottom);
            decorControlBounds.push_back(Rect(center - slot * 0.5F, trackTop,
                                               center + slot * 0.5F, trackBottom));
            registerScreenBounds = false;
            Win32Ui::Impl::DrawBevel(trough, b[5].Get(), b[3].Get(), b[4].Get(), true);
            registerScreenBounds = true;
            float magnitude = 0.5F;
            if (!model.visualization.spectrum.empty()) {
                const std::size_t bin = std::min(model.visualization.spectrum.size() - 1,
                    index * model.visualization.spectrum.size() / bands.size());
                magnitude = 0.25F + std::clamp(model.visualization.spectrum[bin], 0.0F, 1.0F) * 0.6F;
            }
            const float y = trackBottom - magnitude * (trackBottom - trackTop);
            Win32Ui::Impl::DrawBevel(Rect(center - 6, y - 3, center + 6, y + 3), b[2].Get(), b[3].Get(), b[4].Get());
            Win32Ui::Impl::DrawText(bands[index], Rect(center - slot * 0.5F, content.bottom - 16,
                                        center + slot * 0.5F, content.bottom), b[8].Get(), tinyFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

void Win32Ui::Impl::DrawLibrary(const D2D1_RECT_F& bounds,
                     std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        auto content = DrawPanel(bounds, L"RIVAN LIBRARY", b[1].Get(), b[2].Get(), b[3].Get(), b[4].Get(),
                                 b[13].Get(), b[7].Get());
        const float treeWidth = std::clamp(Width(content) * 0.30F, 130.0F, 215.0F);
        const auto tree = Rect(content.left + 2, content.top + 2, content.left + treeWidth,
                               content.bottom - 2);
        // SCREEN: Playlist navigation tree.
        Win32Ui::Impl::DrawBevel(tree, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
        Win32Ui::Impl::DrawText(L"[-] MUSIC", Rect(tree.left + 7, tree.top + 4, tree.right - 48, tree.top + 24),
                 b[6].Get(), regularFormat.Get());
        // New-playlist (+) button, left of the refresh glyph. Starts the inline name editor.
        const auto newPlaylist = Rect(tree.right - 46, tree.top + 4, tree.right - 26, tree.top + 24);
        const bool newHot = Contains(newPlaylist, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
        if (newHot) target->FillRectangle(newPlaylist, b[7].Get());
        Win32Ui::Impl::DrawText(L"+", newPlaylist, newHot ? b[12].Get() : b[6].Get(), regularFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        Win32Ui::Impl::AddSimpleHit(newPlaylist, HitKind::NewPlaylist);
        newPlaylistButtonBounds = newPlaylist;
        // Manual library refresh button, right of the header. Circular-arrow glyph.
        const auto refresh = Rect(tree.right - 24, tree.top + 4, tree.right - 4, tree.top + 24);
        const bool refreshHot = Contains(refresh, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
        if (refreshHot) target->FillRectangle(refresh, b[7].Get());
        // Circular-arrow glyph sits low in this font; lift its draw bounds to match plus.
        const auto refreshGlyph = Rect(refresh.left, refresh.top - 1, refresh.right, refresh.bottom - 1);
        Win32Ui::Impl::DrawText(L"\u21BB", refreshGlyph, refreshHot ? b[12].Get() : b[6].Get(), regularFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        Win32Ui::Impl::AddSimpleHit(refresh, HitKind::Refresh);

        const auto treeList = Rect(tree.left + 2, tree.top + 27, tree.right - 2, tree.bottom - 2);
        target->PushAxisAlignedClip(treeList, D2D1_ANTIALIAS_MODE_ALIASED);
        constexpr float rowH = 20.0F;
        const std::size_t visibleTreeRows = static_cast<std::size_t>(
            std::max(0.0F, std::floor(Height(treeList) / rowH)));
        const std::size_t treeMax = model.playlists.size() > visibleTreeRows
            ? model.playlists.size() - visibleTreeRows : 0;
        treeScroll = std::min(treeScroll, treeMax);
        for (std::size_t rowIndex = 0; rowIndex < visibleTreeRows; ++rowIndex) {
            const std::size_t index = treeScroll + rowIndex;
            if (index >= model.playlists.size()) break;
            const auto& playlist = model.playlists[index];
            const float top = treeList.top + static_cast<float>(rowIndex) * rowH;
            const auto row = Rect(treeList.left + 2, top, treeList.right - 2, top + rowH);
            const bool hot = Contains(row, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            // A playlist row highlights when it is the opened selection or part of a
            // multi-selection (ctrl/shift on user playlists, for reorder / delete).
            const bool multiSelected = playlistSelection.contains(playlist.id);
            const bool highlight = playlist.selected || multiSelected;
            const bool editingThisRow = playlistNameEditing && playlistNameRenaming &&
                                        playlistRenameId == playlist.id;
            if (highlight || hot) {
                target->FillRectangle(row, highlight ? b[11].Get() : b[7].Get());
            }
            ID2D1Brush* textBrush = highlight ? b[12].Get() : b[6].Get();
            const float indent = static_cast<float>(playlist.depth) * 12.0F;
            // Row hit first so the triangle hit (added last) wins inside its sub-rect;
            // HitTest scans in reverse, so later hits take priority. index carries the
            // tree row so shift-range playlist selection can resolve without a lookup.
            {
                HitRegion hit;
                hit.bounds = row;
                hit.kind = HitKind::Playlist;
                hit.id = playlist.id;
                hit.index = index;
                hits.push_back(hit);
            }
            // Expand/collapse triangle only for collapsible folders; leaves and All Music
            // get none. Clicking the triangle toggles; clicking the row selects.
            const auto twist = Rect(row.left + 2 + indent, row.top, row.left + 17 + indent, row.bottom);
            if (playlist.collapsible) {
                Win32Ui::Impl::DrawText(playlist.expanded ? L"\u25BE" : L"\u25B8", twist, textBrush, regularFormat.Get());
                Win32Ui::Impl::AddIdHit(twist, HitKind::PlaylistToggle, playlist.id);
            } else if (!playlist.allMusic && !playlist.youtube) {
                Win32Ui::Impl::DrawText(L"\u2022", twist, textBrush, tinyFormat.Get());
            }
            if (editingThisRow) {
                // Inline rename field replaces the name for the row being renamed.
                const auto field = Rect(row.left + 17 + indent, row.top + 1, row.right - 3, row.bottom - 1);
                Win32Ui::Impl::DrawBevel(field, b[5].Get(), b[3].Get(), b[4].Get(), true);
                const bool caret = (GetTickCount64() / 500ULL) % 2ULL == 0ULL;
                Win32Ui::Impl::DrawText(playlistNameBuffer + (caret ? L"_" : L""),
                         Rect(field.left + 4, field.top, field.right - 4, field.bottom),
                         b[6].Get(), regularFormat.Get());
            } else {
                Win32Ui::Impl::DrawText(playlist.name, Rect(row.left + 17 + indent, row.top, row.right - 32, row.bottom),
                         textBrush, regularFormat.Get());
                Win32Ui::Impl::DrawText(std::to_wstring(playlist.trackCount),
                         Rect(row.right - 29, row.top, row.right - 3, row.bottom),
                         b[8].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
            // Playlist drag targets: a highlighted row accepts a child folder; lines
            // continue to show sibling insertion points.
            if (dragActive && dragKind == DragKind::Playlist) {
                if (dropIntoPlaylistId == playlist.id) {
                    target->DrawRectangle(row, b[12].Get(), 1.0F);
                } else if (dropBeforePlaylistId == playlist.id) {
                    target->FillRectangle(Rect(row.left, row.top - 1.0F, row.right, row.top + 1.0F),
                                          b[12].Get());
                } else if (dropAtPlaylistEnd && index + 1 == model.playlists.size()) {
                    target->FillRectangle(Rect(row.left, row.bottom - 1.0F, row.right, row.bottom + 1.0F),
                                          b[12].Get());
                }
            }
        }
        target->PopAxisAlignedClip();
        treeListBounds = treeList;

        // Inline create field: when creating a new playlist, show a text row under the
        // header so the user can type the name before it exists in the tree.
        if (playlistNameEditing && !playlistNameRenaming) {
            const auto field = Rect(tree.left + 4, tree.top + 27, tree.right - 4, tree.top + 47);
            Win32Ui::Impl::DrawBevel(field, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
            const bool caret = (GetTickCount64() / 500ULL) % 2ULL == 0ULL;
            const std::wstring shown = playlistNameBuffer.empty() && !caret
                ? L"New playlist name..."
                : playlistNameBuffer + (caret ? L"_" : L"");
            Win32Ui::Impl::DrawText(shown, Rect(field.left + 5, field.top, field.right - 5, field.bottom),
                     playlistNameBuffer.empty() ? b[10].Get() : b[6].Get(), regularFormat.Get());
        }

        const float rightLeft = tree.right + 5;
        const auto right = Rect(rightLeft, content.top + 2, content.right - 2, content.bottom - 2);

        if (model.youtubeBrowsing) {
            Win32Ui::Impl::DrawText(L"YOUTUBE — SEARCH OR PASTE URL", Rect(right.left, right.top, right.right,
                                                            right.top + 18),
                     b[8].Get(), tinyFormat.Get());
            // Source toggle above search: YouTube vs YouTube Music (catalog audio + covers).
            const float sourceMid = right.left + Width(right) * 0.5F;
            const auto ytBtn = Rect(right.left, right.top + 19, sourceMid - 2.0F, right.top + 39);
            const auto ytmBtn = Rect(sourceMid + 2.0F, right.top + 19, right.right, right.top + 39);
            const bool ytHot =
                Contains(ytBtn, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            const bool ytmHot =
                Contains(ytmBtn, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            const bool ytOn = !model.youtubeMusicSearch;
            const bool ytmOn = model.youtubeMusicSearch;
            Win32Ui::Impl::DrawBevel(ytBtn, (ytOn || ytHot) ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get(),
                      ytOn);
            Win32Ui::Impl::DrawBevel(ytmBtn, (ytmOn || ytmHot) ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get(),
                      ytmOn);
            Win32Ui::Impl::DrawText(L"YOUTUBE", ytBtn, ytOn ? b[9].Get() : b[10].Get(), tinyFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            Win32Ui::Impl::DrawText(L"YOUTUBE MUSIC", ytmBtn, ytmOn ? b[9].Get() : b[10].Get(), tinyFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            Win32Ui::Impl::AddIdHit(ytBtn, HitKind::SettingsAction, 53);
            Win32Ui::Impl::AddIdHit(ytmBtn, HitKind::SettingsAction, 54);

            const auto localSearch =
                Rect(right.left, right.top + 43, right.right - 72, right.top + 67);
            Win32Ui::Impl::DrawSearch(localSearch, playlistQuery, SearchTarget::Playlist, b[5].Get(), b[3].Get(),
                       b[4].Get(), b[6].Get(), b[6].Get());
            const auto go = Rect(right.right - 68, right.top + 43, right.right, right.top + 67);
            const bool goHot = Contains(go, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            Win32Ui::Impl::DrawBevel(go, goHot ? b[7].Get() : b[2].Get(), b[3].Get(), b[4].Get(), false);
            Win32Ui::Impl::DrawText(L"GO", go, b[9].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            Win32Ui::Impl::AddIdHit(go, HitKind::SettingsAction, 50);

            const auto statusLine =
                Rect(right.left, localSearch.bottom + 3, right.right - 120, localSearch.bottom + 20);
            Win32Ui::Impl::DrawText(model.youtubeStatus.empty() ? L" " : model.youtubeStatus, statusLine,
                     model.youtubeBusy ? b[8].Get() : b[10].Get(), tinyFormat.Get());
            if (model.youtubePageCount > 1) {
                const auto prev = Rect(right.right - 116, localSearch.bottom + 2,
                                       right.right - 60, localSearch.bottom + 21);
                const auto next = Rect(right.right - 56, localSearch.bottom + 2, right.right,
                                       localSearch.bottom + 21);
                const bool prevHot =
                    Contains(prev, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
                const bool nextHot =
                    Contains(next, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
                Win32Ui::Impl::DrawBevel(prev, (prevHot && model.youtubeCanPagePrev) ? b[7].Get() : b[2].Get(),
                          b[3].Get(), b[4].Get(), false);
                Win32Ui::Impl::DrawBevel(next, (nextHot && model.youtubeCanPageNext) ? b[7].Get() : b[2].Get(),
                          b[3].Get(), b[4].Get(), false);
                Win32Ui::Impl::DrawText(L"PREV", prev,
                         model.youtubeCanPagePrev ? b[9].Get() : b[10].Get(), tinyFormat.Get(),
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                Win32Ui::Impl::DrawText(L"NEXT", next,
                         model.youtubeCanPageNext ? b[9].Get() : b[10].Get(), tinyFormat.Get(),
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                if (model.youtubeCanPagePrev) AddIdHit(prev, HitKind::SettingsAction, 51);
                if (model.youtubeCanPageNext) AddIdHit(next, HitKind::SettingsAction, 52);
            }

            playlistSearchBounds =
                Rect(right.left, statusLine.bottom + 2, right.right, right.bottom);
            Win32Ui::Impl::DrawBevel(playlistSearchBounds, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
            target->PushAxisAlignedClip(playlistSearchBounds, D2D1_ANTIALIAS_MODE_ALIASED);
            constexpr float ytRowH = 22.0F;
            playlistSearchRows = static_cast<std::size_t>(
                std::max(0.0F, std::floor(Height(playlistSearchBounds) / ytRowH)));
            const std::size_t maxScroll =
                model.youtubeResults.size() > playlistSearchRows
                    ? model.youtubeResults.size() - playlistSearchRows
                    : 0;
            playlistSearchScroll = std::min(playlistSearchScroll, maxScroll);
            for (std::size_t rowIndex = 0; rowIndex < playlistSearchRows; ++rowIndex) {
                const std::size_t index = playlistSearchScroll + rowIndex;
                if (index >= model.youtubeResults.size()) break;
                const auto& item = model.youtubeResults[index];
                const float top =
                    playlistSearchBounds.top + 2 + static_cast<float>(rowIndex) * ytRowH;
                const auto row = Rect(playlistSearchBounds.left + 2, top,
                                      playlistSearchBounds.right - 2, top + ytRowH);
                const bool hot =
                    Contains(row, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
                if (item.selected || hot) {
                    target->FillRectangle(row, item.selected ? b[11].Get() : b[7].Get());
                }
                ID2D1Brush* textBrush = item.selected ? b[12].Get() : b[6].Get();
                std::wstring mark = L"\u2022";
                float markRight = row.left + 20.0F;
                if (item.downloading) {
                    if (item.downloadProgress >= 0.0F) {
                        const int pct = static_cast<int>(item.downloadProgress + 0.5F);
                        wchar_t pctBuf[8]{};
                        swprintf_s(pctBuf, L"%d%%", pct);
                        mark = pctBuf;
                    } else {
                        mark = L"...";
                    }
                    markRight = row.left + 36.0F;
                } else if (item.ready) {
                    mark = L"\u266A";
                } else if (item.failed) {
                    mark = L"!";
                }
                Win32Ui::Impl::DrawText(mark, Rect(row.left + 4, row.top, markRight, row.bottom), textBrush,
                         item.downloading ? tinyFormat.Get() : regularFormat.Get());
                Win32Ui::Impl::DrawText(item.title, Rect(markRight + 2, row.top, row.right - 48, row.bottom),
                         textBrush, regularFormat.Get());
                if (item.durationSeconds > 0.0) {
                    const int total = static_cast<int>(item.durationSeconds + 0.5);
                    const int minutes = total / 60;
                    const int seconds = total % 60;
                    wchar_t timeBuf[16]{};
                    swprintf_s(timeBuf, L"%d:%02d", minutes, seconds);
                    Win32Ui::Impl::DrawText(timeBuf, Rect(row.right - 46, row.top, row.right - 4, row.bottom),
                             b[8].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
                }
                Win32Ui::Impl::AddIdHit(row, HitKind::YoutubeResult, item.id);
            }
            target->PopAxisAlignedClip();
            return;
        }

        Win32Ui::Impl::DrawText(L"CURRENT FOLDER / PLAYLIST", Rect(right.left, right.top, right.right, right.top + 18),
                 b[8].Get(), tinyFormat.Get());
        const auto localSearch = Rect(right.left, right.top + 19, right.right, right.top + 43);
        // SCREEN: Current-folder search field.
        Win32Ui::Impl::DrawSearch(localSearch, playlistQuery, SearchTarget::Playlist, b[5].Get(), b[3].Get(), b[4].Get(),
                    b[6].Get(), b[6].Get());
        // Sticky file preview at bottom of CURRENT FOLDER; track list scrolls above it.
        constexpr float previewHandleHeight = 18.0F;
        const float previewHeight = model.filePreviewEnabled && filePreviewExpanded
                                        ? std::clamp(Height(right) * 0.34F, 130.0F, 260.0F)
                                        : 0.0F;
        const float previewTop = right.bottom - previewHandleHeight - previewHeight;
        playlistSearchBounds = Rect(right.left, localSearch.bottom + 3, right.right,
                                    model.filePreviewEnabled ? previewTop : right.bottom);
        // SCREEN: Current-folder track list with per-subfolder section headers.
        Win32Ui::Impl::DrawSectionedTracks(playlistSearchBounds, playlistSearchScroll, playlistSearchRows,
                            b[5].Get(), b[6].Get(), b[6].Get(), b[11].Get(), b[12].Get(), b[10].Get());
        previewVideoBounds = {};
        if (model.filePreviewEnabled) {
            const auto handle = Rect(right.left + Width(right) * 0.40F, previewTop,
                                     right.left + Width(right) * 0.60F,
                                     previewTop + previewHandleHeight);
            Win32Ui::Impl::DrawBevel(handle, b[2].Get(), b[3].Get(), b[4].Get(), false);
            Win32Ui::Impl::DrawText(filePreviewExpanded ? L"\u25BC" : L"\u25B2", handle, b[9].Get(),
                     tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            Win32Ui::Impl::AddSimpleHit(handle, HitKind::FilePreviewToggle);
            if (filePreviewExpanded) {
                const auto preview = Rect(right.left, handle.bottom, right.right, right.bottom);
                Win32Ui::Impl::DrawBevel(preview, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
                previewVideoBounds = Rect(preview.left + 3, preview.top + 3,
                                          preview.right - 3, preview.bottom - 3);
                if (previewBitmap) {
                    Win32Ui::Impl::DrawPreviewBitmap(previewVideoBounds);
                    if (previewIsVideo) {
                        Win32Ui::Impl::AddSimpleHit(previewVideoBounds, HitKind::FilePreviewFullscreen);
                    }
                } else {
                    const wchar_t* message = L"NOTHING PLAYING";
                    if (!ActivePreviewPath().empty()) {
                        message = previewIsVideo ? L"LOADING PREVIEW..." : L"NO COVER AVAILABLE";
                    }
                    Win32Ui::Impl::DrawText(message, previewVideoBounds, b[10].Get(), smallFormat.Get(),
                             DWRITE_TEXT_ALIGNMENT_CENTER);
                }
            }
        }
    }

void Win32Ui::Impl::DrawMini(const D2D1_SIZE_F size,
                   std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        target->FillRectangle(Rect(0, 0, size.width, size.height), b[0].Get());
        Win32Ui::Impl::DrawSkinDecor(size);
        screenBounds.clear();
        panelBounds.clear();
        decorControlBounds.clear();
        deferTexts = true;
        const auto bounds = Rect(4, 4, size.width - 4, size.height - 4);
        auto content = DrawPanel(bounds, L"RIVAN // SHADE MODE", b[1].Get(), b[2].Get(), b[3].Get(),
                                 b[4].Get(), b[13].Get(), b[7].Get());
        const auto title = Rect(bounds.left + 4, bounds.top + 4, bounds.right - 4, bounds.top + 22);
        Win32Ui::Impl::DrawWindowButton(Rect(title.right - 19, title.top + 2, title.right - 3, title.bottom - 2), L"X", 3,
                         b[2].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawWindowButton(Rect(title.right - 38, title.top + 2, title.right - 22, title.bottom - 2), L"^", 2,
                         b[2].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        const auto lcd = Rect(content.left + 3, content.top + 3, content.right - 3, content.top + 49);
        // SCREEN: Mini-player LCD.
        Win32Ui::Impl::DrawBevel(lcd, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
        Win32Ui::Impl::DrawText(FormatTime(model.positionSeconds), Rect(lcd.left + 5, lcd.top, lcd.left + 105, lcd.bottom),
                 b[6].Get(), digitalFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        Win32Ui::Impl::DrawText(model.nowTitle, Rect(lcd.left + 111, lcd.top + 2, lcd.right - 5, lcd.top + 25),
                 b[6].Get(), regularFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        Win32Ui::Impl::DrawText(model.nowArtist, Rect(lcd.left + 111, lcd.top + 24, lcd.right - 5, lcd.bottom - 2),
                 b[8].Get(), smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        const float seekTop = lcd.bottom + 5;
        const float progress = model.durationSeconds > 0.0
            ? static_cast<float>(model.positionSeconds / model.durationSeconds) : 0.0F;
        Win32Ui::Impl::DrawSlider(Rect(content.left + 4, seekTop, content.right - 4, seekTop + 15), progress,
                   HitKind::Seek, b[5].Get(), b[13].Get(), b[2].Get(), b[3].Get(), b[4].Get());
        const float buttonTop = content.bottom - 29;
        Win32Ui::Impl::DrawButton(Rect(content.left + 4, buttonTop, content.left + 48, content.bottom - 3), L"|<<",
                   Command::Previous, b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawButton(Rect(content.left + 52, buttonTop, content.left + 105, content.bottom - 3),
                   model.playback == PlaybackState::Playing ? L"||" : L">", Command::PlayPause,
                   b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get(),
                   model.playback == PlaybackState::Playing);
        Win32Ui::Impl::DrawButton(Rect(content.left + 109, buttonTop, content.left + 153, content.bottom - 3), L">>|",
                   Command::Next, b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[13].Get());
        Win32Ui::Impl::DrawText(L"VOL", Rect(content.right - 181, buttonTop, content.right - 151, content.bottom - 3),
                 b[13].Get(), tinyFormat.Get());
        Win32Ui::Impl::DrawSlider(Rect(content.right - 148, buttonTop + 5, content.right - 4, content.bottom - 8), model.volume,
                   HitKind::Volume, b[5].Get(), b[13].Get(), b[2].Get(), b[3].Get(), b[4].Get());
        Win32Ui::Impl::DrawSkinDecor(size, 1);
        Win32Ui::Impl::DrawSkinDecor(size, 2);
        Win32Ui::Impl::FlushDeferredTexts();
        Win32Ui::Impl::DrawImageSelection(size);
    }

// Decodes a skin image file into a device bitmap, caching by absolute path.
[[nodiscard]] ID2D1Bitmap* Win32Ui::Impl::LoadSkinBitmap(const std::filesystem::path& relative) {
        if (!wicFactory || !target || relative.empty() || model.activeSkin.directory.empty()) {
            return nullptr;
        }
        const std::wstring key = (model.activeSkin.directory / relative).wstring();
        if (const auto found = imageCache.find(key); found != imageCache.end()) {
            return found->second.Get();
        }
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wicFactory->CreateDecoderFromFilename(key.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, decoder.ReleaseAndGetAddressOf()))) {
            return nullptr;
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(decoder->GetFrame(0, frame.ReleaseAndGetAddressOf())) ||
            FAILED(wicFactory->CreateFormatConverter(converter.ReleaseAndGetAddressOf())) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut)) ||
            FAILED(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr,
                 bitmap.ReleaseAndGetAddressOf()))) {
            return nullptr;
        }
        auto* raw = bitmap.Get();
        imageCache[key] = std::move(bitmap);
        return raw;
    }

[[nodiscard]] std::vector<Win32Ui::Impl::DecorRef> Win32Ui::Impl::DecorOrder(const skin::Skin& value) {
        std::vector<DecorRef> result;
        result.reserve(value.images.size() + value.shapes.size());
        for (std::size_t index = 0; index < value.images.size(); ++index) {
            result.push_back({true, index, value.images[index].priority});
        }
        for (std::size_t index = 0; index < value.shapes.size(); ++index) {
            result.push_back({false, index, value.shapes[index].priority});
        }
        // Priority 1 is drawn last, making it visually topmost.
        std::stable_sort(result.begin(), result.end(), [](const DecorRef& left, const DecorRef& right) {
            return left.priority > right.priority;
        });
        return result;
    }

[[nodiscard]] const std::vector<Win32Ui::Impl::DecorRef>& Win32Ui::Impl::CachedDecorOrder() {
        if (decorOrderRevision != model.revision) {
            decorOrder = DecorOrder(model.activeSkin);
            decorOrderRevision = model.revision;
        }
        return decorOrder;
    }

// Layer 0 draws on window background. Layers 1 and 2 replay enabled decor over
    // panels and screens while control holes keep sliders usable and visible.
void Win32Ui::Impl::DrawSkinDecor(const D2D1_SIZE_F size, int layer) {
        ComPtr<ID2D1PathGeometry> layerMask;
        const auto& includedBounds = layer == 1 ? panelBounds : screenBounds;
        // Panel bevel stroke is 2px and centered on the edge, so half sits outside
        // panelBounds. Expand the over-panels mask so decor covers that border too.
        const float panelMaskPad =
            layer == 1 && model.activeSkin.appearance.showPanelBorders ? 2.0F : 0.0F;
        if (layer > 0 && !includedBounds.empty() &&
            SUCCEEDED(d2dFactory->CreatePathGeometry(layerMask.ReleaseAndGetAddressOf()))) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(layerMask->Open(sink.ReleaseAndGetAddressOf()))) {
                const auto addRect = [&sink](const D2D1_RECT_F& rect, float pad = 0.0F) {
                    sink->BeginFigure({rect.left - pad, rect.top - pad}, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine({rect.right + pad, rect.top - pad});
                    sink->AddLine({rect.right + pad, rect.bottom + pad});
                    sink->AddLine({rect.left - pad, rect.bottom + pad});
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                };
                sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
                for (const auto& bounds : includedBounds) addRect(bounds, panelMaskPad);
                if (layer == 1) {
                    for (const auto& screen : screenBounds) addRect(screen);
                }
                if (layer == 1) {
                    for (const auto& control : decorControlBounds) addRect(control);
                }
                if (SUCCEEDED(sink->Close())) {
                    target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), layerMask.Get()), nullptr);
                } else {
                    layerMask.Reset();
                }
            } else {
                layerMask.Reset();
            }
        }
        if (layer > 0 && !layerMask) return;
        const auto denorm = [size](float nx, float ny) {
            return D2D1::Point2F(nx * size.width, ny * size.height);
        };
        for (const auto ref : CachedDecorOrder()) {
            if (ref.image) {
                const auto& image = model.activeSkin.images[ref.index];
                const bool overlaysLayer = layer == 0 || (layer == 1 ? image.overPanels
                                                                     : image.overScreens);
                if (!overlaysLayer) continue;
                ID2D1Bitmap* bitmap = LoadSkinBitmap(image.file);
                if (!bitmap) continue;
                const auto topLeft = denorm(image.x, image.y);
                const auto destination = Rect(topLeft.x, topLeft.y,
                                              topLeft.x + image.width * size.width,
                                              topLeft.y + image.height * size.height);
                D2D1_MATRIX_3X2_F previousTransform{};
                target->GetTransform(&previousTransform);
                const auto center = D2D1::Point2F((destination.left + destination.right) * 0.5F,
                                                 (destination.top + destination.bottom) * 0.5F);
                target->SetTransform(D2D1::Matrix3x2F::Scale(
                                         image.flipHorizontal ? -1.0F : 1.0F,
                                         image.flipVertical ? -1.0F : 1.0F, center) *
                                     D2D1::Matrix3x2F::Rotation(image.rotation, center) *
                                     previousTransform);
                const float imageOpacity = std::clamp(
                    image.opacity * model.activeSkin.appearance.backgroundImageOpacity, 0.0F, 1.0F);
                target->DrawBitmap(bitmap, destination, imageOpacity,
                                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                // Soft accent wash masked by the bitmap alpha so transparent cutouts stay clear.
                if (image.tint.alpha > 0) {
                    auto tintColor = ToD2D(image.tint);
                    // Cap wash strength so the source image stays visible (accent, not full recolor).
                    tintColor.a = std::clamp(tintColor.a * imageOpacity * 0.55F, 0.0F, 1.0F);
                    if (!decorBrush) {
                        (void)target->CreateSolidColorBrush(tintColor,
                                                            decorBrush.ReleaseAndGetAddressOf());
                    }
                    if (decorBrush) {
                        decorBrush->SetColor(tintColor);
                        // FillOpacityMask requires aliased AA; restore afterward.
                        const D2D1_ANTIALIAS_MODE previousAa = target->GetAntialiasMode();
                        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
                        target->FillOpacityMask(bitmap, decorBrush.Get(),
                                               D2D1_OPACITY_MASK_CONTENT_GRAPHICS,
                                               &destination, nullptr);
                        target->SetAntialiasMode(previousAa);
                    }
                }
                target->SetTransform(previousTransform);
                continue;
            }
            const auto& shape = model.activeSkin.shapes[ref.index];
            const bool overlaysLayer = layer == 0 || (layer == 1 ? shape.overPanels
                                                                 : shape.overScreens);
            if (!overlaysLayer) continue;
            // Use RGB from shape.color; transparency comes only from shape.opacity so
            // studio 100% OPACITY is fully solid even when legacy manifests store AA < FF.
            auto color = ToD2D(shape.color);
            color.a = std::clamp(shape.opacity, 0.0F, 1.0F);
            if (!decorBrush && FAILED(target->CreateSolidColorBrush(
                    color, decorBrush.ReleaseAndGetAddressOf()))) {
                continue;
            }
            if (!decorBrush) continue;
            decorBrush->SetColor(color);
            const auto topLeft = denorm(shape.x, shape.y);
            const auto rect = Rect(topLeft.x, topLeft.y,
                                   topLeft.x + shape.width * size.width,
                                   topLeft.y + shape.height * size.height);
            const float stroke = std::max(0.5F, shape.strokeWidth);
            D2D1_MATRIX_3X2_F previousTransform{};
            target->GetTransform(&previousTransform);
            const auto center = D2D1::Point2F((rect.left + rect.right) * 0.5F,
                                               (rect.top + rect.bottom) * 0.5F);
            target->SetTransform(D2D1::Matrix3x2F::Scale(shape.flipHorizontal ? -1.0F : 1.0F,
                                                           shape.flipVertical ? -1.0F : 1.0F, center) *
                                 D2D1::Matrix3x2F::Rotation(shape.rotation, center) *
                                 previousTransform);
            switch (shape.kind) {
            case skin::ShapeKind::Rectangle:
                if (shape.filled) target->FillRectangle(rect, decorBrush.Get());
                else target->DrawRectangle(rect, decorBrush.Get(), stroke);
                break;
            case skin::ShapeKind::Ellipse: {
                const auto ellipse = D2D1::Ellipse(
                    D2D1::Point2F((rect.left + rect.right) * 0.5F, (rect.top + rect.bottom) * 0.5F),
                    Width(rect) * 0.5F, Height(rect) * 0.5F);
                if (shape.filled) target->FillEllipse(ellipse, decorBrush.Get());
                else target->DrawEllipse(ellipse, decorBrush.Get(), stroke);
                break;
            }
            case skin::ShapeKind::Line:
                target->DrawLine({rect.left, rect.top}, {rect.right, rect.bottom}, decorBrush.Get(), stroke);
                break;
            }
            target->SetTransform(previousTransform);
        }
        if (layerMask) target->PopLayer();
    }

void Win32Ui::Impl::DrawImageSelection(const D2D1_SIZE_F size) {
        if (!model.skinStudioVisible) return;
        D2D1_RECT_F bounds{};
        if (studioShapeFocused && !model.activeSkin.shapes.empty()) {
            const auto& shape = model.activeSkin.shapes[
                std::min(studioShapeIndex, model.activeSkin.shapes.size() - 1)];
            bounds = Rect(shape.x * size.width, shape.y * size.height,
                          (shape.x + shape.width) * size.width,
                          (shape.y + shape.height) * size.height);
        } else if (studioImageFocused && !model.activeSkin.images.empty()) {
            const auto& image = model.activeSkin.images[
                std::min(studioImageIndex, model.activeSkin.images.size() - 1)];
            bounds = Rect(image.x * size.width, image.y * size.height,
                          (image.x + image.width) * size.width,
                          (image.y + image.height) * size.height);
        } else {
            return;
        }
            ComPtr<ID2D1SolidColorBrush> selectionBrush;
            if (SUCCEEDED(target->CreateSolidColorBrush(ToD2D(model.activeSkin.colors.accent),
                    selectionBrush.ReleaseAndGetAddressOf()))) {
                target->DrawRectangle(bounds, selectionBrush.Get(), 1.5F);
                const auto handle = [&](float x, float y) {
                    target->FillRectangle(Rect(x - 5.0F, y - 5.0F, x + 5.0F, y + 5.0F),
                                          selectionBrush.Get());
                };
                handle(bounds.right, bounds.bottom);
                const float centerX = (bounds.left + bounds.right) * 0.5F;
                target->DrawLine({centerX, bounds.top}, {centerX, bounds.top - 18.0F},
                                 selectionBrush.Get(), 1.5F);
                target->FillEllipse(D2D1::Ellipse({centerX, bounds.top - 22.0F}, 5.0F, 5.0F),
                                    selectionBrush.Get());
        }
    }

void Win32Ui::Impl::DrawFull(const D2D1_SIZE_F size,
                  std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        target->FillRectangle(Rect(0, 0, size.width, size.height), b[0].Get());
        Win32Ui::Impl::DrawSkinDecor(size);
        screenBounds.clear();
        panelBounds.clear();
        decorControlBounds.clear();
        constexpr float margin = 8.0F;
        constexpr float gap = 6.0F;
        const auto safe = Rect(margin, margin, size.width - margin, size.height - margin - 2.0F);
        const float leftWidth = std::clamp(size.width * 0.44F, 330.0F, 520.0F);
        const float leftRight = safe.left + leftWidth;
        const float playerHeight = std::clamp(Height(safe) * 0.30F, 195.0F, 238.0F);
        const float equalizerHeight = std::clamp(Height(safe) * 0.18F, 108.0F, 145.0F);
        const float playlistTop = safe.top + playerHeight + gap;
        const float equalizerTop = safe.bottom - equalizerHeight;

        deferTexts = true;
        Win32Ui::Impl::DrawPlayer(Rect(safe.left, safe.top, leftRight, safe.top + playerHeight), b);
        Win32Ui::Impl::DrawPlaylistEditor(Rect(safe.left, playlistTop, leftRight, equalizerTop - gap), b);
        Win32Ui::Impl::DrawEqualizer(Rect(safe.left, equalizerTop, leftRight, safe.bottom), b);
        Win32Ui::Impl::DrawLibrary(Rect(leftRight + gap, safe.top, safe.right, safe.bottom), b);
        Win32Ui::Impl::DrawSkinDecor(size, 1);
        Win32Ui::Impl::DrawSkinDecor(size, 2);
        Win32Ui::Impl::FlushDeferredTexts();
        Win32Ui::Impl::DrawImageSelection(size);
    }

void Win32Ui::Impl::DrawSettings(const D2D1_SIZE_F size,
                      std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        hits.clear();
        const auto panel = Rect(10.0F, 10.0F, size.width - 10.0F, size.height - 10.0F);
        target->FillRectangle(Rect(0, 0, size.width, size.height), b[0].Get());
        auto content = DrawPanel(panel, L"RIVAN PREFERENCES", b[1].Get(), b[2].Get(), b[3].Get(),
                                 b[4].Get(), b[13].Get(), b[7].Get());
        captionRect = Rect(panel.left + 4, panel.top + 4, content.right - 72, panel.top + 22);
        Win32Ui::Impl::DrawButton(Rect(content.right - 67, content.top + 3, content.right - 3, content.top + 25), L"CLOSE",
                   Command::ToggleSettings, b[2].Get(), b[1].Get(), b[3].Get(), b[4].Get(), b[9].Get());
        const float navigationWidth = std::clamp(Width(content) * 0.24F, 145.0F, 230.0F);
        const auto navigation = Rect(content.left + 3, content.top + 31,
                                     content.left + navigationWidth, content.bottom - 3);
        // SCREEN: Preferences navigation list.
        Win32Ui::Impl::DrawBevel(navigation, b[5].Get(), b[3].Get(), b[4].Get(), true);
        const std::array categories{SettingCategory::General, SettingCategory::Appearance,
                                    SettingCategory::Discord, SettingCategory::Downloading,
                                    SettingCategory::SkinManager};
        float top = navigation.top + 5;
        for (const auto category : categories) {
            const auto row = Rect(navigation.left + 4, top, navigation.right - 4, top + 25);
            const bool selected = category == model.settingsCategory;
            if (selected) target->FillRectangle(row, b[11].Get());
            Win32Ui::Impl::DrawText(CategoryName(category), Rect(row.left + 6, row.top, row.right - 4, row.bottom),
                     selected ? b[12].Get() : b[6].Get(), regularFormat.Get());
            Win32Ui::Impl::AddSettingHit(row, category);
            top += 27;
        }
        const auto details = Rect(navigation.right + 7, navigation.top, content.right - 3, content.bottom - 3);
        settingsDetailsBounds = details;
        // SCREEN: Preferences detail pane.
        Win32Ui::Impl::DrawBevel(details, b[5].Get(), b[3].Get(), b[4].Get(), true);
        const bool integrationCategory = model.settingsCategory == SettingCategory::General ||
                                          model.settingsCategory == SettingCategory::Appearance ||
                                          model.settingsCategory == SettingCategory::Discord ||
                                         model.settingsCategory == SettingCategory::Downloading;
        if (!integrationCategory) {
            Win32Ui::Impl::DrawText(CategoryName(model.settingsCategory), Rect(details.left + 15, details.top + 13,
                     details.right - 15, details.top + 42), b[6].Get(), headingFormat.Get());
        }
        if (model.settingsCategory == SettingCategory::SkinManager) {
            Win32Ui::Impl::DrawSkinManagerPane(details, b);
            return;
        }
        settingsSkinListBounds = {};
        settingsSkinRows = 0;
        if (model.settingsCategory == SettingCategory::General ||
            model.settingsCategory == SettingCategory::Appearance ||
            model.settingsCategory == SettingCategory::Discord ||
            model.settingsCategory == SettingCategory::Downloading) {
            Win32Ui::Impl::DrawGeneralPane(details, b);
            return;
        }
        settingsScrollY = 0.0F;
        settingsContentHeight = 0.0F;
        Win32Ui::Impl::DrawText(L"Settings values and persistence are owned by the application core.",
                 Rect(details.left + 15, details.top + 53, details.right - 15, details.top + 78),
                 b[9].Get(), regularFormat.Get());
        Win32Ui::Impl::DrawText(L"This classic control surface keeps the existing validated callback seam.",
                 Rect(details.left + 15, details.top + 79, details.right - 15, details.top + 105),
                 b[10].Get(), smallFormat.Get());
        Win32Ui::Impl::DrawBevel(Rect(details.left + 15, details.bottom - 48, details.right - 15, details.bottom - 16),
                  b[1].Get(), b[3].Get(), b[4].Get());
        Win32Ui::Impl::DrawText(L"CALLBACK INTERFACE: ONLINE", Rect(details.left + 20, details.bottom - 48,
                 details.right - 20, details.bottom - 16), b[8].Get(), regularFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }

// General pane: music folder list. Show every configured root, then one empty
    // slot so the next folder can be chosen. After each choice another empty slot
    // appears (no limit). Subfolders of all roots become playlists.
    void Win32Ui::Impl::DrawGeneralPane(const D2D1_RECT_F& details,
                         std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        const float left = details.left + 15;
        const float right = details.right - 15;
        const float contentTop = details.top + 15;
        const float viewportBottom = details.bottom - 4;
        const float viewportHeight = std::max(0.0F, viewportBottom - contentTop);
        {
            const float maxScroll = std::max(0.0F, settingsContentHeight - viewportHeight);
            settingsScrollY = std::clamp(settingsScrollY, 0.0F, maxScroll);
        }

        // Clip scrolled content below the category heading.
        const auto clip = Rect(details.left + 2, contentTop, details.right - 2, viewportBottom);
        target->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);

        float y = contentTop - settingsScrollY;

        if (model.settingsCategory == SettingCategory::General) {
        Win32Ui::Impl::DrawText(L"WINDOWS", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        const float optionWidth = (right - left - 8) * 0.5F;
        SettingsButton(Rect(left, y, left + optionWidth, y + 24),
                       model.startAtStartup ? L"START AT STARTUP: ON" : L"START AT STARTUP: OFF",
                       15, b);
        SettingsButton(Rect(left + optionWidth + 8, y, right, y + 24),
                       model.exitToTray ? L"EXIT TO TRAY: ON" : L"EXIT TO TRAY: OFF",
                       16, b);
        y += 34;

        // Action encoding: browse = 100 + index, clear = 200 + index (index 0 = primary).
        const auto field = [&](const wchar_t* caption, const std::wstring& value,
                               std::size_t index, bool allowClear) {
            if (caption != nullptr && caption[0] != L'\0') {
                Win32Ui::Impl::DrawText(caption, Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                         DWRITE_TEXT_ALIGNMENT_CENTER);
                y += 29;
            } else {
                y += 4;
            }
            const float browseW = 78.0F;
            const float clearW = allowClear ? 60.0F : 0.0F;
            const auto box = Rect(left, y, right - browseW - clearW - 8, y + 24);
            Win32Ui::Impl::DrawBevel(box, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
            Win32Ui::Impl::DrawText(value.empty() ? L"(not set)" : value,
                     Rect(box.left + 5, box.top, box.right - 4, box.bottom),
                     value.empty() ? b[10].Get() : b[6].Get(), regularFormat.Get());
            SettingsButton(Rect(right - browseW - clearW - 4, y, right - clearW - 4, y + 24),
                           L"BROWSE...", 100 + static_cast<std::uint64_t>(index), b);
            if (allowClear) {
                SettingsButton(Rect(right - clearW, y, right, y + 24), L"CLEAR",
                               200 + static_cast<std::uint64_t>(index), b);
            }
            y += 28;
        };

        const std::wstring primary =
            model.musicFolders.empty() ? std::wstring{} : model.musicFolders[0];
        field(L"MUSIC FOLDER", primary, 0, false);
        if (!primary.empty()) {
            for (std::size_t i = 1; i < model.musicFolders.size(); ++i) {
                field(L"", model.musicFolders[i], i, true);
            }
            // Empty trailing slot only after last chosen folder (no limit).
            field(L"", L"", model.musicFolders.size(), false);
        }
        y += 10;

        Win32Ui::Impl::DrawText(L"FILE PREVIEW", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.filePreviewEnabled ? L"FILE PREVIEW: ON" : L"FILE PREVIEW: OFF",
                       14, b);
        y += 34;

        Win32Ui::Impl::DrawText(L"PLAYLISTS", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.duplicateAsFile ? L"DUPLICATE: COPY FILE ON DISK"
                                             : L"DUPLICATE: ADD SECOND ENTRY",
                       17, b);
        y += 26;
        Win32Ui::Impl::DrawText(model.duplicateAsFile
                     ? L"Right-click > Duplicate copies the audio file and adds the copy."
                     : L"Right-click > Duplicate adds another reference to the same track.",
                  Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
        y += 22;
        }

        if (model.settingsCategory == SettingCategory::Appearance) {
        Win32Ui::Impl::DrawText(L"TRACK COVERS", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.trackCoverArtEnabled ? L"SONG COVERS: ON" : L"SONG COVERS: OFF",
                       22, b);
        y += 26;
        Win32Ui::Impl::DrawText(L"Small cached covers appear after titles when embedded artwork is available.",
                 Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
        y += 22;
        }

        if (model.settingsCategory == SettingCategory::Discord) {
        Win32Ui::Impl::DrawText(L"DISCORD", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.discordEnabled ? L"RICH PRESENCE: ON" : L"RICH PRESENCE: OFF",
                       18, b);
        y += 26;
        Win32Ui::Impl::DrawText(L"Shows the playing track in Discord. Needs Discord desktop running.",
                 Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
        y += 20;
        const float discordOptionWidth = (right - left - 8) * 0.5F;
        SettingsButton(Rect(left, y, left + discordOptionWidth, y + 24),
                       model.discordShowArtist ? L"SHOW ARTIST: ON" : L"SHOW ARTIST: OFF",
                       20, b);
        SettingsButton(Rect(left + discordOptionWidth + 8, y, right, y + 24),
                        model.discordShowImageText ? L"IMAGE TEXT: RIVAN"
                                                   : L"IMAGE TEXT: OFF",
                        21, b);
        y += 34;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.discordShowGithubButton ? L"GITHUB BUTTON: ON"
                                                     : L"GITHUB BUTTON: OFF",
                       23, b);
        y += 26;
        Win32Ui::Impl::DrawText(
            L"Visible to other users only; links to https://github.com/gyatstian/Rivan.",
            Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
        y += 22;

        Win32Ui::Impl::DrawText(L"RICH PRESENCE IMAGE URL", Rect(left, y, right, y + 25),
                 b[8].Get(), headingFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        const float clearW = 60.0F;
        const auto imageBox = Rect(left, y, right - clearW - 4, y + 24);
        const bool imageActive = discordImageEditing;
        Win32Ui::Impl::DrawBevel(imageBox, imageActive ? b[7].Get() : b[5].Get(), b[3].Get(),
                  b[4].Get(), true, 2.0F);
        const std::wstring& shownUrl = imageActive ? discordImageBuffer
                                                   : model.discordImageUrl;
        Win32Ui::Impl::DrawText(shownUrl.empty() ? L"https://... (default Rivan image)" : shownUrl,
                 Rect(imageBox.left + 5, imageBox.top, imageBox.right - 4, imageBox.bottom),
                 shownUrl.empty() ? b[10].Get() : b[6].Get(), regularFormat.Get());
        const float clipTop = settingsDetailsBounds.top + 15.0F;
        const float clipBottom = settingsDetailsBounds.bottom - 4.0F;
        if (imageBox.bottom > clipTop && imageBox.top < clipBottom) {
            HitRegion imageHit;
            imageHit.bounds = Rect(imageBox.left, std::max(imageBox.top, clipTop),
                                   imageBox.right, std::min(imageBox.bottom, clipBottom));
            imageHit.kind = HitKind::DiscordImageField;
            hits.push_back(imageHit);
        }
        SettingsButton(Rect(right - clearW, y, right, y + 24), L"CLEAR", 19, b);
        y += 26;
        Win32Ui::Impl::DrawText(L"Direct public image URL only; temporary attachment URLs may fail.",
                  Rect(left, y, right, y + 14), b[6].Get(), tinyFormat.Get());
        y += 22;
        }

        if (model.settingsCategory == SettingCategory::Downloading) {
        Win32Ui::Impl::DrawText(L"YOUTUBE", Rect(left, y, right, y + 25), b[8].Get(), headingFormat.Get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 29;
        SettingsButton(Rect(left, y, right, y + 24),
                       model.youtubeEnabled ? L"YOUTUBE DOWNLOADER: ON"
                                            : L"YOUTUBE DOWNLOADER: OFF",
                       4, b);
        y += 30;
        if (model.youtubeEnabled) {
            const int mode = std::clamp(model.youtubeDownloadMode, 0, 2);
            // Full-width format selector: cycles MP3 -> Original -> Video (action 9).
            static constexpr const wchar_t* kFormatLabels[] = {
                L"FORMAT: MP3 (FFMPEG)",
                L"FORMAT: ORIGINAL (M4A)",
                L"FORMAT: VIDEO (MP4)",
            };
            SettingsButton(Rect(left, y, right, y + 24), kFormatLabels[mode], 9, b);
            y += 26;
            // Inline hint clarifying the tradeoff / requirement per format.
            const wchar_t* formatHint = L"";
            if (mode == 0) {
                formatHint = model.youtubeFfmpegInstalled
                                 ? L"Transcodes to .mp3 via ffmpeg (re-encode, universal)."
                                 : L"Needs ffmpeg — install below.";
            } else if (mode == 1) {
                formatHint = L"Instant, no ffmpeg, highest fidelity. Saves .m4a/.opus.";
            } else {
                formatHint = model.youtubeFfmpegInstalled
                                 ? L"Video + audio merged to .mp4."
                                 : L"No ffmpeg: falls back to progressive .mp4.";
            }
            Win32Ui::Impl::DrawText(formatHint, Rect(left, y, right, y + 14), b[6].Get(),
                     tinyFormat.Get());
            y += 18;

            const float stepW = 28.0F;
            const auto drawQualityRow = [&](const wchar_t* caption, const wchar_t* label,
                                            std::uint64_t minusId, std::uint64_t plusId) {
                Win32Ui::Impl::DrawText(caption, Rect(left, y, right, y + 16), b[8].Get(), tinyFormat.Get());
                y += 18;
                const auto qualityBox =
                    Rect(left + stepW + 4, y, right - stepW - 4, y + 24);
                SettingsButton(Rect(left, y, left + stepW, y + 24), L"-", minusId, b);
                SettingsButton(Rect(right - stepW, y, right, y + 24), L"+", plusId, b);
                Win32Ui::Impl::DrawBevel(qualityBox, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
                Win32Ui::Impl::DrawText(label,
                         Rect(qualityBox.left + 6, qualityBox.top, qualityBox.right - 6,
                              qualityBox.bottom),
                         b[6].Get(), tinyFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
                y += 30;
            };

            // Audio quality: one shared 0-9 knob for every format. In MP3 it drives the
            // libmp3lame VBR encode; in Original/Video it selects best/mid/worst stream.
            const int q = std::clamp(model.youtubeAudioQuality, 0, 9);
            static constexpr const wchar_t* kQualityLabels[] = {
                L"0 — best audio (highest fidelity, largest)",
                L"1 — very high audio",
                L"2 — high audio",
                L"3 — good audio",
                L"4 — good audio, smaller",
                L"5 — mid audio (~≤160 kb/s)",
                L"6 — mid audio, smaller",
                L"7 — low audio",
                L"8 — low audio, smaller",
                L"9 — worst audio (smallest file)",
            };
            drawQualityRow(L"AUDIO QUALITY (lower number = better · shared by all formats)",
                           kQualityLabels[q], 7, 8);

            // Video quality only applies to the Video (mp4) format.
            if (mode == 2) {
                const int vq = std::clamp(model.youtubeMp4VideoQuality, 0, 5);
                static constexpr const wchar_t* kVideoLabels[] = {
                    L"0 — max 144p, smallest file",
                    L"1 — max 240p",
                    L"2 — max 360p",
                    L"3 — max 480p",
                    L"4 — max 720p",
                    L"5 — max 1080p, largest file",
                };
                drawQualityRow(L"VIDEO QUALITY (height cap · lower = smaller)",
                               kVideoLabels[vq], 10, 11);
            }
        }
        // Hide install buttons once the tool is present; keep while installing.
        const bool showYtInstall =
            !model.youtubeYtDlpInstalled || model.youtubeInstallingYtDlp;
        const bool showFfInstall =
            !model.youtubeFfmpegInstalled || model.youtubeInstallingFfmpeg;
        if (showYtInstall || showFfInstall) {
            const wchar_t* ytLabel = model.youtubeInstallingYtDlp ? L"INSTALLING YT-DLP..."
                                                                   : L"INSTALL YT-DLP";
            const wchar_t* ffLabel = model.youtubeInstallingFfmpeg ? L"INSTALLING FFMPEG..."
                                                                    : L"INSTALL FFMPEG";
            if (showYtInstall && showFfInstall) {
                const float toolW = (right - left - 8) * 0.5F;
                SettingsButton(Rect(left, y, left + toolW, y + 24), ytLabel, 5, b);
                SettingsButton(Rect(left + toolW + 8, y, right, y + 24), ffLabel, 6, b);
            } else if (showYtInstall) {
                SettingsButton(Rect(left, y, right, y + 24), ytLabel, 5, b);
            } else {
                SettingsButton(Rect(left, y, right, y + 24), ffLabel, 6, b);
            }
            y += 28;
        }
        }

        target->PopAxisAlignedClip();

        // y is contentTop - scroll + content height; recover full content height.
        settingsContentHeight = (y + settingsScrollY) - contentTop + 8.0F;
        const float maxScroll = std::max(0.0F, settingsContentHeight - viewportHeight);
        settingsScrollY = std::clamp(settingsScrollY, 0.0F, maxScroll);

        // Thin scrollbar track when content overflows.
        if (maxScroll > 0.5F) {
            const float trackLeft = details.right - 8.0F;
            const float trackTop = contentTop;
            const float trackBottom = viewportBottom;
            const float trackH = trackBottom - trackTop;
            Win32Ui::Impl::DrawBevel(Rect(trackLeft, trackTop, details.right - 3.0F, trackBottom),
                      b[5].Get(), b[3].Get(), b[4].Get(), true, 1.0F);
            const float thumbH = std::max(24.0F, trackH * (viewportHeight / settingsContentHeight));
            const float thumbTravel = trackH - thumbH;
            const float thumbY = trackTop + (maxScroll > 0.0F
                                                 ? (settingsScrollY / maxScroll) * thumbTravel
                                                 : 0.0F);
            target->FillRectangle(Rect(trackLeft + 1.0F, thumbY, details.right - 4.0F, thumbY + thumbH),
                                  b[8].Get());
        }
    }

// Skin Manager pane: two side-by-side actions on top (open the skin studio / open the
    // skins folder), and a scrollable-ish list of saved and built-in skins below. Clicking
    // a skin applies it immediately.
    void Win32Ui::Impl::DrawSkinManagerPane(const D2D1_RECT_F& details,
                             std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        const float left = details.left + 15;
        const float right = details.right - 15;
        const auto buttons = Rect(left, details.top + 46, right, details.top + 72);
        const float half = Width(buttons) * 0.5F;
        StudioButton(Rect(buttons.left, buttons.top, buttons.left + half - 4, buttons.bottom),
                     L"SKIN STUDIO", 100, b);
        StudioButton(Rect(buttons.left + half + 4, buttons.top, buttons.right, buttons.bottom),
                     L"SKIN FOLDER", 101, b);

        Win32Ui::Impl::DrawText(L"SAVED SKINS", Rect(left, buttons.bottom + 6, right, buttons.bottom + 22),
                  b[8].Get(), tinyFormat.Get());
        const auto list = Rect(left, buttons.bottom + 24, right, details.bottom - 8);
        settingsSkinListBounds = list;
        settingsSkinRows = static_cast<std::size_t>(
            std::max(1.0F, std::floor((Height(list) - 4.0F) / 29.0F)));
        const std::size_t skinMax =
            model.skins.size() > settingsSkinRows ? model.skins.size() - settingsSkinRows : 0;
        settingsSkinScroll = std::min(settingsSkinScroll, skinMax);
        // SCREEN: Saved-skins list.
        Win32Ui::Impl::DrawBevel(list, b[5].Get(), b[3].Get(), b[4].Get(), true);
        target->PushAxisAlignedClip(list, D2D1_ANTIALIAS_MODE_ALIASED);
        float rowTop = list.top + 2;
        const std::size_t last =
            std::min(model.skins.size(), settingsSkinScroll + settingsSkinRows);
        for (std::size_t index = settingsSkinScroll; index < last; ++index) {
            const auto& s = model.skins[index];
            const auto row = Rect(list.left + 2, rowTop, list.right - 2, rowTop + 27);
            const bool hot = Contains(row, static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            if (s.active) target->FillRectangle(row, b[11].Get());
            else if (hot) target->FillRectangle(row, b[7].Get());
            Win32Ui::Impl::DrawText(s.active ? L"\u25B6" : (s.builtIn ? L"\u2302" : L"\u2022"),
                     Rect(row.left + 4, row.top, row.left + 20, row.bottom),
                     s.active ? b[12].Get() : b[6].Get(), regularFormat.Get());
            const float actionsLeft = s.builtIn ? row.right : row.right - 150.0F;
            Win32Ui::Impl::DrawText(s.name, Rect(row.left + 22, row.top, actionsLeft - 4, row.bottom),
                      s.active ? b[12].Get() : b[9].Get(), regularFormat.Get());
            if (!s.builtIn) {
                const float buttonWidth = 46.0F;
                StudioButton(Rect(actionsLeft, row.top + 3, actionsLeft + buttonWidth, row.bottom - 3),
                             L"RENAME", 600 + index, b);
                StudioButton(Rect(actionsLeft + 50, row.top + 3, actionsLeft + 96, row.bottom - 3),
                             L"EDIT", 700 + index, b);
                StudioButton(Rect(actionsLeft + 100, row.top + 3, row.right - 2, row.bottom - 3),
                             L"DELETE", 800 + index, b);
            }
            HitRegion hit;
            hit.bounds = Rect(row.left, row.top, actionsLeft, row.bottom);
            hit.kind = HitKind::Studio;
            hit.id = 200 + index;  // Apply skin at this index.
            hits.push_back(hit);
            rowTop += 29;
        }
        target->PopAxisAlignedClip();
        if (managerNameEditing && managerSkinIndex < model.skins.size()) {
            const auto prompt = Rect(list.left + 8, list.bottom - 34, list.right - 8, list.bottom - 7);
            target->FillRectangle(prompt, b[1].Get());
            // SCREEN: Skin rename text field.
            Win32Ui::Impl::DrawBevel(Rect(prompt.left, prompt.top, prompt.right - 58, prompt.bottom), b[5].Get(),
                      b[3].Get(), b[4].Get(), true);
            Win32Ui::Impl::DrawText(managerSkinName + (((GetTickCount64() / 500ULL) % 2ULL == 0ULL) ? L"_" : L""),
                     Rect(prompt.left + 5, prompt.top, prompt.right - 63, prompt.bottom), b[9].Get(),
                     regularFormat.Get());
            StudioButton(Rect(prompt.right - 54, prompt.top, prompt.right, prompt.bottom), L"APPLY",
                         900 + managerSkinIndex, b);
        }
        if (model.skins.empty()) {
            Win32Ui::Impl::DrawText(L"< NO SKINS INSTALLED >", list, b[10].Get(), regularFormat.Get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

// Brushes are derived from the active skin palette so appearance is never hard-coded.
    // Index legend (kept for the existing draw call sites):
    //  0 windowBg 1 panelBg 2 controlBg 3 bevelLight 4 bevelDark 5 screen 6 accent
    //  7 hoverBg 8 accent 9 textPrimary 10 textSecondary 11 selection 12 textPrimary
    //  13 controls (seek/vol fill, titlebars, window chrome text, transport labels, EQ ON/AUTO)
    // Reuses solidBrushes; returns references for Draw* call sites that expect ComPtr array.
[[nodiscard]] std::array<ComPtr<ID2D1SolidColorBrush>, 14>& Win32Ui::Impl::UpdateBrushes() {
        const auto& c = model.activeSkin.colors;
        const std::array<D2D1_COLOR_F, 14> colors{
            ToD2D(c.windowBackground),
            ToD2D(c.panelBackground),
            ToD2D(c.raisedBackground),
            ToD2D(c.border),
            ToD2D(Darken(c.border, 0.45F)),
            ToD2D(c.screenBackground),
            ToD2D(c.accent),
            ToD2D(c.hoverBackground),
            ToD2D(c.accent),
            ToD2D(c.textPrimary),
            ToD2D(c.textSecondary),
            ToD2D(c.selection),
            ToD2D(c.textPrimary),
            ToD2D(c.playbackProgress),
        };
        for (std::size_t index = 0; index < solidBrushes.size(); ++index) {
            if (!solidBrushes[index]) {
                target->CreateSolidColorBrush(colors[index], solidBrushes[index].ReleaseAndGetAddressOf());
            } else {
                solidBrushes[index]->SetColor(colors[index]);
            }
        }
        // Preserve true alpha so screen opacity reveals decor and panel content underneath.
        const float screenOpacity = std::clamp(model.activeSkin.appearance.screenOpacity, 0.0F, 1.0F);
        const auto screen = ToD2D(c.screenBackground);
        const D2D1_COLOR_F screenColor{screen.r, screen.g, screen.b, screenOpacity};
        if (solidBrushes[5]) solidBrushes[5]->SetColor(screenColor);
        for (std::size_t index = 0; index < solidBrushes.size(); ++index) {
            currentBrushes[index] = solidBrushes[index].Get();
        }
        return solidBrushes;
    }

// Keeps a click-to-play mono-selection in sync with the transport. A plain track click
    // both selects and plays the row, so the played track lands in trackSelection. When the
    // transport auto-advances, the new track's `playing` flag already highlights it; without
    // this the previous row would keep its selection fill and look like it is still active.
    // Only the lone auto-selection of the previously playing row is moved; genuine multi- or
    // ctrl-selections are left untouched.
    void Win32Ui::Impl::SyncSelectionToPlayback() {
        std::size_t nowPlaying = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < model.tracks.size(); ++i) {
            if (model.tracks[i].playing) { nowPlaying = i; break; }
        }
        if (nowPlaying == lastPlayingModelIndex) return;
        // The playing row changed. If the selection is exactly the row that was playing,
        // it came from click-to-play, so hand it off to the new playing row (or drop it
        // when playback stopped) instead of stranding a stale highlight.
        if (lastPlayingModelIndex != static_cast<std::size_t>(-1) &&
            trackSelection.size() == 1 && trackSelection.contains(lastPlayingModelIndex)) {
            trackSelection.clear();
            if (nowPlaying != static_cast<std::size_t>(-1)) {
                trackSelection.insert(nowPlaying);
                trackAnchor = nowPlaying;
            } else {
                trackAnchor = static_cast<std::size_t>(-1);
            }
        }
        lastPlayingModelIndex = nowPlaying;
    }

void Win32Ui::Impl::Paint() {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        if (!CreateTarget()) {
            EndPaint(window, &paint);
            return;
        }
        const std::uint64_t previousRevision = model.revision;
        try { host.SnapshotUiModel(model); } catch (...) {}
        if (!model.trackCoverArtEnabled && !trackCoverCache.empty()) {
            trackCoverCache.clear();
            trackCoverUseCounter = 0;
            nextTrackCoverLookup = {};
        }
        // Paint only presents latest decoded preview frame. Decoder work stays off UI thread.
        if (windowKind == WindowKind::Main) SyncFilePreview();
        Win32Ui::Impl::SyncSelectionToPlayback();
        Win32Ui::Impl::ApplySkinFonts();
        hits.clear();
        colorFocusRegions.clear();
        auto& brushes = UpdateBrushes();
        target->BeginDraw();
        const auto size = target->GetSize();
        lastCanvas = size;
        if (windowKind == WindowKind::Settings) {
            Win32Ui::Impl::DrawSettings(size, brushes);
        } else if (windowKind == WindowKind::SkinStudio) {
            if (studioSection == StudioSection::Colors &&
                model.skinColorFocusRevision != seenColorFocusRevision) {
                studioColorIndex = std::min(model.focusedSkinColor, StudioColorFields().size() - 1);
                studioColorPickerVisible = true;
                studioHexEditing = false;
                studioHexSelectAll = false;
                seenColorFocusRevision = model.skinColorFocusRevision;
            }
            if (model.skinElementFocusRevision != seenElementFocusRevision) {
                const int focused = model.focusedSkinElement;
                if (focused != 0) studioSection = StudioSection::Elements;
                studioShapeFocused = focused > 0;
                studioImageFocused = focused < 0;
                if (focused > 0 && !model.activeSkin.shapes.empty()) {
                    studioShapeIndex = std::min(static_cast<std::size_t>(focused - 1),
                                                model.activeSkin.shapes.size() - 1);
                } else if (focused < 0 && !model.activeSkin.images.empty()) {
                    studioImageIndex = std::min(static_cast<std::size_t>(-focused - 1),
                                                model.activeSkin.images.size() - 1);
                }
                seenElementFocusRevision = model.skinElementFocusRevision;
            }
            if (studioOpen && !previewPending && model.revision != previousRevision) {
                studioDraft = model.activeSkin;
                if (!studioDraft.images.empty()) {
                    studioImageIndex = std::min(studioImageIndex, studioDraft.images.size() - 1);
                }
            }
            DrawSkinStudio(size, brushes);
        } else {
            if (model.skinElementFocusRevision != seenElementFocusRevision) {
                const int focused = model.focusedSkinElement;
                studioShapeFocused = focused > 0;
                studioImageFocused = focused < 0;
                if (focused > 0 && !model.activeSkin.shapes.empty()) {
                    studioShapeIndex = std::min(static_cast<std::size_t>(focused - 1),
                                                model.activeSkin.shapes.size() - 1);
                } else if (focused < 0 && !model.activeSkin.images.empty()) {
                    studioImageIndex = std::min(static_cast<std::size_t>(-focused - 1),
                                                model.activeSkin.images.size() - 1);
                }
                seenElementFocusRevision = model.skinElementFocusRevision;
            }
            if (previewFullscreen && previewIsVideo && filePreviewExpanded &&
                !model.miniPlayer) {
                Win32Ui::Impl::DrawPreviewFullscreenOverlay(size, brushes);
            } else {
                previewFullscreen = false;
                const bool compact = model.miniPlayer || size.width < 700.0F || size.height < 390.0F;
                if (compact) DrawMini(size, brushes);
                else Win32Ui::Impl::DrawFull(size, brushes);
            }
        }
        const HRESULT result = target->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) DiscardTarget();
        EndPaint(window, &paint);
        Win32Ui::Impl::SyncRefreshTimer();
    }

void Win32Ui::Impl::Resize(UINT width, UINT height) {
        if (target && width != 0 && height != 0 &&
            FAILED(target->Resize(D2D1::SizeU(width, height)))) DiscardTarget();
        InvalidateRect(window, nullptr, FALSE);
    }

void Win32Ui::Impl::InvokeSafely(Command command) {
        try { host.Invoke(command); } catch (...) {}
        InvalidateRect(window, nullptr, FALSE);
    }

// ---- Notification-area (system tray) support ----------------------------

[[nodiscard]] NOTIFYICONDATAW Win32Ui::Impl::TrayIconData() const noexcept {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = kTrayIconId;
        return data;
    }

void Win32Ui::Impl::AddTrayIcon() {
        if (trayIconAdded || !window) return;
        NOTIFYICONDATAW data = TrayIconData();
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayCallbackMessage;
        data.hIcon = LoadRivanIcon(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
        lstrcpynW(data.szTip, L"Rivan", static_cast<int>(std::size(data.szTip)));
        if (Shell_NotifyIconW(NIM_ADD, &data)) trayIconAdded = true;
    }

void Win32Ui::Impl::RemoveTrayIcon() {
        if (!trayIconAdded) return;
        NOTIFYICONDATAW data = TrayIconData();
        Shell_NotifyIconW(NIM_DELETE, &data);
        trayIconAdded = false;
    }

// Restores the hidden main window and drops the tray icon.
    void Win32Ui::Impl::RestoreFromTray() {
        if (window) {
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        Win32Ui::Impl::RemoveTrayIcon();
    }

// Right-click tray menu: Open restores the window, Exit closes for real.
    void Win32Ui::Impl::ShowTrayMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, kTrayMenuOpen, L"Open Rivan");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"Exit");
        POINT cursor{};
        GetCursorPos(&cursor);
        // Required so the menu dismisses correctly when the user clicks elsewhere.
        SetForegroundWindow(window);
        const int command = static_cast<int>(TrackPopupMenu(
            menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            cursor.x, cursor.y, 0, window, nullptr));
        DestroyMenu(menu);
        if (command == kTrayMenuOpen) {
            Win32Ui::Impl::RestoreFromTray();
        } else if (command == kTrayMenuExit) {
            Win32Ui::Impl::RemoveTrayIcon();
            DestroyWindow(window);
        }
    }

} // namespace rivan::ui
