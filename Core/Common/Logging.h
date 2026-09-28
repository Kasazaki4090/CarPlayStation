#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace CarPlayStation::Core::Common {

enum class LogLevel : uint8_t {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3
};

[[nodiscard]] constexpr std::string_view LogLevelToString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

// Pluggable log sink. The default writes to stdout; implementations (file, UI console,
// remote telemetry) can replace it at runtime via Logger::SetSink().
class ILogSink {
public:
    virtual ~ILogSink() = default;

    virtual void Write(LogLevel level, std::string_view tag, const std::string& message) = 0;
};

namespace detail {

class ConsoleLogSink final : public ILogSink {
public:
    void Write(LogLevel level, std::string_view tag, const std::string& message) override {
        char stamp[16]{};
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &now);
#else
        localtime_r(&now, &tm_buf);
#endif
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm_buf);

        std::printf("[%s] [%s] [%.*s] %s\n",
                    stamp,
                    LogLevelToString(level).data(),
                    static_cast<int>(tag.size()),
                    tag.data(),
                    message.c_str());
        std::fflush(stdout);
    }
};

} // namespace detail

// Thread-safe logging facade (singleton). Formatting happens on the caller's stack;
// only the final write is serialized through the mutex.
class Logger {
public:
    [[nodiscard]] static Logger& Instance() noexcept {
        static Logger instance;
        return instance;
    }

    // Replace the active sink (e.g. with a file or UI-backed implementation)
    void SetSink(std::shared_ptr<ILogSink> sink) {
        std::lock_guard lock(mutex_);
        if (sink) {
            sink_ = std::move(sink);
        }
    }

    // Stream-style variadic formatting: CPS_LOG_INFO("tag", "value ", 42, 'x')
    template <typename... Args>
    void Log(LogLevel level, std::string_view tag, Args&&... args) const {
        std::ostringstream oss;
        (void)(oss << ... << std::forward<Args>(args));

        const std::string message = oss.str();
        std::lock_guard lock(mutex_);
        if (sink_) {
            sink_->Write(level, tag, message);
        }
    }

private:
    Logger() : sink_(std::make_shared<detail::ConsoleLogSink>()) {}

    mutable std::mutex mutex_;
    std::shared_ptr<ILogSink> sink_;
};

} // namespace CarPlayStation::Core::Common

#define CPS_LOG_DEBUG(tag, ...) \
    ::CarPlayStation::Core::Common::Logger::Instance().Log(::CarPlayStation::Core::Common::LogLevel::Debug, tag, __VA_ARGS__)
#define CPS_LOG_INFO(tag, ...) \
    ::CarPlayStation::Core::Common::Logger::Instance().Log(::CarPlayStation::Core::Common::LogLevel::Info, tag, __VA_ARGS__)
#define CPS_LOG_WARN(tag, ...) \
    ::CarPlayStation::Core::Common::Logger::Instance().Log(::CarPlayStation::Core::Common::LogLevel::Warn, tag, __VA_ARGS__)
#define CPS_LOG_ERROR(tag, ...) \
    ::CarPlayStation::Core::Common::Logger::Instance().Log(::CarPlayStation::Core::Common::LogLevel::Error, tag, __VA_ARGS__)
