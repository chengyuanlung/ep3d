#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace paramcad {

enum class JsonType { Null, Bool, Number, String, Array, Object };

// Minimal in-Core JSON document model with value semantics. Objects preserve
// key insertion order (stored as an ordered vector of key/value pairs; lookup
// is a linear find), which makes the writer output deterministic.
class JsonValue {
public:
    using Member = std::pair<std::string, JsonValue>;

    JsonValue() = default; // Null

    static JsonValue makeNull();
    static JsonValue makeBool(bool value);
    static JsonValue makeNumber(double value);
    static JsonValue makeString(std::string value);
    static JsonValue makeArray();
    static JsonValue makeObject();

    JsonType type() const noexcept { return type_; }
    bool isNull() const noexcept { return type_ == JsonType::Null; }

    // Accessors return the stored value; calling one on a mismatched type
    // returns the type's default (false / 0.0 / empty). Callers validate
    // type() first.
    bool asBool() const noexcept { return type_ == JsonType::Bool ? bool_ : false; }
    double asNumber() const noexcept { return type_ == JsonType::Number ? number_ : 0.0; }
    const std::string& asString() const noexcept { return string_; }

    // Array contents (empty for non-arrays).
    const std::vector<JsonValue>& items() const noexcept { return items_; }
    // Object members in insertion order (empty for non-objects).
    const std::vector<Member>& members() const noexcept { return members_; }

    // Appends an element (array helper).
    void add(JsonValue element);
    // Inserts or replaces a member, preserving first-insertion order (object
    // helper).
    void set(std::string key, JsonValue value);
    // Linear search; nullptr when the key is absent or this is not an object.
    const JsonValue* find(std::string_view key) const noexcept;

private:
    JsonType type_ = JsonType::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> items_;
    std::vector<Member> members_;
};

struct JsonParseError {
    bool ok = true;
    std::size_t line = 0;
    std::size_t column = 0;
    std::string message;
};

// Hand-rolled recursive-descent parser. A leading UTF-8 BOM (EF BB BF) is
// skipped; line/column positions count from the first byte after it. On
// failure err.ok is false, err carries a 1-based line/column and a message,
// and the returned value is Null. No exceptions escape.
JsonValue parseJson(std::string_view text, JsonParseError& err);

// Pretty writer: 2-space indent, object keys in insertion order, doubles via
// std::to_chars shortest-round-trip form, so writeJson(parseJson(writeJson(x)))
// is byte-identical to writeJson(x). Non-finite numbers are not representable
// in JSON and are written as 0 (the schema never produces them).
std::string writeJson(const JsonValue& value);

} // namespace paramcad
