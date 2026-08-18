// Json.h
// Shared strict JSON DOM parser used by the lyrics, update, and online-media services.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rivan::core {

struct JsonValue final {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Object, Array };
    Kind kind{Kind::Null};
    bool boolean{};
    double number{};
    std::string string;
    std::vector<std::pair<std::string, JsonValue>> object;
    std::vector<JsonValue> array;
};

// Parses exactly one root value; the entire input must be consumed (trailing whitespace
// allowed). Returns nullopt on any structural or semantic violation, including malformed
// strings, unknown escapes, lone surrogates, out-of-range numbers, and nesting past the
// depth cap.
[[nodiscard]] std::optional<JsonValue> ParseJson(std::string_view text);

// First-wins lookup: returns the member with `name`, or nullptr when the value is not an
// object or the member is absent. Duplicate keys resolve to the first occurrence.
[[nodiscard]] const JsonValue* JsonMember(const JsonValue& object, std::string_view name) noexcept;

} // namespace rivan::core