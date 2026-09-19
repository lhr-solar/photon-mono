#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace photon::json {

class Value;

using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

class Value {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) : data_(nullptr) {}
    Value(bool v) : data_(v) {}
    Value(int v) : data_(static_cast<double>(v)) {}
    Value(unsigned v) : data_(static_cast<double>(v)) {}
    Value(std::int64_t v) : data_(static_cast<double>(v)) {}
    Value(std::uint64_t v) : data_(static_cast<double>(v)) {}
    Value(double v) : data_(v) {}
    Value(const char* v) : data_(std::string(v)) {}
    Value(std::string v) : data_(std::move(v)) {}
    Value(Array v) : data_(std::move(v)) {}
    Value(Object v) : data_(std::move(v)) {}

    Kind kind() const;
    bool isNull() const { return kind() == Kind::Null; }
    bool isBool() const { return kind() == Kind::Bool; }
    bool isNumber() const { return kind() == Kind::Number; }
    bool isString() const { return kind() == Kind::String; }
    bool isArray() const { return kind() == Kind::Array; }
    bool isObject() const { return kind() == Kind::Object; }

    bool asBool(bool fallback = false) const;
    double asNumber(double fallback = 0.0) const;
    std::int64_t asInt(std::int64_t fallback = 0) const;
    std::string asString(std::string_view fallback = {}) const;
    const Array& asArray() const;
    const Object& asObject() const;

    bool contains(std::string_view key) const;
    const Value& operator[](std::string_view key) const;
    const Value& at(std::size_t index) const;

    static const Value& null();

private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data_{nullptr};
};

struct ParseError {
    std::string message;
    std::size_t offset = 0;
};

std::optional<Value> parse(std::string_view text, ParseError* error = nullptr);
std::string stringify(const Value& value);

}  // namespace photon::json
