#include "Core/Serialization/JsonValue.h"
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace paramcad {

// ---------------------------------------------------------------------------
// JsonValue
// ---------------------------------------------------------------------------

JsonValue JsonValue::makeNull() {
    return JsonValue{};
}

JsonValue JsonValue::makeBool(bool value) {
    JsonValue v;
    v.type_ = JsonType::Bool;
    v.bool_ = value;
    return v;
}

JsonValue JsonValue::makeNumber(double value) {
    JsonValue v;
    v.type_ = JsonType::Number;
    v.number_ = value;
    return v;
}

JsonValue JsonValue::makeString(std::string value) {
    JsonValue v;
    v.type_ = JsonType::String;
    v.string_ = std::move(value);
    return v;
}

JsonValue JsonValue::makeArray() {
    JsonValue v;
    v.type_ = JsonType::Array;
    return v;
}

JsonValue JsonValue::makeObject() {
    JsonValue v;
    v.type_ = JsonType::Object;
    return v;
}

void JsonValue::add(JsonValue element) {
    if (type_ != JsonType::Array) return;
    items_.push_back(std::move(element));
}

void JsonValue::set(std::string key, JsonValue value) {
    if (type_ != JsonType::Object) return;
    for (auto& member : members_) {
        if (member.first == key) {
            member.second = std::move(value);
            return;
        }
    }
    members_.emplace_back(std::move(key), std::move(value));
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    if (type_ != JsonType::Object) return nullptr;
    for (const auto& member : members_)
        if (member.first == key) return &member.second;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

namespace {

void appendEscapedString(std::string& out, const std::string& text) {
    out.push_back('"');
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

void appendNumber(std::string& out, double value) {
    if (!std::isfinite(value)) { // not representable in JSON; schema never emits these
        out.push_back('0');
        return;
    }
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

void appendIndent(std::string& out, int depth) {
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
}

void writeValue(std::string& out, const JsonValue& value, int depth) {
    switch (value.type()) {
        case JsonType::Null:
            out += "null";
            break;
        case JsonType::Bool:
            out += value.asBool() ? "true" : "false";
            break;
        case JsonType::Number:
            appendNumber(out, value.asNumber());
            break;
        case JsonType::String:
            appendEscapedString(out, value.asString());
            break;
        case JsonType::Array: {
            const auto& items = value.items();
            if (items.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (std::size_t i = 0; i < items.size(); ++i) {
                out.push_back('\n');
                appendIndent(out, depth + 1);
                writeValue(out, items[i], depth + 1);
                if (i + 1 < items.size()) out.push_back(',');
            }
            out.push_back('\n');
            appendIndent(out, depth);
            out.push_back(']');
            break;
        }
        case JsonType::Object: {
            const auto& members = value.members();
            if (members.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (std::size_t i = 0; i < members.size(); ++i) {
                out.push_back('\n');
                appendIndent(out, depth + 1);
                appendEscapedString(out, members[i].first);
                out += ": ";
                writeValue(out, members[i].second, depth + 1);
                if (i + 1 < members.size()) out.push_back(',');
            }
            out.push_back('\n');
            appendIndent(out, depth);
            out.push_back('}');
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Parser (hand-rolled recursive descent)
// ---------------------------------------------------------------------------

class JsonParser {
public:
    JsonParser(std::string_view text, JsonParseError& err)
        : text_(text), err_(err) {}

    JsonValue parse() {
        skipWhitespace();
        JsonValue value = parseValue(0);
        if (!err_.ok) return JsonValue{};
        skipWhitespace();
        if (pos_ < text_.size()) {
            fail("unexpected trailing content after JSON value");
            return JsonValue{};
        }
        return value;
    }

private:
    static constexpr int kMaxDepth = 256;

    bool atEnd() const noexcept { return pos_ >= text_.size(); }
    char peek() const noexcept { return text_[pos_]; }

    void advance() noexcept {
        if (text_[pos_] == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        ++pos_;
    }

    void skipWhitespace() noexcept {
        while (!atEnd()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') advance();
            else break;
        }
    }

    void fail(std::string message) {
        if (!err_.ok) return; // keep the first error
        err_.ok = false;
        err_.line = line_;
        err_.column = column_;
        err_.message = std::move(message);
    }

    bool consume(char expected, const char* description) {
        if (atEnd() || peek() != expected) {
            fail(std::string("expected ") + description);
            return false;
        }
        advance();
        return true;
    }

    JsonValue parseValue(int depth) {
        if (depth > kMaxDepth) {
            fail("nesting too deep");
            return JsonValue{};
        }
        if (atEnd()) {
            fail("unexpected end of input, expected a JSON value");
            return JsonValue{};
        }
        switch (peek()) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': return parseString();
            case 't': return parseLiteral("true", JsonValue::makeBool(true));
            case 'f': return parseLiteral("false", JsonValue::makeBool(false));
            case 'n': return parseLiteral("null", JsonValue::makeNull());
            default: return parseNumber();
        }
    }

    JsonValue parseLiteral(std::string_view literal, JsonValue value) {
        if (text_.substr(pos_, literal.size()) != literal) {
            fail("invalid literal");
            return JsonValue{};
        }
        for (std::size_t i = 0; i < literal.size(); ++i) advance();
        return value;
    }

    JsonValue parseNumber() {
        const std::size_t start = pos_;
        if (!atEnd() && peek() == '-') advance();
        if (atEnd() || peek() < '0' || peek() > '9') {
            fail("invalid number");
            return JsonValue{};
        }
        while (!atEnd() && peek() >= '0' && peek() <= '9') advance();
        if (!atEnd() && peek() == '.') {
            advance();
            if (atEnd() || peek() < '0' || peek() > '9') {
                fail("expected digits after decimal point");
                return JsonValue{};
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9') advance();
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            advance();
            if (!atEnd() && (peek() == '+' || peek() == '-')) advance();
            if (atEnd() || peek() < '0' || peek() > '9') {
                fail("expected digits in exponent");
                return JsonValue{};
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9') advance();
        }
        double value = 0.0;
        const char* first = text_.data() + start;
        const char* last = text_.data() + pos_;
        const auto result = std::from_chars(first, last, value);
        if (result.ec != std::errc{} || result.ptr != last) {
            fail("invalid number");
            return JsonValue{};
        }
        return JsonValue::makeNumber(value);
    }

    // Appends a Unicode code point as UTF-8.
    static void appendUtf8(std::string& out, std::uint32_t codepoint) {
        if (codepoint <= 0x7F) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    bool parseHex4(std::uint32_t& value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            if (atEnd()) {
                fail("unterminated \\u escape");
                return false;
            }
            const char c = peek();
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<std::uint32_t>(c - 'A' + 10);
            else {
                fail("invalid hex digit in \\u escape");
                return false;
            }
            value = value * 16 + digit;
            advance();
        }
        return true;
    }

    JsonValue parseString() {
        std::string out;
        advance(); // opening quote
        while (true) {
            if (atEnd()) {
                fail("unterminated string");
                return JsonValue{};
            }
            const unsigned char c = static_cast<unsigned char>(peek());
            if (c == '"') {
                advance();
                return JsonValue::makeString(std::move(out));
            }
            if (c < 0x20) {
                fail("unescaped control character in string");
                return JsonValue{};
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                advance();
                continue;
            }
            advance(); // backslash
            if (atEnd()) {
                fail("unterminated escape sequence");
                return JsonValue{};
            }
            const char escape = peek();
            switch (escape) {
                case '"': out.push_back('"'); advance(); break;
                case '\\': out.push_back('\\'); advance(); break;
                case '/': out.push_back('/'); advance(); break;
                case 'b': out.push_back('\b'); advance(); break;
                case 'f': out.push_back('\f'); advance(); break;
                case 'n': out.push_back('\n'); advance(); break;
                case 'r': out.push_back('\r'); advance(); break;
                case 't': out.push_back('\t'); advance(); break;
                case 'u': {
                    advance();
                    std::uint32_t codepoint = 0;
                    if (!parseHex4(codepoint)) return JsonValue{};
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        // High surrogate: a \uXXXX low surrogate must follow.
                        if (atEnd() || peek() != '\\') {
                            fail("expected low surrogate after high surrogate");
                            return JsonValue{};
                        }
                        advance();
                        if (atEnd() || peek() != 'u') {
                            fail("expected low surrogate after high surrogate");
                            return JsonValue{};
                        }
                        advance();
                        std::uint32_t low = 0;
                        if (!parseHex4(low)) return JsonValue{};
                        if (low < 0xDC00 || low > 0xDFFF) {
                            fail("invalid low surrogate");
                            return JsonValue{};
                        }
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                        fail("unexpected low surrogate");
                        return JsonValue{};
                    }
                    appendUtf8(out, codepoint);
                    break;
                }
                default:
                    fail("invalid escape sequence");
                    return JsonValue{};
            }
        }
    }

    JsonValue parseArray(int depth) {
        JsonValue array = JsonValue::makeArray();
        advance(); // '['
        skipWhitespace();
        if (!atEnd() && peek() == ']') {
            advance();
            return array;
        }
        while (true) {
            skipWhitespace();
            JsonValue element = parseValue(depth + 1);
            if (!err_.ok) return JsonValue{};
            array.add(std::move(element));
            skipWhitespace();
            if (atEnd()) {
                fail("unterminated array, expected ',' or ']'");
                return JsonValue{};
            }
            if (peek() == ',') {
                advance();
                continue;
            }
            if (peek() == ']') {
                advance();
                return array;
            }
            fail("expected ',' or ']' in array");
            return JsonValue{};
        }
    }

    JsonValue parseObject(int depth) {
        JsonValue object = JsonValue::makeObject();
        advance(); // '{'
        skipWhitespace();
        if (!atEnd() && peek() == '}') {
            advance();
            return object;
        }
        while (true) {
            skipWhitespace();
            if (atEnd() || peek() != '"') {
                fail("expected string key in object");
                return JsonValue{};
            }
            JsonValue key = parseString();
            if (!err_.ok) return JsonValue{};
            skipWhitespace();
            if (!consume(':', "':' after object key")) return JsonValue{};
            skipWhitespace();
            JsonValue value = parseValue(depth + 1);
            if (!err_.ok) return JsonValue{};
            object.set(key.asString(), std::move(value));
            skipWhitespace();
            if (atEnd()) {
                fail("unterminated object, expected ',' or '}'");
                return JsonValue{};
            }
            if (peek() == ',') {
                advance();
                continue;
            }
            if (peek() == '}') {
                advance();
                return object;
            }
            fail("expected ',' or '}' in object");
            return JsonValue{};
        }
    }

    std::string_view text_;
    JsonParseError& err_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

} // namespace

JsonValue parseJson(std::string_view text, JsonParseError& err) {
    err = JsonParseError{};
    // Tolerate a UTF-8 BOM (EF BB BF) so files from BOM-writing editors load;
    // line/column positions are counted from the first byte after it.
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    JsonParser parser(text, err);
    JsonValue value = parser.parse();
    if (!err.ok) return JsonValue{};
    return value;
}

std::string writeJson(const JsonValue& value) {
    std::string out;
    writeValue(out, value, 0);
    return out;
}

} // namespace paramcad
