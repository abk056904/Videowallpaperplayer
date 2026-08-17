#include "util/json.h"

#include <cmath>
#include <cwchar>
#include <format>

namespace vw::util {

namespace {

class Parser {
public:
    explicit Parser(const std::wstring& text) : s_(text) {}

    Result<Json> run() {
        skipWs();
        auto v = parseValue();
        if (!v) return v;
        skipWs();
        if (pos_ != s_.size()) return error(L"trailing characters after value");
        return v;
    }

private:
    const std::wstring& s_;
    size_t pos_ = 0;

    Result<Json> error(const wchar_t* msg) const {
        return std::unexpected(std::format(L"JSON parse error at offset {}: {}", pos_, msg));
    }
    Result<std::wstring> errorStr(const wchar_t* msg) const {
        return std::unexpected(std::format(L"JSON parse error at offset {}: {}", pos_, msg));
    }

    void skipWs() {
        while (pos_ < s_.size() && (s_[pos_] == L' ' || s_[pos_] == L'\t' ||
                                   s_[pos_] == L'\n' || s_[pos_] == L'\r')) {
            ++pos_;
        }
    }

    bool consume(wchar_t c) {
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    Result<Json> parseValue() {
        if (pos_ >= s_.size()) return error(L"unexpected end of input");
        const wchar_t c = s_[pos_];
        if (c == L'{') return parseObject();
        if (c == L'[') return parseArray();
        if (c == L'"') return parseString();
        if (c == L't') return parseLiteral(L"true", Json::boolean(true));
        if (c == L'f') return parseLiteral(L"false", Json::boolean(false));
        if (c == L'n') return parseLiteral(L"null", Json::null());
        if (c == L'-' || (c >= L'0' && c <= L'9')) return parseNumber();
        return error(L"unexpected character");
    }

    Result<Json> parseLiteral(const wchar_t* lit, Json value) {
        const size_t n = wcslen(lit);
        if (s_.compare(pos_, n, lit) != 0) return error(L"invalid literal");
        pos_ += n;
        return value;
    }

    Result<std::wstring> parseStringRaw() {
        ++pos_; // consume opening quote
        std::wstring out;
        while (pos_ < s_.size()) {
            const wchar_t c = s_[pos_++];
            if (c == L'"') return out;
            if (c == L'\\') {
                if (pos_ >= s_.size()) return errorStr(L"bad escape");
                const wchar_t e = s_[pos_++];
                switch (e) {
                    case L'"': out += L'"'; break;
                    case L'\\': out += L'\\'; break;
                    case L'/': out += L'/'; break;
                    case L'b': out += L'\b'; break;
                    case L'f': out += L'\f'; break;
                    case L'n': out += L'\n'; break;
                    case L'r': out += L'\r'; break;
                    case L't': out += L'\t'; break;
                    case L'u': {
                        if (pos_ + 4 > s_.size()) return errorStr(L"bad \\u escape");
                        unsigned code = 0;
                        for (int i = 0; i < 4; ++i) {
                            const wchar_t h = s_[pos_++];
                            code <<= 4;
                            if (h >= L'0' && h <= L'9') code |= static_cast<unsigned>(h - L'0');
                            else if (h >= L'a' && h <= L'f') code |= static_cast<unsigned>(h - L'a' + 10);
                            else if (h >= L'A' && h <= L'F') code |= static_cast<unsigned>(h - L'A' + 10);
                            else return errorStr(L"bad \\u escape digit");
                        }
                        out += static_cast<wchar_t>(code);
                        break;
                    }
                    default: return errorStr(L"unknown escape");
                }
            } else if (static_cast<unsigned>(c) < 0x20) {
                return errorStr(L"control character in string");
            } else {
                out += c;
            }
        }
        return errorStr(L"unterminated string");
    }

    Result<Json> parseString() {
        auto s = parseStringRaw();
        if (!s) return std::unexpected(s.error());
        return Json::string(std::move(*s));
    }

    Result<Json> parseNumber() {
        const size_t start = pos_;
        consume(L'-');
        if (consume(L'0')) {
            // integer part done (no leading zeros)
        } else if (pos_ < s_.size() && s_[pos_] >= L'1' && s_[pos_] <= L'9') {
            while (pos_ < s_.size() && s_[pos_] >= L'0' && s_[pos_] <= L'9') ++pos_;
        } else {
            return error(L"bad number");
        }
        if (consume(L'.')) {
            if (pos_ >= s_.size() || s_[pos_] < L'0' || s_[pos_] > L'9') return error(L"bad fraction");
            while (pos_ < s_.size() && s_[pos_] >= L'0' && s_[pos_] <= L'9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == L'e' || s_[pos_] == L'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == L'+' || s_[pos_] == L'-')) ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < L'0' || s_[pos_] > L'9') return error(L"bad exponent");
            while (pos_ < s_.size() && s_[pos_] >= L'0' && s_[pos_] <= L'9') ++pos_;
        }
        const std::wstring tok = s_.substr(start, pos_ - start);
        wchar_t* end = nullptr;
        const double d = wcstod(tok.c_str(), &end);
        if (end == nullptr || *end != L'\0' || !std::isfinite(d)) return error(L"bad number value");
        return Json::number(d);
    }

    Result<Json> parseObject() {
        ++pos_; // consume '{'
        Json::Object obj;
        skipWs();
        if (consume(L'}')) return Json::object(std::move(obj));
        for (;;) {
            skipWs();
            if (pos_ >= s_.size()) return error(L"unterminated object");
            if (s_[pos_] != L'"') return error(L"expected object key");
            auto key = parseStringRaw();
            if (!key) return std::unexpected(key.error());
            skipWs();
            if (!consume(L':')) return error(L"expected ':' after key");
            skipWs();
            auto val = parseValue();
            if (!val) return std::unexpected(val.error());
            obj.emplace(std::move(*key), std::move(*val));
            skipWs();
            if (consume(L'}')) return Json::object(std::move(obj));
            if (!consume(L',')) return error(L"expected ',' or '}'");
        }
    }

    Result<Json> parseArray() {
        ++pos_; // consume '['
        Json::Array arr;
        skipWs();
        if (consume(L']')) return Json::array(std::move(arr));
        for (;;) {
            skipWs();
            auto val = parseValue();
            if (!val) return std::unexpected(val.error());
            arr.push_back(std::move(*val));
            skipWs();
            if (consume(L']')) return Json::array(std::move(arr));
            if (!consume(L',')) return error(L"expected ',' or ']'");
        }
    }
};

} // namespace

bool Json::asBool(bool def) const {
    if (const auto* b = std::get_if<bool>(&value_)) return *b;
    return def;
}

double Json::asNumber(double def) const {
    if (const auto* n = std::get_if<double>(&value_)) return *n;
    return def;
}

int64_t Json::asInt(int64_t def) const {
    if (const auto* n = std::get_if<double>(&value_)) return static_cast<int64_t>(*n);
    return def;
}

const std::wstring& Json::asString(const std::wstring& def) const {
    if (const auto* s = std::get_if<std::wstring>(&value_)) return *s;
    return def;
}

const Json::Array& Json::asArray() const {
    static const Array empty;
    if (const auto* a = std::get_if<Array>(&value_)) return *a;
    return empty;
}

const Json::Object& Json::asObject() const {
    static const Object empty;
    if (const auto* o = std::get_if<Object>(&value_)) return *o;
    return empty;
}

const Json& Json::get(const std::wstring& key) const {
    static const Json missing;
    const auto& obj = asObject();
    const auto it = obj.find(key);
    return it != obj.end() ? it->second : missing;
}

Result<Json> Json::parse(const std::wstring& text) {
    return Parser(text).run();
}

std::wstring Json::serialize() const {
    switch (kind()) {
        case Kind::Null:
            return L"null";
        case Kind::Bool:
            return asBool() ? L"true" : L"false";
        case Kind::Number:
            return std::format(L"{}", asNumber());
        case Kind::String: {
            std::wstring out = L"\"";
            for (const wchar_t c : asString()) {
                switch (c) {
                    case L'"': out += L"\\\""; break;
                    case L'\\': out += L"\\\\"; break;
                    case L'\b': out += L"\\b"; break;
                    case L'\f': out += L"\\f"; break;
                    case L'\n': out += L"\\n"; break;
                    case L'\r': out += L"\\r"; break;
                    case L'\t': out += L"\\t"; break;
                    default:
                        if (static_cast<unsigned>(c) < 0x20) {
                            out += std::format(L"\\u{:04x}", static_cast<unsigned>(c));
                        } else {
                            out += c;
                        }
                }
            }
            out += L'"';
            return out;
        }
        case Kind::Array: {
            std::wstring out = L"[";
            bool first = true;
            for (const auto& e : asArray()) {
                if (!first) out += L", ";
                first = false;
                out += e.serialize();
            }
            out += L']';
            return out;
        }
        case Kind::Object: {
            std::wstring out = L"{";
            bool first = true;
            for (const auto& [k, v] : asObject()) {
                if (!first) out += L", ";
                first = false;
                out += Json::string(k).serialize();
                out += L": ";
                out += v.serialize();
            }
            out += L'}';
            return out;
        }
    }
    return L"null";
}

} // namespace vw::util
