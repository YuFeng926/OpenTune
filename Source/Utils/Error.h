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
    AlreadyInitialized = 201,
    InitializationFailed = 202,
    
    InvalidAudioInput = 300,
    InvalidSampleRate = 301,
    InvalidAudioLength = 302,
    AudioTooShort = 303,
    
    InvalidF0Input = 400,
    F0ExtractionFailed = 401,
    VocoderSynthesisFailed = 402,
    
    MelConfigInvalid = 500,
    MelFFTSizeInvalid = 501,
    MelNotConfigured = 502,
    
    InvalidParameter = 600,
    OutOfMemory = 601,
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
        case ErrorCode::AlreadyInitialized: return "System already initialized";
        case ErrorCode::InitializationFailed: return "Initialization failed";
        
        case ErrorCode::InvalidAudioInput: return "Invalid audio input";
        case ErrorCode::InvalidSampleRate: return "Invalid sample rate";
        case ErrorCode::InvalidAudioLength: return "Invalid audio length";
        case ErrorCode::AudioTooShort: return "Audio too short for processing";
        
        case ErrorCode::InvalidF0Input: return "Invalid F0 input";
        case ErrorCode::F0ExtractionFailed: return "F0 extraction failed";
        case ErrorCode::VocoderSynthesisFailed: return "Vocoder synthesis failed";
        
        case ErrorCode::MelConfigInvalid: return "Invalid Mel spectrogram configuration";
        case ErrorCode::MelFFTSizeInvalid: return "FFT size must be power of 2";
        case ErrorCode::MelNotConfigured: return "Mel spectrogram processor not configured";
        
        case ErrorCode::InvalidParameter: return "Invalid parameter value";
        case ErrorCode::OutOfMemory: return "Out of memory";
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
    
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(value()); }
    
    const T* operator->() const { return &value(); }
    
    const Error& error() const& {
        return std::get<Error>(data_);
    }
    
    T valueOr(T defaultValue) const& {
        return ok() ? value() : std::move(defaultValue);
    }
    
    T valueOr(T defaultValue) && {
        return ok() ? std::move(value()) : std::move(defaultValue);
    }
    
    template<typename F>
    auto map(F&& f) const& -> Result<decltype(f(std::declval<T>()))> {
        using U = decltype(f(std::declval<T>()));
        if (ok()) return Result<U>::success(f(value()));
        return Result<U>::failure(error());
    }
    
    template<typename F>
    auto andThen(F&& f) const& -> decltype(f(std::declval<T>())) {
        using ResultType = decltype(f(std::declval<T>()));
        if (ok()) return f(value());
        return ResultType::failure(error());
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
