// Small JSON DOM used for IPC messages and the settings file.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace bta {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() : v_(nullptr) {}
    Json(std::nullptr_t) : v_(nullptr) {}
    Json(bool b) : v_(b) {}
    Json(int i) : v_(static_cast<double>(i)) {}
    Json(int64_t i) : v_(static_cast<double>(i)) {}
    Json(uint32_t i) : v_(static_cast<double>(i)) {}
    Json(uint64_t i) : v_(static_cast<double>(i)) {}
    Json(double d) : v_(d) {}
    Json(const char* s) : v_(std::string(s)) {}
    Json(std::string s) : v_(std::move(s)) {}
    Json(Array a) : v_(std::move(a)) {}
    Json(Object o) : v_(std::move(o)) {}

    static Json MakeObject() { return Json(Object{}); }
    static Json MakeArray() { return Json(Array{}); }

    bool IsNull() const { return std::holds_alternative<std::nullptr_t>(v_); }
    bool IsBool() const { return std::holds_alternative<bool>(v_); }
    bool IsNumber() const { return std::holds_alternative<double>(v_); }
    bool IsString() const { return std::holds_alternative<std::string>(v_); }
    bool IsArray() const { return std::holds_alternative<Array>(v_); }
    bool IsObject() const { return std::holds_alternative<Object>(v_); }

    bool AsBool(bool def = false) const { return IsBool() ? std::get<bool>(v_) : def; }
    double AsNumber(double def = 0) const { return IsNumber() ? std::get<double>(v_) : def; }
    int64_t AsInt(int64_t def = 0) const { return IsNumber() ? static_cast<int64_t>(std::get<double>(v_)) : def; }
    std::string AsString(const std::string& def = {}) const { return IsString() ? std::get<std::string>(v_) : def; }
    const Array& AsArray() const;
    const Object& AsObject() const;
    Array& MutArray();

    // Object access. operator[] converts null to object.
    Json& operator[](const std::string& key);
    const Json* Find(const std::string& key) const;
    bool Has(const std::string& key) const { return Find(key) != nullptr; }
    // Reference to the member, or to a static null value if missing (safe in range-for over temporaries' members
    // only while the owning object lives).
    const Json& Get(const std::string& key) const;
    void Push(Json v) { MutArray().push_back(std::move(v)); }

    std::string Dump(bool pretty = false) const;
    static std::optional<Json> Parse(const std::string& text, std::string* error = nullptr);

private:
    void DumpTo(std::string& out, bool pretty, int indent) const;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v_;
};

}  // namespace bta
