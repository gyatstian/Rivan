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
        playlistSearchBounds = Rect(right.left, localSearch.bottom + 3, right.right, right.bottom);
        // SCREEN: Current-folder track list with per-subfolder section headers.
        Win32Ui::Impl::DrawSectionedTracks(playlistSearchBounds, playlistSearchScroll, playlistSearchRows,
                            b[5].Get(), b[6].Get(), b[6].Get(), b[11].Get(), b[12].Get(), b[10].Get());
    }

bool Win32Ui::Impl::IsUserPlaylistId(std::uint64_t id) const noexcept {
    for (const auto& playlist : model.playlists) {
        if (playlist.id == id) return playlist.user;
    }
    return false;
}

bool Win32Ui::Impl::IsReorderablePlaylistId(std::uint64_t id) const noexcept {
    for (const auto& playlist : model.playlists) {
        if (playlist.id == id) return playlist.reorderable;
    }
    return false;
}

std::uint64_t Win32Ui::Impl::PlaylistReorderParent(std::uint64_t id) const noexcept {
    for (const auto& playlist : model.playlists) {
        if (playlist.id == id) return playlist.parentId;
    }
    return ~1ULL;
}

void Win32Ui::Impl::ResetTrackSelectionForPlaylist(std::uint64_t playlistId) {
    if (trackSelectionPlaylist == playlistId) return;
    trackSelectionPlaylist = playlistId;
    trackSelection.clear();
    trackAnchor = static_cast<std::size_t>(-1);
}

std::vector<std::size_t> Win32Ui::Impl::SelectedTrackIndicesSorted() const {
    return std::vector<std::size_t>(trackSelection.begin(), trackSelection.end());
}

void Win32Ui::Impl::ApplyTrackClickSelection(std::size_t modelIndex, bool ctrl, bool shift) {
    ResetTrackSelectionForPlaylist(model.selectedPlaylistId);
    if (modelIndex >= model.tracks.size()) return;
    if (shift && trackAnchor != static_cast<std::size_t>(-1)) {
        const std::size_t lo = std::min(trackAnchor, modelIndex);
        const std::size_t hi = std::max(trackAnchor, modelIndex);
        if (!ctrl) trackSelection.clear();
        for (std::size_t i = lo; i <= hi && i < model.tracks.size(); ++i) {
            trackSelection.insert(i);
        }
        return;
    }
    if (ctrl) {
        if (!trackSelection.insert(modelIndex).second) trackSelection.erase(modelIndex);
        trackAnchor = modelIndex;
        return;
    }
    trackSelection.clear();
    trackSelection.insert(modelIndex);
    trackAnchor = modelIndex;
}

void Win32Ui::Impl::ApplyPlaylistClickSelection(std::uint64_t id, std::size_t treeIndex,
                                                bool ctrl, bool shift) {
    if ((ctrl || shift) && IsUserPlaylistId(id)) {
        if (shift && playlistAnchorId != 0) {
            std::size_t anchorIndex = static_cast<std::size_t>(-1);
            for (std::size_t i = 0; i < model.playlists.size(); ++i) {
                if (model.playlists[i].id == playlistAnchorId) { anchorIndex = i; break; }
            }
            if (anchorIndex != static_cast<std::size_t>(-1)) {
                const std::size_t lo = std::min(anchorIndex, treeIndex);
                const std::size_t hi = std::max(anchorIndex, treeIndex);
                if (!ctrl) playlistSelection.clear();
                for (std::size_t i = lo; i <= hi && i < model.playlists.size(); ++i) {
                    if (model.playlists[i].user) playlistSelection.insert(model.playlists[i].id);
                }
                return;
            }
        }
        if (!playlistSelection.insert(id).second) playlistSelection.erase(id);
        playlistAnchorId = id;
        return;
    }
    // Plain click: clear multi-selection and open the playlist.
    playlistSelection.clear();
    playlistAnchorId = id;
    playlistQuery.clear();
    playlistQuerySelectAll = false;
    playlistSearchScroll = 0;
    if (window) KillTimer(window, kYoutubeSearchDebounceTimer);
    try { host.SelectPlaylist(id); } catch (...) {}
}

void Win32Ui::Impl::BeginTrackPress(const HitRegion& hit, float x, float y) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    ApplyTrackClickSelection(hit.index, ctrl, shift);
    // Defer play to release: a plain single click on an already-mono-selected row plays;
    // ctrl/shift never play. A press-drag on an editable playlist reorders instead.
    pendingTrackActivate = !ctrl && !shift;
    pendingTrackActivateId = hit.id;
    dragKind = DragKind::Track;
    dragActive = false;
    dragStart = {x, y};
    dragTrackIndex = hit.index;
    dragTrackPlaylistId = hit.index < model.tracks.size()
                              ? model.tracks[hit.index].sourcePlaylistId
                              : 0;
    dropTrackIndex = static_cast<std::size_t>(-1);
    SetCapture(window);
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::BeginPlaylistPress(const HitRegion& hit, float x, float y) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    ApplyPlaylistClickSelection(hit.id, hit.index, ctrl, shift);
    // Directory folders and user playlists reorder by drag; others just select/open.
    if (IsReorderablePlaylistId(hit.id) && !ctrl && !shift) {
        dragKind = DragKind::Playlist;
        dragActive = false;
        dragStart = {x, y};
        dragPlaylistId = hit.id;
        dragPlaylistParent = PlaylistReorderParent(hit.id);
        dropBeforePlaylistId = 0;
        dropIntoPlaylistId = 0;
        dropAtPlaylistEnd = false;
        SetCapture(window);
    }
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::UpdateRowDrag(float x, float y) {
    if (!dragActive) {
        // Promote to an active drag only after moving past a small threshold, so a
        // shaky click is not mistaken for a reorder.
        const float dx = x - dragStart.x;
        const float dy = y - dragStart.y;
        if (dx * dx + dy * dy < 25.0F) return;
        // Parent-folder views contain rows from descendant folders. The source playlist
        // carried by the row tells us which direct list is safe to reorder.
        if (dragKind == DragKind::Track &&
            (!model.selectedPlaylistTracksReorderable || dragTrackPlaylistId == 0)) return;
        dragActive = true;
    }
    if (dragKind == DragKind::Track) {
        dropTrackIndex = ResolveTrackDrop(y);
    } else if (dragKind == DragKind::Playlist) {
        ResolvePlaylistDrop(y);
    }
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::FinishRowDrag() noexcept {
    const DragKind kind = dragKind;
    const bool wasActive = dragActive;
    dragKind = DragKind::None;
    dragActive = false;
    try {
        if (wasActive && kind == DragKind::Track) {
            FinishTrackDrag();
        } else if (wasActive && kind == DragKind::Playlist) {
            FinishPlaylistDrag();
        } else if (!wasActive && kind == DragKind::Track && pendingTrackActivate) {
            // A plain click that never became a drag: play the row.
            host.ActivateTrack(pendingTrackActivateId);
        }
    } catch (...) {}
    pendingTrackActivate = false;
    dropTrackIndex = static_cast<std::size_t>(-1);
    dragTrackPlaylistId = 0;
    dropBeforePlaylistId = 0;
    dropIntoPlaylistId = 0;
    dropAtPlaylistEnd = false;
    InvalidateRect(window, nullptr, FALSE);
}

std::size_t Win32Ui::Impl::ResolveTrackDrop(float y) const {
    // Use rendered hit rows instead of playlistListBounds: Directory folders render in
    // the current-folder pane, while user playlists render in the editor pane. Restrict
    // the target to the source playlist of the row being dragged.
    if (model.tracks.empty() || dragTrackPlaylistId == 0) return 0;
    const HitRegion* first = nullptr;
    const HitRegion* last = nullptr;
    std::size_t firstSourceIndex = 0;
    std::size_t lastSourceIndex = 0;
    for (const auto& hit : hits) {
        // Both current-folder and playlist-editor panes can contribute track hits. Restrict
        // this drag to its original pane; otherwise a same-height row in the other pane
        // can replace the correct insertion target after a redraw.
        if (hit.kind != HitKind::Track || hit.index >= model.tracks.size() ||
            model.tracks[hit.index].sourcePlaylistId != dragTrackPlaylistId ||
            dragStart.x < hit.bounds.left || dragStart.x >= hit.bounds.right) continue;
        const auto sourceIndex = SourceTrackIndex(&model.tracks[hit.index]);
        if (sourceIndex == static_cast<std::size_t>(-1)) continue;
        if (first == nullptr || hit.bounds.top < first->bounds.top) {
            first = &hit;
            firstSourceIndex = sourceIndex;
        }
        if (last == nullptr || hit.bounds.top > last->bounds.top) {
            last = &hit;
            lastSourceIndex = sourceIndex;
        }
        if (y >= hit.bounds.top && y < hit.bounds.bottom) {
            return y > (hit.bounds.top + hit.bounds.bottom) * 0.5F
                       ? sourceIndex + 1
                       : sourceIndex;
        }
    }
    if (first == nullptr) return 0;
    if (y <= first->bounds.top) return firstSourceIndex;
    return lastSourceIndex + 1;
}

void Win32Ui::Impl::ResolvePlaylistDrop(float y) {
    dropBeforePlaylistId = 0;
    dropIntoPlaylistId = 0;
    dropAtPlaylistEnd = false;
    constexpr float rowH = 20.0F;
    if (model.playlists.empty()) return;
    // A reorder is scoped to the dragged row's sibling group: only rows that share its
    // parentId are valid snap targets. dropAtPlaylistEnd means "end of that group"
    // (beforeId 0), which the manager resolves against the dragged folder's own parent.
    const std::uint64_t group = dragPlaylistParent;
    const auto isSibling = [&](std::size_t i) noexcept {
        return model.playlists[i].reorderable && model.playlists[i].parentId == group;
    };
    const float local = y - treeListBounds.top;
    const long long rel = static_cast<long long>(std::floor(local / rowH));
    const long long rowUnder = static_cast<long long>(treeScroll) + rel;
    std::size_t targetRow = 0;
    if (rowUnder < 0) {
        targetRow = 0;  // Above the list: snap before the first sibling.
    } else if (rowUnder >= static_cast<long long>(model.playlists.size())) {
        dropAtPlaylistEnd = true;
        return;
    } else {
        const auto index = static_cast<std::size_t>(rowUnder);
        const float rowTop = treeListBounds.top +
            static_cast<float>(index - treeScroll) * rowH;
        // Dropping over a row's middle moves a filesystem playlist into that folder.
        // Top and bottom quarters retain precise sibling ordering behavior.
        const bool sourceIsFolder = dragPlaylistParent != ui::kUserPlaylistGroupParent;
        const auto& hovered = model.playlists[index];
        const bool targetIsFolder = hovered.reorderable &&
                                    hovered.parentId != ui::kUserPlaylistGroupParent;
        if (sourceIsFolder && targetIsFolder && hovered.id != dragPlaylistId &&
            y >= rowTop + rowH * 0.25F && y <= rowTop + rowH * 0.75F) {
            dropIntoPlaylistId = hovered.id;
            return;
        }
        const bool lowerHalf = y > rowTop + rowH * 0.5F;
        targetRow = lowerHalf ? index + 1 : index;
    }
    if (targetRow >= model.playlists.size()) { dropAtPlaylistEnd = true; return; }
    // Snap to the next sibling at/after target; else append to the end of the group.
    for (std::size_t i = targetRow; i < model.playlists.size(); ++i) {
        if (isSibling(i)) { dropBeforePlaylistId = model.playlists[i].id; return; }
    }
    dropAtPlaylistEnd = true;
}

void Win32Ui::Impl::FinishTrackDrag() {
    if (!model.selectedPlaylistTracksReorderable ||
        dropTrackIndex == static_cast<std::size_t>(-1)) return;
    const auto selected = SelectedTrackIndicesSorted();
    if (selected.empty() || dragTrackPlaylistId == 0) return;
    // A mixed selection cannot be represented by one playlist reorder. Do not silently
    // move only part of it when rows from multiple subfolders are selected.
    for (const auto index : selected) {
        if (index >= model.tracks.size() ||
            model.tracks[index].sourcePlaylistId != dragTrackPlaylistId) return;
    }
    std::vector<std::size_t> sourceIndices;
    sourceIndices.reserve(selected.size());
    for (std::size_t index = 0; index < model.tracks.size(); ++index) {
        if (model.tracks[index].sourcePlaylistId != dragTrackPlaylistId) continue;
        if (std::binary_search(selected.begin(), selected.end(), index)) {
            sourceIndices.push_back(SourceTrackIndex(&model.tracks[index]));
        }
    }
    host.ReorderSelectedTracks(dragTrackPlaylistId, sourceIndices, dropTrackIndex);
    trackSelection.clear();
    trackAnchor = static_cast<std::size_t>(-1);
}

void Win32Ui::Impl::FinishPlaylistDrag() {
    if (dragPlaylistId == 0) return;
    if (dropIntoPlaylistId != 0) {
        host.MovePlaylistInto(dragPlaylistId, dropIntoPlaylistId);
        dragPlaylistId = 0;
        return;
    }
    if (!dropAtPlaylistEnd && dropBeforePlaylistId == 0) return;
    if (dropBeforePlaylistId == dragPlaylistId) return;
    host.ReorderUserPlaylist(dragPlaylistId, dropAtPlaylistEnd ? 0 : dropBeforePlaylistId);
    dragPlaylistId = 0;
}

void Win32Ui::Impl::RemoveSelectedTracks() {
    if (!model.selectedPlaylistIsUser) {
        MessageBoxW(window,
                    L"REM removes songs from a user playlist. Select a playlist you created "
                    L"with the + button first.",
                    L"Rivan", MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto indices = SelectedTrackIndicesSorted();
    if (indices.empty()) {
        MessageBoxW(window,
                    L"Select one or more songs in the playlist editor, then press REM.",
                    L"Rivan", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (model.selectedPlaylistDeletesFiles) {
        const std::wstring count = std::to_wstring(indices.size());
        const std::wstring message =
            L"Delete " + count + L" selected song" + (indices.size() == 1 ? L"" : L"s") +
            L" from disk?\n\nREM permanently removes these source file" +
            (indices.size() == 1 ? L"" : L"s") + L". This cannot be undone.";
        if (MessageBoxW(window, message.c_str(), L"Delete music files",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
            return;
        }
    }
    try { host.RemoveTracksAt(indices); } catch (...) {}
    trackSelection.clear();
    trackAnchor = static_cast<std::size_t>(-1);
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::BeginCreatePlaylist() {
    playlistNameEditing = true;
    playlistNameRenaming = false;
    playlistRenameId = 0;
    playlistNameBuffer.clear();
    activeSearch = SearchTarget::None;
    SetFocus(window);
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::CommitPlaylistName() {
    const std::wstring name = playlistNameBuffer;
    const bool renaming = playlistNameRenaming;
    const std::uint64_t renameId = playlistRenameId;
    playlistNameEditing = false;
    playlistNameRenaming = false;
    playlistNameBuffer.clear();
    if (name.empty()) { InvalidateRect(window, nullptr, FALSE); return; }
    try {
        if (renaming) host.RenameUserPlaylist(renameId, name);
        else host.CreateUserPlaylist(name);
    } catch (...) {}
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::CancelPlaylistName() {
    playlistNameEditing = false;
    playlistNameRenaming = false;
    playlistNameBuffer.clear();
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::CommitTrackName() {
    const std::size_t index = trackRenameIndex;
    const std::wstring name = trackNameBuffer;
    trackNameEditing = false;
    trackRenameIndex = static_cast<std::size_t>(-1);
    trackNameBuffer.clear();
    trackNameCursor = 0;
    trackNameSelectAll = false;
    if (!name.empty()) {
        try { host.RenameTrackAt(index, name); } catch (...) {}
    }
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::CancelTrackName() {
    trackNameEditing = false;
    trackRenameIndex = static_cast<std::size_t>(-1);
    trackNameBuffer.clear();
    trackNameCursor = 0;
    trackNameSelectAll = false;
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::BeginTrackRename(std::size_t modelIndex) {
    if (modelIndex >= model.tracks.size()) return;
    trackNameEditing = true;
    trackRenameIndex = modelIndex;
    const auto path = std::filesystem::path(model.tracks[modelIndex].filePath);
    trackNameBuffer = path.stem().wstring();
    trackNameCursor = trackNameBuffer.size();
    trackNameSelectAll = !trackNameBuffer.empty();
    SetFocus(window);
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::PointerRightDown(float x, float y) {
    if (windowKind != WindowKind::Main || model.miniPlayer) return;
    if (HasTitlebar()) y -= kTitlebarHeight;
    SetFocus(window);
    // Menus depend on playlist permissions; repaint can lag a recent selection.
    try { host.SnapshotUiModel(model); } catch (...) {}
    mouse = {static_cast<LONG>(x), static_cast<LONG>(y)};
    const HitRegion* found = HitTestContent(x, y);
    if (found == nullptr) return;
    if (found->kind == HitKind::Track) {
        ResetTrackSelectionForPlaylist(model.selectedPlaylistId);
        // Right-clicking outside the current selection re-selects just that row.
        if (!trackSelection.contains(found->index)) {
            trackSelection.clear();
            trackSelection.insert(found->index);
            trackAnchor = found->index;
            InvalidateRect(window, nullptr, FALSE);
        }
        ShowTrackContextMenu(found->index);
    } else if (found->kind == HitKind::Playlist && IsUserPlaylistId(found->id)) {
        if (!playlistSelection.contains(found->id)) {
            playlistSelection.clear();
            playlistSelection.insert(found->id);
            playlistAnchorId = found->id;
            InvalidateRect(window, nullptr, FALSE);
        }
        ShowPlaylistContextMenu(found->id);
    }
}

void Win32Ui::Impl::SyncMouseFromCursor() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    ScreenToClient(window, &cursor);
    mouse = cursor;
}

void Win32Ui::Impl::ShowTrackContextMenu(std::size_t modelIndex) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    HMENU addMenu = CreatePopupMenu();
    HMENU moveMenu = CreatePopupMenu();
    // Add-to-playlist submenu: one command id per user playlist (base 3000), plus a
    // "New playlist..." entry (id 2). Remove = 3, Duplicate = 4.
    constexpr UINT kAddBase = 3000;
    constexpr UINT kMoveBase = 4000;
    std::vector<std::uint64_t> addTargets;
    std::vector<std::uint64_t> moveTargets;
    if (addMenu) {
        for (const auto& playlist : model.playlists) {
            if (!playlist.user) continue;
            AppendMenuW(addMenu, MF_STRING, kAddBase + addTargets.size(), playlist.name.c_str());
            addTargets.push_back(playlist.id);
        }
        if (!addTargets.empty()) AppendMenuW(addMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(addMenu, MF_STRING, 2, L"New playlist...");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(addMenu), L"Add to playlist");
    }
    if (moveMenu) {
        for (const auto& playlist : model.playlists) {
            if (!playlist.user || playlist.id == model.selectedPlaylistId) continue;
            AppendMenuW(moveMenu, MF_STRING, kMoveBase + moveTargets.size(), playlist.name.c_str());
            moveTargets.push_back(playlist.id);
        }
        if (moveTargets.empty()) {
            AppendMenuW(moveMenu, MF_STRING | MF_GRAYED, kMoveBase, L"No other playlists");
        }
        AppendMenuW(menu, MF_POPUP | (model.selectedPlaylistCanMoveTracks ? 0U : MF_GRAYED),
                    reinterpret_cast<UINT_PTR>(moveMenu), L"Move to playlist");
    }
    // The snapshot records selected playlist permissions directly. Looking it up again
    // in tree rows can be stale while selection and repaint are being synchronized.
    const bool editable = model.selectedPlaylistIsUser;
    const wchar_t* removeLabel = model.selectedPlaylistDeletesFiles ? L"Delete from disk" : L"Remove";
    AppendMenuW(menu, MF_STRING | (editable ? 0U : MF_GRAYED), 3, removeLabel);
    AppendMenuW(menu, MF_STRING | (editable ? 0U : MF_GRAYED), 4, L"Duplicate");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 5, L"Rename");
    const bool hasAudio = std::any_of(trackSelection.begin(), trackSelection.end(), [this](std::size_t index) {
        return index < model.tracks.size() && model.tracks[index].audioFile;
    });
    AppendMenuW(menu, MF_STRING | (hasAudio ? 0U : MF_GRAYED), 6, L"Change cover");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window);
    const int command = static_cast<int>(TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, window, nullptr));
    DestroyMenu(menu);
    SyncMouseFromCursor();
    if (command == 0) {
        InvalidateRect(window, nullptr, FALSE);
        return;
    }

    const auto indices = SelectedTrackIndicesSorted();
    try {
        if (command == 2) {
            // New playlist from selection: create empty, then add the selection to it once
            // it becomes the selected playlist. Simpler path: create then add by capturing.
            BeginCreatePlaylist();
        } else if (command == 3) {
            RemoveSelectedTracks();
        } else if (command == 4) {
            if (!indices.empty()) host.DuplicateTracksAt(indices);
        } else if (command == 5 && modelIndex < model.tracks.size()) {
            BeginTrackRename(modelIndex);
        } else if (command == 6) {
            if (!indices.empty()) {
                host.ChangeTracksCover(indices);
                trackCoverCache.clear();
                trackCoverUseCounter = 0;
                nextTrackCoverLookup = {};
            }
        } else if (command >= static_cast<int>(kMoveBase)) {
            const std::size_t which = static_cast<std::size_t>(command) - kMoveBase;
            if (which < moveTargets.size() && !indices.empty()) {
                host.MoveTracksToPlaylist(moveTargets[which], indices);
            }
        } else if (command >= static_cast<int>(kAddBase)) {
            const std::size_t which = static_cast<std::size_t>(command) - kAddBase;
            if (which < addTargets.size() && !indices.empty()) {
                host.AddTracksToPlaylist(addTargets[which], indices);
            }
        }
    } catch (...) {}
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::ShowPlaylistContextMenu(std::uint64_t playlistId) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, L"Rename");
    AppendMenuW(menu, MF_STRING, 2, L"Delete");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window);
    const int command = static_cast<int>(TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, window, nullptr));
    DestroyMenu(menu);
    SyncMouseFromCursor();
    if (command == 1) {
        // Inline rename of the clicked playlist.
        playlistNameEditing = true;
        playlistNameRenaming = true;
        playlistRenameId = playlistId;
        for (const auto& playlist : model.playlists) {
            if (playlist.id == playlistId) { playlistNameBuffer = playlist.name; break; }
        }
        SetFocus(window);
        InvalidateRect(window, nullptr, FALSE);
    } else if (command == 2) {
        // Delete the whole multi-selection when the clicked row is part of it.
        std::vector<std::uint64_t> ids;
        if (playlistSelection.contains(playlistId)) {
            ids.assign(playlistSelection.begin(), playlistSelection.end());
        } else {
            ids.push_back(playlistId);
        }
        std::size_t folders = 0;
        std::size_t tracks = 0;
        for (std::size_t i = 0; i < model.playlists.size(); ++i) {
            const auto& playlist = model.playlists[i];
            if (std::find(ids.begin(), ids.end(), playlist.id) == ids.end()) continue;
            bool coveredBySelectedParent = false;
            for (std::size_t parent = i; parent-- > 0;) {
                if (model.playlists[parent].depth >= playlist.depth) continue;
                coveredBySelectedParent =
                    std::find(ids.begin(), ids.end(), model.playlists[parent].id) != ids.end();
                break;
            }
            if (coveredBySelectedParent) continue;
            ++folders;
            tracks += playlist.trackCount;
        }
        const std::wstring message =
            L"Delete " + std::to_wstring(folders) + L" folder" + (folders == 1 ? L"" : L"s") +
            L" and " + std::to_wstring(tracks) + L" music file" +
            (tracks == 1 ? L"" : L"s") +
            L" from disk?\n\nThis permanently deletes every selected folder and its contents. "
            L"This cannot be undone.";
        if (MessageBoxW(window, message.c_str(), L"Delete music folders",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
        playlistSelection.clear();
        try { host.DeleteUserPlaylists(ids); } catch (...) {}
        InvalidateRect(window, nullptr, FALSE);
    } else {
        InvalidateRect(window, nullptr, FALSE);
    }
}

} // namespace rivan::ui
