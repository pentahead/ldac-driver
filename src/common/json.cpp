#include "common/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bta {

namespace {
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    std::optional<Json> Run(std::string* err)
    {
        auto v = Value(0);
        Ws();
        if (v && pos_ != s_.size()) {
            Fail("trailing characters");
            v.reset();
        }
        if (!v && err) *err = err_ + " at offset " + std::to_string(pos_);
        return v;
    }

private:
    static constexpr int kMaxDepth = 64;

    void Ws()
    {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }
    void Fail(const char* m)
    {
        if (err_.empty()) err_ = m;
    }
    bool Lit(const char* lit)
    {
        size_t n = strlen(lit);
        if (s_.compare(pos_, n, lit) != 0) return false;
        pos_ += n;
        return true;
    }

    std::optional<Json> Value(int depth)
    {
        if (depth > kMaxDepth) {
            Fail("nesting too deep");
            return std::nullopt;
        }
        Ws();
        if (pos_ >= s_.size()) {
            Fail("unexpected end");
            return std::nullopt;
        }
        char c = s_[pos_];
        if (c == '{') return ObjectV(depth);
        if (c == '[') return ArrayV(depth);
        if (c == '"') {
            auto str = StringV();
            if (!str) return std::nullopt;
            return Json(std::move(*str));
        }
        if (Lit("true")) return Json(true);
        if (Lit("false")) return Json(false);
        if (Lit("null")) return Json(nullptr);
        return NumberV();
    }

    std::optional<Json> NumberV()
    {
        const char* start = s_.c_str() + pos_;
        char* end = nullptr;
        double d = strtod(start, &end);
        if (end == start) {
            Fail("invalid value");
            return std::nullopt;
        }
        pos_ += static_cast<size_t>(end - start);
        return Json(d);
    }

    static void AppendUtf8(std::string& out, uint32_t cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(uint32_t& out)
    {
        if (pos_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char h = s_[pos_++];
            out <<= 4;
            if (h >= '0' && h <= '9') out |= h - '0';
            else if (h >= 'a' && h <= 'f') out |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') out |= h - 'A' + 10;
            else return false;
        }
        return true;
    }

    std::optional<std::string> StringV()
    {
        ++pos_;  // opening quote
        std::string out;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) {
                Fail("control character in string");
                return std::nullopt;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) break;
            char e = s_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!Hex4(cp)) {
                    Fail("bad \\u escape");
                    return std::nullopt;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                    pos_ += 2;
                    uint32_t lo;
                    if (!Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) {
                        Fail("bad surrogate pair");
                        return std::nullopt;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default: Fail("bad escape"); return std::nullopt;
            }
        }
        Fail("unterminated string");
        return std::nullopt;
    }

    std::optional<Json> ArrayV(int depth)
    {
        ++pos_;
        Json::Array arr;
        Ws();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return Json(std::move(arr));
        }
        for (;;) {
            auto v = Value(depth + 1);
            if (!v) return std::nullopt;
            arr.push_back(std::move(*v));
            Ws();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return Json(std::move(arr));
            }
            Fail("expected , or ]");
            return std::nullopt;
        }
    }

    std::optional<Json> ObjectV(int depth)
    {
        ++pos_;
        Json::Object obj;
        Ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return Json(std::move(obj));
        }
        for (;;) {
            Ws();
            if (pos_ >= s_.size() || s_[pos_] != '"') {
                Fail("expected key");
                return std::nullopt;
            }
            auto key = StringV();
            if (!key) return std::nullopt;
            Ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                Fail("expected :");
                return std::nullopt;
            }
            ++pos_;
            auto v = Value(depth + 1);
            if (!v) return std::nullopt;
            obj[*key] = std::move(*v);
            Ws();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return Json(std::move(obj));
            }
            Fail("expected , or }");
            return std::nullopt;
        }
    }

    const std::string& s_;
    size_t pos_ = 0;
    std::string err_;
};

void EscapeTo(std::string& out, const std::string& s)
{
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}
}  // namespace

const Json::Array& Json::AsArray() const { return IsArray() ? std::get<Array>(v_) : kEmptyArray; }
const Json::Object& Json::AsObject() const { return IsObject() ? std::get<Object>(v_) : kEmptyObject; }

Json::Array& Json::MutArray()
{
    if (!IsArray()) v_ = Array{};
    return std::get<Array>(v_);
}

Json& Json::operator[](const std::string& key)
{
    if (!IsObject()) v_ = Object{};
    return std::get<Object>(v_)[key];
}

const Json* Json::Find(const std::string& key) const
{
    if (!IsObject()) return nullptr;
    const auto& o = std::get<Object>(v_);
    auto it = o.find(key);
    return it == o.end() ? nullptr : &it->second;
}

const Json& Json::Get(const std::string& key) const
{
    static const Json kNull;
    const Json* j = Find(key);
    return j ? *j : kNull;
}

std::string Json::Dump(bool pretty) const
{
    std::string out;
    DumpTo(out, pretty, 0);
    return out;
}

void Json::DumpTo(std::string& out, bool pretty, int indent) const
{
    auto nl = [&](int ind) {
        if (!pretty) return;
        out += '\n';
        out.append(static_cast<size_t>(ind) * 2, ' ');
    };
    if (IsNull()) {
        out += "null";
    } else if (IsBool()) {
        out += std::get<bool>(v_) ? "true" : "false";
    } else if (IsNumber()) {
        double d = std::get<double>(v_);
        char buf[32];
        if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9.0e15)
            snprintf(buf, sizeof buf, "%lld", static_cast<long long>(d));
        else if (std::isfinite(d))
            snprintf(buf, sizeof buf, "%.17g", d);
        else
            snprintf(buf, sizeof buf, "null");
        out += buf;
    } else if (IsString()) {
        EscapeTo(out, std::get<std::string>(v_));
    } else if (IsArray()) {
        const auto& a = std::get<Array>(v_);
        out += '[';
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) out += ',';
            nl(indent + 1);
            a[i].DumpTo(out, pretty, indent + 1);
        }
        if (!a.empty()) nl(indent);
        out += ']';
    } else {
        const auto& o = std::get<Object>(v_);
        out += '{';
        bool first = true;
        for (const auto& [k, v] : o) {
            if (!first) out += ',';
            first = false;
            nl(indent + 1);
            EscapeTo(out, k);
            out += pretty ? ": " : ":";
            v.DumpTo(out, pretty, indent + 1);
        }
        if (!o.empty()) nl(indent);
        out += '}';
    }
}

std::optional<Json> Json::Parse(const std::string& text, std::string* error)
{
    return Parser(text).Run(error);
}

}  // namespace bta
