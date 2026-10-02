#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

// Shared timestamp helper: "YYYY-MM-DD HH:MM:SS.mmm"
inline std::string NowStamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(now);
    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &secs);
#else
    localtime_r(&secs, &tm_buf);
#endif
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf);
    char full[48]{};
    std::snprintf(full, sizeof(full), "%s.%03d", stamp, ms);
    return std::string(full);
}

class ConsoleLogSink final : public ILogSink {
public:
    void Write(LogLevel level, std::string_view tag, const std::string& message) override {
        std::printf("[%s] [%s] [%.*s] %s\n",
                    NowStamp().c_str(),
                    LogLevelToString(level).data(),
                    static_cast<int>(tag.size()),
                    tag.data(),
                    message.c_str());
        std::fflush(stdout);
    }
};

// Appends every line to a file (created/append mode). Thread-safe via the Logger mutex.
class FileLogSink final : public ILogSink {
public:
    explicit FileLogSink(const std::string& path) {
        file_ = std::make_unique<std::ofstream>(path, std::ios::app);
    }

    void Write(LogLevel level, std::string_view tag, const std::string& message) override {
        if (!file_ || !file_->is_open()) {
            return;
        }
        (*file_) << "[" << NowStamp() << "] [" << LogLevelToString(level) << "] ["
                 << tag << "] " << message << "\n";
        file_->flush();
    }

private:
    std::unique_ptr<std::ofstream> file_;
};

// Fans a single log call out to several sinks (e.g. console + file).
class MultiLogSink final : public ILogSink {
public:
    void Add(std::shared_ptr<ILogSink> sink) {
        if (sink) {
            sinks_.push_back(std::move(sink));
        }
    }

    void Write(LogLevel level, std::string_view tag, const std::string& message) override {
        for (auto& s : sinks_) {
            s->Write(level, tag, message);
        }
    }

private:
    std::vector<std::shared_ptr<ILogSink>> sinks_;
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
