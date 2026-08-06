#pragma once

#include <string>
#include <variant>
#include <optional>
#include <stdexcept>
#include <utility>

namespace OpenTune {

enum class ErrorCode : int {
    Success = 0,
    
    ModelNotFound = 100,
    ModelLoadFailed = 101,
    ModelInferenceFailed = 102,
    InvalidModelType = 103,
    SessionCreationFailed = 104,
    
    NotInitialized = 200,
    
    InvalidAudioInput = 300,
    InvalidAudioLength = 302,
    
    MelFFTSizeInvalid = 501,
    MelNotConfigured = 502,
    
    InvalidParameter = 600,
    OperationCancelled = 602,
    
    UnknownError = 999
};

inline const char* errorCodeMessage(ErrorCode code) {
    switch (code) {
        case ErrorCode::Success: return "Operation completed successfully";
        
        case ErrorCode::ModelNotFound: return "Model file not found";
        case ErrorCode::ModelLoadFailed: return "Failed to load model";
        case ErrorCode::ModelInferenceFailed: return "Model inference failed";
        case ErrorCode::InvalidModelType: return "Invalid model type specified";
        case ErrorCode::SessionCreationFailed: return "Failed to create ONNX session";
        
        case ErrorCode::NotInitialized: return "System not initialized";
        
        case ErrorCode::InvalidAudioInput: return "Invalid audio input";
        case ErrorCode::InvalidAudioLength: return "Invalid audio length";
        
        case ErrorCode::MelFFTSizeInvalid: return "FFT size must be power of 2";
        case ErrorCode::MelNotConfigured: return "Mel spectrogram processor not configured";
        
        case ErrorCode::InvalidParameter: return "Invalid parameter value";
        case ErrorCode::OperationCancelled: return "Operation was cancelled";
        
        default: return "Unknown error";
    }
}

struct Error {
    ErrorCode code{ErrorCode::Success};
    std::string message;
    std::string context;
    
    Error() = default;
    
    explicit Error(ErrorCode c, std::string msg = "", std::string ctx = "")
        : code(c), message(std::move(msg)), context(std::move(ctx)) {}
    
    static Error fromCode(ErrorCode c, const std::string& ctx = "") {
        return Error(c, errorCodeMessage(c), ctx);
    }
    
    bool ok() const { return code == ErrorCode::Success; }
    explicit operator bool() const { return ok(); }
    
    std::string fullMessage() const {
        if (context.empty()) return message.empty() ? errorCodeMessage(code) : message;
        return context + ": " + (message.empty() ? errorCodeMessage(code) : message);
    }
};

template<typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error err) : data_(std::move(err)) {}
    
    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(ErrorCode code, const std::string& context = "") {
        return Result(Error::fromCode(code, context));
    }
    static Result failure(Error err) { return Result(std::move(err)); }
    
    bool ok() const { return std::holds_alternative<T>(data_); }
    explicit operator bool() const { return ok(); }
    
    const T& value() const& {
        if (!ok()) throw std::runtime_error(error().fullMessage());
        return std::get<T>(data_);
    }
    
    T&& value() && {
        if (!ok()) throw std::runtime_error(error().fullMessage());
        return std::get<T>(std::move(data_));
    }
    
    const Error& error() const& {
        return std::get<Error>(data_);
    }

private:
    std::variant<T, Error> data_;
};

template<>
class Result<void> {
public:
    Result() : error_(std::nullopt) {}
    Result(Error err) : error_(std::move(err)) {}
    
    static Result success() { return Result(); }
    static Result failure(ErrorCode code, const std::string& context = "") {
        return Result(Error::fromCode(code, context));
    }
    static Result failure(Error err) { return Result(std::move(err)); }
    
    bool ok() const { return !error_.has_value(); }
    explicit operator bool() const { return ok(); }
    
    void value() const {
        if (!ok()) throw std::runtime_error(error_->fullMessage());
    }
    
    const Error& error() const& { return *error_; }
    
private:
    std::optional<Error> error_;
};

}
