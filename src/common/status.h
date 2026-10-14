// Error/status type shared by every user-mode module.
#pragma once

#include <string>
#include <utility>

namespace bta {

enum class Err {
    Ok = 0,
    InvalidArgument,
    NotSupported,
    NotAvailable,      // dependency (driver, device, service) missing
    Timeout,
    Rejected,          // remote rejected a request
    ProtocolError,     // malformed or unexpected remote data
    Disconnected,
    BufferOverflow,
    EncoderError,
    IoError,
    StateError,
    Internal,
};

const char* ErrName(Err e);

class Status {
public:
    Status() = default;
    Status(Err code, std::string msg) : code_(code), msg_(std::move(msg)) {}
    static Status Ok() { return {}; }

    bool ok() const { return code_ == Err::Ok; }
    explicit operator bool() const { return ok(); }
    Err code() const { return code_; }
    const std::string& message() const { return msg_; }
    std::string ToString() const;

private:
    Err code_ = Err::Ok;
    std::string msg_;
};

// Minimal expected<T, Status>.
template <typename T>
class Result {
public:
    Result(T v) : value_(std::move(v)), has_(true) {}
    Result(Status s) : status_(std::move(s)), has_(false) {}
    bool ok() const { return has_; }
    explicit operator bool() const { return has_; }
    const Status& status() const { return status_; }
    T& value() { return value_; }
    const T& value() const { return value_; }
    T* operator->() { return &value_; }
    T& operator*() { return value_; }

private:
    T value_{};
    Status status_;
    bool has_;
};

}  // namespace bta
