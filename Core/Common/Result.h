#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace CarPlayStation::Core::Common {

// Unified error code taxonomy for the whole project (no raw exceptions across layer boundaries)
enum class StatusCode : uint32_t {
    Ok = 0,
    UnknownError,
    InvalidArgument,
    NotFound,
    NotSupported,
    Timeout,
    NetworkError,
    TransportDisconnected,
    AuthenticationFailed,
    ProtocolError,
    MediaPipelineError,
    HardwareError,
    ResourceExhausted
};

[[nodiscard]] constexpr std::string_view StatusCodeToString(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::Ok: return "Ok";
        case StatusCode::UnknownError: return "UnknownError";
        case StatusCode::InvalidArgument: return "InvalidArgument";
        case StatusCode::NotFound: return "NotFound";
        case StatusCode::NotSupported: return "NotSupported";
        case StatusCode::Timeout: return "Timeout";
        case StatusCode::NetworkError: return "NetworkError";
        case StatusCode::TransportDisconnected: return "TransportDisconnected";
        case StatusCode::AuthenticationFailed: return "AuthenticationFailed";
        case StatusCode::ProtocolError: return "ProtocolError";
        case StatusCode::MediaPipelineError: return "MediaPipelineError";
        case StatusCode::HardwareError: return "HardwareError";
        case StatusCode::ResourceExhausted: return "ResourceExhausted";
    }
    return "Unknown";
}

// Lightweight status object returned by every fallible operation.
class Status {
public:
    constexpr Status() noexcept = default;

    [[nodiscard]] static constexpr Status Ok() noexcept { return Status(); }

    [[nodiscard]] static Status Error(StatusCode code, std::string message = {}) {
        return Status(code, std::move(message));
    }

    [[nodiscard]] bool IsOk() const noexcept { return code_ == StatusCode::Ok; }
    [[nodiscard]] bool IsError() const noexcept { return !IsOk(); }
    [[nodiscard]] StatusCode Code() const noexcept { return code_; }
    [[nodiscard]] const std::string& Message() const noexcept { return message_; }

private:
    constexpr Status(StatusCode code, std::string&& message)
        : code_(code), message_(std::move(message)) {}

    StatusCode code_{StatusCode::Ok};
    std::string message_{};
};

// Single-value or error wrapper (modern C++ alternative to out-parameters).
// T must be default constructible.
template <typename T>
class Result {
public:
    // Value constructor (implicit so `Result<int> r = 42;` works)
    Result(T value) : ok_(true), value_(std::move(value)), status_() {}

    // Error constructor from Status (implicit so `Result<T> r = Status::Error(...);` works)
    Result(Status status) : ok_(status.IsOk()), value_(), status_(status) {}

    [[nodiscard]] bool IsOk() const noexcept { return ok_; }
    [[nodiscard]] bool IsError() const noexcept { return !ok_; }

    [[nodiscard]] T& Value() & { return value_; }
    [[nodiscard]] const T& Value() const& { return value_; }
    [[nodiscard]] T&& Value() && { return std::move(value_); }

    [[nodiscard]] T* operator->() noexcept { return &value_; }
    [[nodiscard]] const T* operator->() const noexcept { return &value_; }
    [[nodiscard]] T& operator*() & noexcept { return value_; }
    [[nodiscard]] const T& operator*() const& noexcept { return value_; }

    // Valid only when IsError() is true
    [[nodiscard]] Status Error() const noexcept { return status_; }

private:
    bool ok_{false};
    T value_{};
    Status status_{};
};

} // namespace CarPlayStation::Core::Common
