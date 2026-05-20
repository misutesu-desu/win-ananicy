#include "config.hpp"
#include "logger.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace {
    std::string ToLower(std::string_view str) {
        std::string res(str);
        std::transform(res.begin(), res.end(), res.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return res;
    }
}

ConfigManager::ConfigManager(std::filesystem::path configPath)
    : m_configPath(std::move(configPath)), m_lastWriteTime{} {}

bool ConfigManager::Load() {
    try {
        if (!std::filesystem::exists(m_configPath)) {
            Logger::Error("Configuration file does not exist: " + m_configPath.string());
            return false;
        }

        std::ifstream file(m_configPath);
        if (!file.is_open()) {
            Logger::Error("Failed to open configuration file: " + m_configPath.string());
            return false;
        }

        json data;
        file >> data;

        std::vector<ProcessRule> newRules;
        if (data.is_array()) {
            for (const auto& item : data) {
                if (!item.contains("process_name") || !item["process_name"].is_string()) {
                    Logger::Warn("Skipping invalid config entry (missing or invalid process_name)");
                    continue;
                }

                ProcessRule rule;
                rule.process_name = item["process_name"].get<std::string>();

                if (item.contains("cpu_priority") && item["cpu_priority"].is_string()) {
                    rule.cpu_priority = item["cpu_priority"].get<std::string>();
                }
                if (item.contains("io_priority") && item["io_priority"].is_string()) {
                    rule.io_priority = item["io_priority"].get<std::string>();
                }
                if (item.contains("cpu_affinity") && item["cpu_affinity"].is_string()) {
                    rule.cpu_affinity = item["cpu_affinity"].get<std::string>();
                }
                if (item.contains("background_only") && item["background_only"].is_boolean()) {
                    rule.background_only = item["background_only"].get<bool>();
                }

                newRules.push_back(std::move(rule));
            }
        } else {
            Logger::Error("Configuration root is not a JSON array.");
            return false;
        }

        m_rules = std::move(newRules);
        m_lastWriteTime = std::filesystem::last_write_time(m_configPath);
        Logger::Info("Loaded " + std::to_string(m_rules.size()) + " process optimization rules.");
        return true;
    }
    catch (const std::exception& e) {
        Logger::Error("Exception caught while parsing config: " + std::string(e.what()));
        return false;
    }
}

bool ConfigManager::CheckAndReload() {
    try {
        if (!std::filesystem::exists(m_configPath)) {
            return false;
        }

        auto currentWriteTime = std::filesystem::last_write_time(m_configPath);
        if (currentWriteTime != m_lastWriteTime) {
            Logger::Info("Configuration change detected. Reloading rules...");
            return Load();
        }
    }
    catch (const std::exception& e) {
        // Can fail temporarily if the configuration file is locked during editing
        Logger::Debug("Exception checking file write time: " + std::string(e.what()));
    }
    return false;
}

std::optional<ProcessRule> ConfigManager::FindRule(const std::string& processName) const {
    std::string lowerProcess = ToLower(processName);
    for (const auto& rule : m_rules) {
        if (ToLower(rule.process_name) == lowerProcess) {
            return rule;
        }
    }
    return std::nullopt;
}
