#pragma once

#include <string>
#include <string_view>
#include <mutex>
#include <fstream>

namespace Logger {

enum class Level {
    Debug,
    Info,
    Warning,
    Error
};

// Initializes the logger. Creates/opens the log file path.
void Initialize(const std::wstring& logFilePath);

// Logs a message with a specific log level.
void Log(Level level, std::string_view message);

// Convienence wrappers
void Debug(std::string_view message);
void Info(std::string_view message);
void Warn(std::string_view message);
void Error(std::string_view message);

} // namespace Logger
