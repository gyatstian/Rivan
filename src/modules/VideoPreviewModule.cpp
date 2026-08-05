// VideoPreviewModule.cpp
// Rendering and preview lifecycle for the VIDEO PREVIEW module.
#include "../ui/Win32UiImpl.h"

namespace rivan::ui {

void Win32Ui::Impl::ClearFilePreview() noexcept {
        StopPreviewWorker();
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
        previewVideoBounds = {};
    }

void Win32Ui::Impl::StopPreviewWorker() noexcept {
        if (previewWorker.joinable()) {
            // Source-reader calls can block in synchronous media I/O. Cancel the
            // worker's pending I/O before joining so preview changes cannot stall the
            // UI thread indefinitely.
            CancelSynchronousIo(previewWorker.native_handle());
        }
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
            if (!previewWorkerFailed.load(std::memory_order_acquire)) {
                previewWake.notify_all();
                return true;
            }
            StopPreviewWorker();
        }
        previewWorkerFailed.store(false, std::memory_order_release);
        previewWorker = std::jthread([this](std::stop_token stop) {
            const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool comInitialized = SUCCEEDED(comResult);
            const bool mediaFoundationInitialized =
                comInitialized && SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
            if (!comInitialized || !mediaFoundationInitialized) {
                previewWorkerFailed.store(true, std::memory_order_release);
                if (mediaFoundationInitialized) MFShutdown();
                if (comInitialized) CoUninitialize();
                return;
            }

            try {
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
                    previewWake.wait_for(lock, stop, std::chrono::milliseconds(100), [this, activeGeneration] {
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
                if (generation != activeGeneration || !reader) {
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
                struct BufferUnlock final {
                    IMFMediaBuffer* buffer{};
                    ~BufferUnlock() {
                        if (buffer != nullptr) buffer->Unlock();
                    }
                } unlock{buffer.Get()};
                const UINT sourceStride = static_cast<UINT>(std::abs(stride));
                const std::size_t requiredBytes = static_cast<std::size_t>(sourceStride) * height;
                if (stride != 0 && sourceStride >= packedStride && length >= requiredBytes) {
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
                if (window) InvalidateRect(window, nullptr, FALSE);
            }
            } catch (...) {
                // Preview decoding is optional.  Allocation/decoder failures must not
                // escape a jthread entry point and terminate the entire application.
                previewWorkerFailed.store(true, std::memory_order_release);
            }
            MFShutdown();
            if (comInitialized) CoUninitialize();
        });
        return true;
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
        if (path.empty() || !IsVideoPreviewModuleVisible()) return;
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
        if (!previewIsVideo || !IsVideoPreviewModuleVisible()) return;
        previewFullscreen = true;
        InvalidateRect(window, nullptr, FALSE);
    }

void Win32Ui::Impl::SyncFilePreview(bool advanceVideo) {
        if (!IsVideoPreviewModuleVisible()) {
            previewFullscreen = false;
            if (!previewPath.empty() || previewBitmap) ClearFilePreview();
            return;
        }
        // Preview follows now-playing even while the Youtube browser or another folder is open.
        const auto active = ActivePreviewPath();
        if (active != previewPath || (IsVideoPath(active) && !previewWorker.joinable())) {
            LoadFilePreview(active);
        } else if (previewIsVideo && previewWorkerFailed.load(std::memory_order_acquire)) {
            // A decoder failure is terminal for this preview request. Keep the worker
            // from being recreated on every paint and fall back to static artwork.
            previewIsVideo = false;
            previewFullscreen = false;
            previewBitmap.Reset();
            (void)LoadCoverArt(active);
        }
        else if (advanceVideo && previewIsVideo) UpdateVideoPreviewFrame();
        if (!previewIsVideo) previewFullscreen = false;
    }

void Win32Ui::Impl::DrawVideoPreview(
    const D2D1_RECT_F& bounds,
    std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
    auto content = DrawPanel(bounds, UiModuleRegistry::Get(ModuleId::VideoPreview).Title(),
                             b[1].Get(), b[2].Get(), b[3].Get(), b[4].Get(),
                             b[13].Get(), b[7].Get(), ModuleId::VideoPreview);
    previewVideoBounds = {};
    const auto preview = Rect(content.left + 3.0F, content.top + 3.0F,
                              content.right - 3.0F, content.bottom - 3.0F);
    if (Width(preview) <= 2.0F || Height(preview) <= 2.0F) return;

    Win32Ui::Impl::DrawBevel(preview, b[5].Get(), b[3].Get(), b[4].Get(), true, 2.0F);
    previewVideoBounds = preview;
    if (!model.filePreviewEnabled) {
        Win32Ui::Impl::DrawText(L"FILE PREVIEW DISABLED", preview, b[10].Get(),
                                smallFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (previewBitmap) {
        Win32Ui::Impl::DrawPreviewBitmap(previewVideoBounds);
        if (previewIsVideo) {
            Win32Ui::Impl::AddSimpleHit(previewVideoBounds, HitKind::FilePreviewFullscreen);
        }
    } else {
        const wchar_t* message = L"NOTHING PLAYING";
        if (!ActivePreviewPath().empty()) {
            message = previewIsVideo ? L"LOADING PREVIEW..." : L"NO COVER AVAILABLE";
        }
        Win32Ui::Impl::DrawText(message, preview, b[10].Get(), smallFormat.Get(),
                                DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

bool Win32Ui::Impl::IsVideoPreviewModuleVisible() const noexcept {
    if (windowKind != WindowKind::Main || model.miniPlayer || !model.filePreviewEnabled) {
        return false;
    }
    const auto* item = model.moduleLayout.Find(ModuleId::VideoPreview);
    if (!item || !item->visible || item->collapsed) return false;
    if (model.moduleLayout.IsTabbed(ModuleId::VideoPreview) &&
        model.moduleLayout.tabOrder[model.moduleLayout.ActiveTabIndex()] != ModuleId::VideoPreview) {
        return false;
    }
    return true;
}

} // namespace rivan::ui
