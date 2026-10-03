#include "photon/json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace photon::json {
namespace {

const Array kEmptyArray{};
const Object kEmptyObject{};
const Value kNullValue{};

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::optional<Value> parse(ParseError* error) {
        skipWs();
        auto value = parseValue();
        if (!ok_) {
            if (error) {
                *error = {error_, pos_};
            }
            return std::nullopt;
        }
        skipWs();
        if (pos_ != text_.size()) {
            fail("trailing data after JSON value");
            if (error) {
                *error = {error_, pos_};
            }
            return std::nullopt;
        }
        return value;
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
    bool ok_ = true;
    std::string error_;

    void fail(std::string message) {
        if (ok_) {
            ok_ = false;
            error_ = std::move(message);
        }
    }

    void skipWs() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool consume(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    std::optional<Value> parseValue() {
        skipWs();
        if (pos_ >= text_.size()) {
            fail("unexpected end of input");
            return std::nullopt;
        }
        const char c = text_[pos_];
        if (c == 'n') {
            return parseLiteral("null", Value(nullptr));
        }
        if (c == 't') {
            return parseLiteral("true", Value(true));
        }
        if (c == 'f') {
            return parseLiteral("false", Value(false));
        }
        if (c == '"') {
            return parseString();
        }
        if (c == '[') {
            return parseArray();
        }
        if (c == '{') {
            return parseObject();
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parseNumber();
        }
        fail("unexpected character");
        return std::nullopt;
    }

    std::optional<Value> parseLiteral(std::string_view lit, Value value) {
        if (text_.substr(pos_, lit.size()) != lit) {
            fail("invalid literal");
            return std::nullopt;
        }
        pos_ += lit.size();
        return value;
    }

    std::optional<Value> parseNumber() {
        const std::size_t start = pos_;
        if (consume('-') && (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_])))) {
            fail("invalid number");
            return std::nullopt;
        }
        if (consume('0')) {
            // leading zero must not be followed by more digits
        } else {
            if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                fail("invalid number");
                return std::nullopt;
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }
        if (consume('.')) {
            if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                fail("invalid number");
                return std::nullopt;
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                fail("invalid number");
                return std::nullopt;
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }
        const std::string slice(text_.substr(start, pos_ - start));
        char* end = nullptr;
        const double value = std::strtod(slice.c_str(), &end);
        if (end != slice.c_str() + slice.size()) {
            fail("invalid number");
            return std::nullopt;
        }
        return Value(value);
    }

    std::optional<Value> parseString() {
        if (!consume('"')) {
            fail("expected string");
            return std::nullopt;
        }
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return Value(std::move(out));
            }
            if (c == '\\') {
                if (pos_ >= text_.size()) {
                    fail("unterminated escape");
                    return std::nullopt;
                }
                const char e = text_[pos_++];
                switch (e) {
                    case '"':
                    case '\\':
                    case '/':
                        out.push_back(e);
                        break;
                    case 'b':
                        out.push_back('\b');
                        break;
                    case 'f':
                        out.push_back('\f');
                        break;
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    case 'u': {
                        if (pos_ + 4 > text_.size()) {
                            fail("invalid unicode escape");
                            return std::nullopt;
                        }
                        unsigned code = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = text_[pos_++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') {
                                code |= static_cast<unsigned>(h - '0');
                            } else if (h >= 'a' && h <= 'f') {
                                code |= static_cast<unsigned>(h - 'a' + 10);
                            } else if (h >= 'A' && h <= 'F') {
                                code |= static_cast<unsigned>(h - 'A' + 10);
                            } else {
                                fail("invalid unicode escape");
                                return std::nullopt;
                            }
                        }
                        appendUtf8(out, code);
                        break;
                    }
                    default:
                        fail("invalid escape");
                        return std::nullopt;
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("unescaped control character");
                return std::nullopt;
            }
            out.push_back(c);
        }
        fail("unterminated string");
        return std::nullopt;
    }

    static void appendUtf8(std::string& out, unsigned code) {
        if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    std::optional<Value> parseArray() {
        if (!consume('[')) {
            fail("expected array");
            return std::nullopt;
        }
        Array arr;
        skipWs();
        if (consume(']')) {
            return Value(std::move(arr));
        }
        while (ok_) {
            auto item = parseValue();
            if (!item) {
                return std::nullopt;
            }
            arr.push_back(std::move(*item));
            skipWs();
            if (consume(']')) {
                return Value(std::move(arr));
            }
            if (!consume(',')) {
                fail("expected comma in array");
                return std::nullopt;
            }
            skipWs();
        }
        return std::nullopt;
    }

    std::optional<Value> parseObject() {
        if (!consume('{')) {
            fail("expected object");
            return std::nullopt;
        }
        Object obj;
        skipWs();
        if (consume('}')) {
            return Value(std::move(obj));
        }
        while (ok_) {
            auto key = parseString();
            if (!key) {
                return std::nullopt;
            }
            skipWs();
            if (!consume(':')) {
                fail("expected colon");
                return std::nullopt;
            }
            auto val = parseValue();
            if (!val) {
                return std::nullopt;
            }
            obj.insert_or_assign(key->asString(), std::move(*val));
            skipWs();
            if (consume('}')) {
                return Value(std::move(obj));
            }
            if (!consume(',')) {
                fail("expected comma in object");
                return std::nullopt;
            }
            skipWs();
        }
        return std::nullopt;
    }
};

void stringifyInto(std::string& out, const Value& value) {
    switch (value.kind()) {
        case Value::Kind::Null:
            out += "null";
            break;
        case Value::Kind::Bool:
            out += value.asBool() ? "true" : "false";
            break;
        case Value::Kind::Number: {
            const double n = value.asNumber();
            if (std::isfinite(n) && std::floor(n) == n && n >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
                n <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
                out += std::to_string(static_cast<std::int64_t>(n));
            } else {
                char buf[64];
                const int written = std::snprintf(buf, sizeof(buf), "%.17g", n);
                if (written > 0) {
                    out.append(buf, static_cast<std::size_t>(written));
                } else {
                    out += "0";
                }
            }
            break;
        }
        case Value::Kind::String: {
            out.push_back('"');
            for (unsigned char c : value.asString()) {
                switch (c) {
                    case '"':
                        out += "\\\"";
                        break;
                    case '\\':
                        out += "\\\\";
                        break;
                    case '\b':
                        out += "\\b";
                        break;
                    case '\f':
                        out += "\\f";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    default:
                        if (c < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out += buf;
                        } else {
                            out.push_back(static_cast<char>(c));
                        }
                        break;
                }
            }
            out.push_back('"');
            break;
        }
        case Value::Kind::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto& item : value.asArray()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                stringifyInto(out, item);
            }
            out.push_back(']');
            break;
        }
        case Value::Kind::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [k, v] : value.asObject()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                stringifyInto(out, Value(k));
                out.push_back(':');
                stringifyInto(out, v);
            }
            out.push_back('}');
            break;
        }
    }
}

}  // namespace

Value::Kind Value::kind() const {
    return static_cast<Kind>(data_.index());
}

bool Value::asBool(bool fallback) const {
    if (const auto* v = std::get_if<bool>(&data_)) {
        return *v;
    }
    return fallback;
}

double Value::asNumber(double fallback) const {
    if (const auto* v = std::get_if<double>(&data_)) {
        return *v;
    }
    return fallback;
}

std::int64_t Value::asInt(std::int64_t fallback) const {
    if (const auto* v = std::get_if<double>(&data_)) {
        return static_cast<std::int64_t>(*v);
    }
    return fallback;
}

std::string Value::asString(std::string_view fallback) const {
    if (const auto* v = std::get_if<std::string>(&data_)) {
        return *v;
    }
    return std::string(fallback);
}

const Array& Value::asArray() const {
    if (const auto* v = std::get_if<Array>(&data_)) {
        return *v;
    }
    return kEmptyArray;
}

const Object& Value::asObject() const {
    if (const auto* v = std::get_if<Object>(&data_)) {
        return *v;
    }
    return kEmptyObject;
}

bool Value::contains(std::string_view key) const {
    const auto& obj = asObject();
    return obj.find(std::string(key)) != obj.end();
}

const Value& Value::operator[](std::string_view key) const {
    const auto& obj = asObject();
    const auto it = obj.find(std::string(key));
    if (it == obj.end()) {
        return kNullValue;
    }
    return it->second;
}

const Value& Value::at(std::size_t index) const {
    const auto& arr = asArray();
    if (index >= arr.size()) {
        return kNullValue;
    }
    return arr[index];
}

const Value& Value::null() {
    return kNullValue;
}

std::optional<Value> parse(std::string_view text, ParseError* error) {
    return Parser(text).parse(error);
}

std::string stringify(const Value& value) {
    std::string out;
    stringifyInto(out, value);
    return out;
}

}  // namespace photon::json
