#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "util/Result.h"

namespace vw::util {

// Minimal strict JSON value + parser/writer (decision D-06): no third-party
// dependency, used only for configuration/playlist state — never in frame paths.
class Json {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    using Array = std::vector<Json>;
    using Object = std::map<std::wstring, Json>;

    Json() = default;

    static Json null() { return Json(Value(nullptr)); }
    static Json boolean(bool b) { return Json(Value(b)); }
    static Json number(double n) { return Json(Value(n)); }
    static Json string(std::wstring s) { return Json(Value(std::move(s))); }
    static Json array(Array a) { return Json(Value(std::move(a))); }
    static Json object(Object o) { return Json(Value(std::move(o))); }

    Kind kind() const { return static_cast<Kind>(value_.index()); }
    bool isNull() const { return kind() == Kind::Null; }
    bool isBool() const { return kind() == Kind::Bool; }
    bool isNumber() const { return kind() == Kind::Number; }
    bool isString() const { return kind() == Kind::String; }
    bool isArray() const { return kind() == Kind::Array; }
    bool isObject() const { return kind() == Kind::Object; }

    bool asBool(bool def = false) const;
    double asNumber(double def = 0.0) const;
    int64_t asInt(int64_t def = 0) const;
    const std::wstring& asString(const std::wstring& def = {}) const;
    const Array& asArray() const;   // empty when not an array
    const Object& asObject() const; // empty when not an object

    // Object member access; returns a Null value when missing.
    const Json& get(const std::wstring& key) const;

    // Strict parse: trailing characters / malformed input produce an error.
    static Result<Json> parse(const std::wstring& text);
    std::wstring serialize() const;

private:
    using Value = std::variant<std::nullptr_t, bool, double, std::wstring, Array, Object>;
    explicit Json(Value v) : value_(std::move(v)) {}

    Value value_{nullptr};
};

} // namespace vw::util
