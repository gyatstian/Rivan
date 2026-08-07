// RivanLibraryModule.cpp
// Rendering and module-owned interaction for the RIVAN LIBRARY module.
#include "../ui/Win32UiImpl.h"

namespace rivan::ui {

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

[[nodiscard]] std::wstring Win32Ui::Impl::SelectedPlaylistName() const {
        for (const auto& playlist : model.playlists) {
            if (playlist.selected) return playlist.name;
        }
        return L"PLAYLIST EDITOR";
    }

void Win32Ui::Impl::DrawLibrary(const D2D1_RECT_F& bounds,
                      std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b) {
        auto content = DrawPanel(bounds, UiModuleRegistry::Get(ModuleId::RivanLibrary).Title(),
                                 b[1].Get(), b[2].Get(), b[3].Get(), b[4].Get(),
                                 b[13].Get(), b[7].Get(), ModuleId::RivanLibrary);
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

            const auto localSearch =
                Rect(right.left, right.top + 19, right.right - 72, right.top + 43);
            Win32Ui::Impl::DrawSearch(localSearch, playlistQuery, SearchTarget::Playlist, b[5].Get(), b[3].Get(),
                       b[4].Get(), b[6].Get(), b[6].Get());
            const auto go = Rect(right.right - 68, right.top + 19, right.right, right.top + 43);
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
        playlistSearchBounds = Rect(right.left, localSearch.bottom + 3, right.right, right.bottom);
        // SCREEN: Current-folder track list with per-subfolder section headers.
        Win32Ui::Impl::DrawSectionedTracks(playlistSearchBounds, playlistSearchScroll, playlistSearchRows,
                            b[5].Get(), b[6].Get(), b[6].Get(), b[11].Get(), b[12].Get(), b[10].Get());
    }

} // namespace rivan::ui
