#pragma once

#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <utility>
#include <optional>
#include <stdexcept>

namespace imagine {

enum class StatusCode {
    Ok = 0,
    NotFound,
    AlreadyExists,
    InvalidArgument,
    IoError,
    DatabaseError,
    ParseError,
    InternalError
};

class Status {
public:
    Status() : code_(StatusCode::Ok) {}
    Status(StatusCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    static Status ok() { return Status(); }
    static Status notFound(std::string message) { return Status(StatusCode::NotFound, std::move(message)); }
    static Status alreadyExists(std::string message) { return Status(StatusCode::AlreadyExists, std::move(message)); }
    static Status invalidArgument(std::string message) { return Status(StatusCode::InvalidArgument, std::move(message)); }
    static Status ioError(std::string message) { return Status(StatusCode::IoError, std::move(message)); }
    static Status databaseError(std::string message) { return Status(StatusCode::DatabaseError, std::move(message)); }
    static Status parseError(std::string message) { return Status(StatusCode::ParseError, std::move(message)); }
    static Status internal(std::string message) { return Status(StatusCode::InternalError, std::move(message)); }

    bool isOk() const noexcept { return code_ == StatusCode::Ok; }
    explicit operator bool() const noexcept { return isOk(); }

    StatusCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }

private:
    StatusCode code_;
    std::string message_;
};

template <typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Status status) : data_(std::move(status)) {}

    static Result<T> failure(Status status) {
        return Result<T>(std::move(status));
    }

    bool isOk() const noexcept {
        return std::holds_alternative<T>(data_);
    }

    explicit operator bool() const noexcept {
        return isOk();
    }

    const T& value() const & {
        if (!isOk()) {
            throw std::runtime_error("Attempted to access value on error Result: " + status().message());
        }
        return std::get<T>(data_);
    }

    T& value() & {
        if (!isOk()) {
            throw std::runtime_error("Attempted to access value on error Result: " + status().message());
        }
        return std::get<T>(data_);
    }

    T&& value() && {
        if (!isOk()) {
            throw std::runtime_error("Attempted to access value on error Result: " + status().message());
        }
        return std::get<T>(std::move(data_));
    }

    T valueOr(T defaultValue) const & {
        if (isOk()) {
            return std::get<T>(data_);
        }
        return defaultValue;
    }

    Status status() const {
        if (isOk()) {
            return Status::ok();
        }
        return std::get<Status>(data_);
    }

private:
    std::variant<T, Status> data_;
};

} // namespace imagine
