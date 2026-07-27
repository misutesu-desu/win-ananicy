#include "logger.hpp"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <windows.h>
#include <filesystem>

namespace Logger {

namespace {
    std::mutex g_logMutex;
    std::ofstream g_logFile;
    bool g_initialized = false;

    std::string LevelToString(Level level) {
        switch (level) {
            case Level::Debug:   return "DEBUG";
            case Level::Info:    return "INFO";
            case Level::Warning: return "WARN";
            case Level::Error:   return "ERROR";
        }
        return "UNKNOWN";
    }

    std::string GetTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ) % 1000;

        std::tm tm_now;
        // Use thread-safe local time conversion on Windows
        localtime_s(&tm_now, &in_time_t);

        std::ostringstream oss;
        oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }
}

void Initialize(const std::wstring& logFilePath) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_initialized) return;

    const std::filesystem::path path(logFilePath);
    try {
        std::filesystem::create_directories(path.parent_path());
        constexpr std::uintmax_t maxLogBytes = 5U * 1024U * 1024U;
        if (std::filesystem::exists(path) &&
            std::filesystem::file_size(path) >= maxLogBytes) {
            const std::filesystem::path previous = path.wstring() + L".1";
            std::error_code error;
            std::filesystem::remove(previous, error);
            error.clear();
            std::filesystem::rename(path, previous, error);
        }
    } catch (...) {
        // Console and debugger logging remain available.
    }

    g_logFile.open(path, std::ios::out | std::ios::app);
    g_initialized = g_logFile.is_open();
}

void Log(Level level, std::string_view message) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    std::string timestamp = GetTimestamp();
    std::string levelStr = LevelToString(level);
    std::string formatted = "[" + timestamp + "] [" + levelStr + "] " + std::string(message) + "\n";

    // Write to standard output
    std::cout << formatted;
    std::cout.flush();

    // Write to the file if open
    if (g_initialized && g_logFile.is_open()) {
        g_logFile << formatted;
        g_logFile.flush();
    }

    // Output to the Windows debugger (accessible via DbgView)
    OutputDebugStringA(formatted.c_str());
}

void Debug(std::string_view message) { Log(Level::Debug, message); }
void Info(std::string_view message) { Log(Level::Info, message); }
void Warn(std::string_view message) { Log(Level::Warning, message); }
void Error(std::string_view message) { Log(Level::Error, message); }

} // namespace Logger
