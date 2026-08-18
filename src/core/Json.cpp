// Json.cpp
#include "Json.h"

#include <charconv>
#include <cmath>
#include <cstddef>

namespace rivan::core {
namespace {
constexpr std::size_t kMaximumDepth = 64U;

void SkipWhitespace(std::string_view text, std::size_t& position) noexcept {
    while (position < text.size()) {
        const char value = text[position];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n') return;
        ++position;
    }
}

void AppendUtf8(std::string& output, const std::uint32_t codePoint) {
    if (codePoint <= 0x7fU) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else if (codePoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    }
}

[[nodiscard]] bool ParseHex4(std::string_view text, std::size_t& position,
                             std::uint32_t& codeUnit) {
    if (position + 4U > text.size()) return false;
    codeUnit = 0;
    for (int digit = 0; digit < 4; ++digit) {
        const char value = text[position++];
        codeUnit <<= 4U;
        if (value >= '0' && value <= '9') codeUnit += static_cast<std::uint32_t>(value - '0');
        else if (value >= 'a' && value <= 'f') codeUnit += static_cast<std::uint32_t>(value - 'a' + 10);
        else if (value >= 'A' && value <= 'F') codeUnit += static_cast<std::uint32_t>(value - 'A' + 10);
        else return false;
    }
    return true;
}

[[nodiscard]] bool ParseString(std::string_view text, std::size_t& position, std::string& output) {
    if (position >= text.size() || text[position] != '"') return false;
    ++position;
    output.clear();
    while (position < text.size()) {
        const unsigned char value = static_cast<unsigned char>(text[position++]);
        if (value == '"') return true;
        if (value < 0x20U) return false;
        if (value != '\\') {
            output.push_back(static_cast<char>(value));
            continue;
        }
        if (position >= text.size()) return false;
        switch (text[position++]) {
        case '"': output.push_back('"'); break;
        case '\\': output.push_back('\\'); break;
        case '/': output.push_back('/'); break;
        case 'b': output.push_back('\b'); break;
        case 'f': output.push_back('\f'); break;
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        case 'u': {
            std::uint32_t codePoint{};
            if (!ParseHex4(text, position, codePoint)) return false;
            if (codePoint >= 0xd800U && codePoint <= 0xdbffU) {
                // A high surrogate must pair with a following low-surrogate escape; any
                // other follow-up is a lone surrogate and rejected.
                if (position + 2U > text.size() || text[position] != '\\' ||
                    text[position + 1U] != 'u') {
                    return false;
                }
                position += 2U;
                std::uint32_t lowSurrogate{};
                if (!ParseHex4(text, position, lowSurrogate) ||
                    lowSurrogate < 0xdc00U || lowSurrogate > 0xdfffU) {
                    return false;
                }
                codePoint = 0x10000U + ((codePoint - 0xd800U) << 10U) +
                            (lowSurrogate - 0xdc00U);
            } else if (codePoint >= 0xdc00U && codePoint <= 0xdfffU) {
                return false;
            }
            AppendUtf8(output, codePoint);
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

[[nodiscard]] bool ParseNumber(std::string_view text, std::size_t& position, double& number) {
    const std::size_t start = position;
    if (position < text.size() && text[position] == '-') ++position;
    if (position >= text.size()) return false;
    if (text[position] == '0') {
        ++position;
        // JSON forbids leading zeroes, so "01" and "-01" are invalid.
        if (position < text.size() && text[position] >= '0' && text[position] <= '9') return false;
    } else {
        if (text[position] < '1' || text[position] > '9') return false;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            ++position;
        }
    }
    if (position < text.size() && text[position] == '.') {
        ++position;
        const std::size_t fractionStart = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            ++position;
        }
        if (position == fractionStart) return false;
    }
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            ++position;
        }
        const std::size_t exponentStart = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            ++position;
        }
        if (position == exponentStart) return false;
    }
    if (position == start) return false;
    const auto numberText = text.substr(start, position - start);
    const auto [end, error] = std::from_chars(
        numberText.data(), numberText.data() + numberText.size(), number);
    // Out-of-range values (e.g. 1e999) surface as from_chars errors; the isfinite check
    // additionally rejects any non-finite result a toolchain might produce.
    return error == std::errc{} && end == numberText.data() + numberText.size() &&
           std::isfinite(number);
}

[[nodiscard]] bool ParseLiteral(std::string_view text, std::size_t& position,
                                const std::string_view literal, JsonValue::Kind kind,
                                const bool boolean, JsonValue& value) {
    if (text.substr(position, literal.size()) != literal) return false;
    position += literal.size();
    value.kind = kind;
    value.boolean = boolean;
    return true;
}

[[nodiscard]] bool ParseValue(std::string_view text, std::size_t& position,
                              std::size_t depth, JsonValue& value);
[[nodiscard]] bool ParseObject(std::string_view text, std::size_t& position,
                               std::size_t depth, JsonValue& value);
[[nodiscard]] bool ParseArray(std::string_view text, std::size_t& position,
                              std::size_t depth, JsonValue& value);

[[nodiscard]] bool ParseObject(std::string_view text, std::size_t& position,
                               const std::size_t depth, JsonValue& value) {
    value.kind = JsonValue::Kind::Object;
    value.object.clear();
    ++position; // consume '{'
    SkipWhitespace(text, position);
    if (position < text.size() && text[position] == '}') {
        ++position;
        return true;
    }
    for (;;) {
        SkipWhitespace(text, position);
        std::string key;
        if (!ParseString(text, position, key)) return false;
        SkipWhitespace(text, position);
        if (position >= text.size() || text[position] != ':') return false;
        ++position;
        JsonValue child;
        if (!ParseValue(text, position, depth, child)) return false;
        value.object.emplace_back(std::move(key), std::move(child));
        SkipWhitespace(text, position);
        if (position >= text.size()) return false;
        if (text[position] == '}') {
            ++position;
            return true;
        }
        if (text[position] != ',') return false;
        ++position;
    }
}

[[nodiscard]] bool ParseArray(std::string_view text, std::size_t& position,
                              const std::size_t depth, JsonValue& value) {
    value.kind = JsonValue::Kind::Array;
    value.array.clear();
    ++position; // consume '['
    SkipWhitespace(text, position);
    if (position < text.size() && text[position] == ']') {
        ++position;
        return true;
    }
    for (;;) {
        JsonValue child;
        if (!ParseValue(text, position, depth, child)) return false;
        value.array.push_back(std::move(child));
        SkipWhitespace(text, position);
        if (position >= text.size()) return false;
        if (text[position] == ']') {
            ++position;
            return true;
        }
        if (text[position] != ',') return false;
        ++position;
    }
}

[[nodiscard]] bool ParseValue(std::string_view text, std::size_t& position,
                              const std::size_t depth, JsonValue& value) {
    if (depth > kMaximumDepth) return false;
    SkipWhitespace(text, position);
    if (position >= text.size()) return false;
    switch (text[position]) {
    case '{': return ParseObject(text, position, depth + 1U, value);
    case '[': return ParseArray(text, position, depth + 1U, value);
    case '"':
        value.kind = JsonValue::Kind::String;
        return ParseString(text, position, value.string);
    case 't': return ParseLiteral(text, position, "true", JsonValue::Kind::Bool, true, value);
    case 'f': return ParseLiteral(text, position, "false", JsonValue::Kind::Bool, false, value);
    case 'n': return ParseLiteral(text, position, "null", JsonValue::Kind::Null, false, value);
    default: {
        double number{};
        if (!ParseNumber(text, position, number)) return false;
        value.kind = JsonValue::Kind::Number;
        value.number = number;
        return true;
    }
    }
}

} // namespace

std::optional<JsonValue> ParseJson(std::string_view text) {
    std::size_t position = 0;
    JsonValue root;
    SkipWhitespace(text, position);
    if (!ParseValue(text, position, 0U, root)) return std::nullopt;
    SkipWhitespace(text, position);
    if (position != text.size()) return std::nullopt;
    return root;
}

const JsonValue* JsonMember(const JsonValue& object, std::string_view name) noexcept {
    if (object.kind != JsonValue::Kind::Object) return nullptr;
    for (const auto& [key, member] : object.object) {
        if (key == name) return &member;
    }
    return nullptr;
}

} // namespace rivan::core