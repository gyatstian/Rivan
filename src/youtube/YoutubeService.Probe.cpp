// YoutubeService.Probe.cpp
#include "YoutubeService.Internal.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace rivan::youtube::detail {
namespace {

struct JsonValue final {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Object, Array };
    Kind kind{Kind::Null};
    double number{};
    std::string string;
    std::vector<std::pair<std::string, JsonValue>> object;
    std::vector<JsonValue> array;
};

class JsonParser final {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    [[nodiscard]] bool Parse(JsonValue& value) {
        SkipWhitespace();
        return ParseValue(value, 0);
    }

private:
    void SkipWhitespace() {
        while (position_ < text_.size()) {
            const char ch = text_[position_];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
            ++position_;
        }
    }

    [[nodiscard]] bool Consume(char expected) {
        SkipWhitespace();
        if (position_ >= text_.size() || text_[position_] != expected) return false;
        ++position_;
        return true;
    }

    [[nodiscard]] bool ParseValue(JsonValue& value, std::size_t depth) {
        if (depth > 64) return false;
        SkipWhitespace();
        if (position_ >= text_.size()) return false;
        switch (text_[position_]) {
        case '{': return ParseObject(value, depth + 1);
        case '[': return ParseArray(value, depth + 1);
        case '"':
            value.kind = JsonValue::Kind::String;
            return ParseString(value.string);
        case 't': return ParseLiteral(value, "true", JsonValue::Kind::Bool);
        case 'f': return ParseLiteral(value, "false", JsonValue::Kind::Bool);
        case 'n': return ParseLiteral(value, "null", JsonValue::Kind::Null);
        default: return ParseNumber(value);
        }
    }

    [[nodiscard]] bool ParseLiteral(JsonValue& value, std::string_view literal,
                                    JsonValue::Kind kind) {
        if (text_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        value.kind = kind;
        return true;
    }

    [[nodiscard]] bool ParseNumber(JsonValue& value) {
        const std::size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        if (position_ >= text_.size()) return false;
        if (text_[position_] == '0') {
            ++position_;
        } else {
            if (text_[position_] < '1' || text_[position_] > '9') return false;
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            const std::size_t fractionStart = position_;
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
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
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9') {
                ++position_;
            }
            if (position_ == exponentStart) return false;
        }
        const auto numberText = text_.substr(start, position_ - start);
        const auto [end, error] = std::from_chars(
            numberText.data(), numberText.data() + numberText.size(), value.number);
        if (error != std::errc{} || end != numberText.data() + numberText.size()) return false;
        value.kind = JsonValue::Kind::Number;
        return true;
    }

    static void AppendUtf8(std::string& out, std::uint32_t codePoint) {
        if (codePoint <= 0x7f) {
            out.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else if (codePoint <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else if (codePoint <= 0x10ffff) {
            out.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        }
    }

    [[nodiscard]] bool ParseHex4(std::uint32_t& codeUnit) {
        if (position_ + 4 > text_.size()) return false;
        codeUnit = 0;
        for (int n = 0; n < 4; ++n) {
            const char digit = text_[position_++];
            codeUnit <<= 4;
            if (digit >= '0' && digit <= '9') codeUnit += digit - '0';
            else if (digit >= 'a' && digit <= 'f') codeUnit += digit - 'a' + 10;
            else if (digit >= 'A' && digit <= 'F') codeUnit += digit - 'A' + 10;
            else return false;
        }
        return true;
    }

    [[nodiscard]] bool ParseString(std::string& value) {
        if (position_ >= text_.size() || text_[position_] != '"') return false;
        ++position_;
        value.clear();
        while (position_ < text_.size()) {
            const unsigned char ch = static_cast<unsigned char>(text_[position_++]);
            if (ch == '"') return true;
            if (ch < 0x20) return false;
            if (ch != '\\') {
                value.push_back(static_cast<char>(ch));
                continue;
            }
            if (position_ >= text_.size()) return false;
            const char escaped = text_[position_++];
            switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u': {
                std::uint32_t codePoint = 0;
                if (!ParseHex4(codePoint)) return false;
                if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
                    if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
                        text_[position_ + 1] != 'u') {
                        return false;
                    }
                    position_ += 2;
                    std::uint32_t lowSurrogate = 0;
                    if (!ParseHex4(lowSurrogate) || lowSurrogate < 0xdc00 ||
                        lowSurrogate > 0xdfff) {
                        return false;
                    }
                    codePoint = 0x10000 + ((codePoint - 0xd800) << 10) +
                                (lowSurrogate - 0xdc00);
                } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
                    return false;
                }
                AppendUtf8(value, codePoint);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool ParseObject(JsonValue& value, std::size_t depth) {
        value.kind = JsonValue::Kind::Object;
        value.object.clear();
        ++position_;
        SkipWhitespace();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return true;
        }
        for (;;) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(key) || !Consume(':')) return false;
            JsonValue child;
            if (!ParseValue(child, depth)) return false;
            value.object.emplace_back(std::move(key), std::move(child));
            SkipWhitespace();
            if (position_ >= text_.size()) return false;
            if (text_[position_] == '}') {
                ++position_;
                return true;
            }
            if (text_[position_] != ',') return false;
            ++position_;
        }
    }

    [[nodiscard]] bool ParseArray(JsonValue& value, std::size_t depth) {
        value.kind = JsonValue::Kind::Array;
        value.array.clear();
        ++position_;
        SkipWhitespace();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return true;
        }
        for (;;) {
            JsonValue child;
            if (!ParseValue(child, depth)) return false;
            value.array.push_back(std::move(child));
            SkipWhitespace();
            if (position_ >= text_.size()) return false;
            if (text_[position_] == ']') {
                ++position_;
                return true;
            }
            if (text_[position_] != ',') return false;
            ++position_;
        }
    }

    std::string_view text_;
    std::size_t position_{};
};

const JsonValue* Member(const JsonValue& object, std::string_view name) {
    if (object.kind != JsonValue::Kind::Object) return nullptr;
    const auto found = std::find_if(
        object.object.begin(), object.object.end(),
        [name](const auto& member) { return member.first == name; });
    return found == object.object.end() ? nullptr : &found->second;
}

std::string StringMember(const JsonValue& object, std::string_view name) {
    const auto* value = Member(object, name);
    return value && value->kind == JsonValue::Kind::String ? value->string : std::string{};
}

std::wstring VideoIdMember(const JsonValue& object) {
    return Utf8ToWide(StringMember(object, "id"));
}

double NumberMember(const JsonValue& object, std::string_view name) {
    const auto* value = Member(object, name);
    return value && value->kind == JsonValue::Kind::Number && std::isfinite(value->number)
               ? std::max(0.0, value->number)
               : 0.0;
}

std::uint64_t SizeMember(const JsonValue& object) {
    const double exact = NumberMember(object, "filesize");
    const double approximate = NumberMember(object, "filesize_approx");
    const double value = exact > 0.0 ? exact : approximate;
    return value >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
               ? std::numeric_limits<std::uint64_t>::max()
               : static_cast<std::uint64_t>(value);
}

} // namespace

std::optional<YoutubeProbe> ParseProbeJson(const std::string& stdoutText) {
    for (std::size_t start = stdoutText.find('{'); start != std::string::npos;
         start = stdoutText.find('{', start + 1)) {
        JsonValue root;
        JsonParser parser(std::string_view(stdoutText).substr(start));
        if (!parser.Parse(root)) continue;
        const auto title = StringMember(root, "title");
        const auto* formats = Member(root, "formats");
        if (title.empty() || !formats || formats->kind != JsonValue::Kind::Array) continue;

        YoutubeProbe probe;
        probe.videoId = VideoIdMember(root);
        probe.title = Utf8ToWide(title);
        probe.durationSeconds = NumberMember(root, "duration");
        for (const auto& format : formats->array) {
            if (format.kind != JsonValue::Kind::Object) continue;
            const auto id = StringMember(format, "format_id");
            const auto extension = StringMember(format, "ext");
            if (id.empty() || extension.empty()) continue;
            const auto videoCodec = StringMember(format, "vcodec");
            const auto audioCodec = StringMember(format, "acodec");
            const double heightValue = NumberMember(format, "height");
            const int height = heightValue > std::numeric_limits<int>::max()
                                   ? 0
                                   : static_cast<int>(heightValue);
            const double fps = NumberMember(format, "fps");
            const double abr = NumberMember(format, "abr");
            const auto filesize = SizeMember(format);
            if (videoCodec != "none" && audioCodec == "none" && height > 0) {
                probe.videoFormats.push_back(
                    YoutubeVideoFormat{Utf8ToWide(id), Utf8ToWide(extension), height, fps, filesize});
            }
            if (audioCodec != "none" && videoCodec == "none") {
                probe.audioFormats.push_back(
                    YoutubeAudioFormat{Utf8ToWide(id), Utf8ToWide(extension), abr, filesize});
            }
        }

        std::sort(probe.videoFormats.begin(), probe.videoFormats.end(), [](const auto& left,
                                                                            const auto& right) {
            if (left.height != right.height) return left.height > right.height;
            if (left.fps != right.fps) return left.fps > right.fps;
            if (left.filesize != right.filesize) return left.filesize > right.filesize;
            return left.formatId < right.formatId;
        });
        std::sort(probe.audioFormats.begin(), probe.audioFormats.end(), [](const auto& left,
                                                                            const auto& right) {
            if (left.abr != right.abr) return left.abr > right.abr;
            if (left.filesize != right.filesize) return left.filesize > right.filesize;
            return left.formatId < right.formatId;
        });
        if (probe.videoFormats.empty() && probe.audioFormats.empty()) continue;
        return probe;
    }
    return std::nullopt;
}

} // namespace rivan::youtube::detail

namespace rivan::youtube {

void YoutubeService::RunProbe(std::stop_token stop, std::uint64_t entryId) {
    YoutubeEntry target;
    bool foundEntry = false;
    {
        std::scoped_lock lock(mutex_);
        const auto found = std::find_if(
            state_.entries.begin(), state_.entries.end(),
            [entryId](const YoutubeEntry& entry) { return entry.id == entryId; });
        if (found != state_.entries.end()) {
            target = *found;
            foundEntry = true;
        } else {
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.status = L"Probe failed: entry no longer exists";
            ++state_.generation;
        }
    }
    if (!foundEntry) {
        Notify();
        return;
    }

    const auto ytDlp = LocateYtDlp();
    const std::wstring url = target.videoId.empty()
                                 ? target.webpageUrl
                                 : L"https://www.youtube.com/watch?v=" + target.videoId;
    std::string output;
    std::string error;
    DWORD exitCode = 1;
    std::optional<YoutubeProbe> result;
    if (ytDlp && !url.empty()) {
        const auto arguments = L"--ignore-config --no-cache-dir --dump-single-json --no-warnings --no-playlist " +
                               detail::QuoteArg(url);
        if (detail::RunProcessCapture(*ytDlp, arguments, stop, output, error, &exitCode) &&
            exitCode == 0 && !stop.stop_requested()) {
            result = detail::ParseProbeJson(output);
        }
    }

    {
        std::scoped_lock lock(mutex_);
        state_.busy = false;
        state_.job = YoutubeJobKind::Idle;
        if (stop.stop_requested()) {
            state_.status = L"Cancelled";
        } else if (!ytDlp) {
            state_.status = L"yt-dlp not installed — use Settings → Online";
        } else if (result) {
            state_.probe = std::move(result);
            state_.probeEntryId = entryId;
            for (auto& entry : state_.entries) {
                if (entry.id != entryId) continue;
                if (!state_.probe->videoId.empty()) entry.videoId = state_.probe->videoId;
                entry.title = state_.probe->title;
                entry.durationSeconds = state_.probe->durationSeconds;
            }
            state_.status = L"Probe ready";
        } else {
            const auto detailText = detail::TailWide(output.empty() ? error : output, 140);
            state_.status = detailText.empty() ? L"Probe failed"
                                                : L"Probe failed: " + detailText;
        }
        ++state_.generation;
    }
    Notify();
}

} // namespace rivan::youtube
