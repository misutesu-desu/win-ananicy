#include "config.hpp"
#include "logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

using json = nlohmann::json;

namespace {

std::string ToLower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

bool IsOneOf(std::string_view value, std::initializer_list<std::string_view> values) {
    const std::string lowered = ToLower(value);
    return std::any_of(values.begin(), values.end(), [&](std::string_view candidate) {
        return lowered == ToLower(candidate);
    });
}

template <typename T>
void ReadOptional(const json& item, const char* key, std::optional<T>& destination) {
    if (item.contains(key) && !item[key].is_null()) {
        destination = item[key].get<T>();
    }
}

void ReadBoolean(const json& item, const char* key, bool& destination) {
    if (!item.contains(key) || item[key].is_null()) {
        return;
    }
    if (!item[key].is_boolean()) {
        throw std::runtime_error(std::string(key) + " must be a boolean");
    }
    destination = item[key].get<bool>();
}

void ReadInteger(const json& item, const char* key, int& destination) {
    if (!item.contains(key) || item[key].is_null()) {
        return;
    }
    if (!item[key].is_number_integer()) {
        throw std::runtime_error(std::string(key) + " must be an integer");
    }
    destination = item[key].get<int>();
}

} // namespace

ConfigManager::ConfigManager(
    std::filesystem::path rulesPath,
    std::filesystem::path settingsPath)
    : m_rulesPath(std::move(rulesPath)),
      m_settingsPath(std::move(settingsPath)) {}

bool ConfigManager::Load() {
    const bool rulesLoaded = LoadRules();
    const bool settingsLoaded = LoadSettings();
    return rulesLoaded && settingsLoaded;
}

bool ConfigManager::LoadRules() {
    try {
        if (!std::filesystem::exists(m_rulesPath)) {
            Logger::Warn("Rules file does not exist: " + m_rulesPath.string());
            return false;
        }

        std::ifstream file(m_rulesPath);
        if (!file.is_open()) {
            Logger::Error("Failed to open rules file: " + m_rulesPath.string());
            return false;
        }

        json data;
        file >> data;
        if (!data.is_array()) {
            Logger::Error("Rules root must be a JSON array.");
            return false;
        }

        std::vector<ProcessRule> parsedRules;
        std::unordered_set<std::string> seenNames;

        for (const auto& item : data) {
            if (!item.is_object() ||
                !item.contains("process_name") ||
                !item["process_name"].is_string()) {
                Logger::Error("A rule is missing a valid process_name.");
                return false;
            }

            ProcessRule rule;
            rule.process_name = item["process_name"].get<std::string>();
            ReadOptional(item, "cpu_priority", rule.cpu_priority);
            ReadOptional(item, "io_priority", rule.io_priority);
            ReadOptional(item, "cpu_affinity", rule.cpu_affinity);
            ReadBoolean(item, "background_only", rule.background_only);
            ReadBoolean(item, "eco_qos", rule.eco_qos);
            ReadBoolean(item, "launcher", rule.launcher);
            ReadInteger(item, "cpu_limit", rule.cpu_limit);
            ReadOptional(item, "smart_trim_threshold_mb", rule.smart_trim_threshold_mb);
            ReadOptional(item, "cpu_throttle_trigger_pct", rule.cpu_throttle_trigger_pct);
            ReadOptional(item, "cpu_throttle_duration_secs", rule.cpu_throttle_duration_secs);
            ReadBoolean(item, "instance_balance", rule.instance_balance);
            ReadBoolean(item, "disallowed", rule.disallowed);
            ReadBoolean(item, "keep_alive", rule.keep_alive);
            if (item.contains("executable_path") && item["executable_path"].is_string()) {
                rule.executable_path = item["executable_path"].get<std::string>();
            }

            std::string reason;
            if (!ValidateRule(rule, reason)) {
                Logger::Error("Invalid rule for '" + rule.process_name + "': " + reason);
                return false;
            }

            const std::string key = ToLower(rule.process_name);
            if (!seenNames.insert(key).second) {
                Logger::Error("Duplicate rule for '" + rule.process_name + "'.");
                return false;
            }
            parsedRules.push_back(std::move(rule));
        }

        m_rules = std::move(parsedRules);
        m_rulesWriteTime = std::filesystem::last_write_time(m_rulesPath);
        Logger::Info("Loaded " + std::to_string(m_rules.size()) + " validated process rules.");
        return true;
    } catch (const std::exception& ex) {
        Logger::Error("Failed to parse rules: " + std::string(ex.what()));
        return false;
    }
}

bool ConfigManager::LoadSettings() {
    try {
        EngineSettings parsed;
        if (std::filesystem::exists(m_settingsPath)) {
            std::ifstream file(m_settingsPath);
            if (!file.is_open()) {
                Logger::Error("Failed to open engine settings: " + m_settingsPath.string());
                return false;
            }

            json data;
            file >> data;
            if (!data.is_object()) {
                Logger::Error("Engine settings root must be a JSON object.");
                return false;
            }

            ReadBoolean(data, "power_plan_enabled", parsed.power_plan_enabled);
            ReadInteger(data, "poll_interval_ms", parsed.poll_interval_ms);
            ReadInteger(data, "smart_trim_cooldown_secs", parsed.smart_trim_cooldown_secs);
            ReadInteger(data, "watchdog_max_retries", parsed.watchdog_max_retries);
        } else {
            Logger::Info("Engine settings file not found; safe defaults will be used.");
        }

        if (parsed.poll_interval_ms < 250 || parsed.poll_interval_ms > 5000) {
            throw std::runtime_error("poll_interval_ms must be between 250 and 5000");
        }
        if (parsed.smart_trim_cooldown_secs < 5 ||
            parsed.smart_trim_cooldown_secs > 3600) {
            throw std::runtime_error(
                "smart_trim_cooldown_secs must be between 5 and 3600");
        }
        if (parsed.watchdog_max_retries < 0 ||
            parsed.watchdog_max_retries > 20) {
            throw std::runtime_error(
                "watchdog_max_retries must be between 0 and 20");
        }
        m_settings = parsed;

        if (std::filesystem::exists(m_settingsPath)) {
            m_settingsWriteTime = std::filesystem::last_write_time(m_settingsPath);
        }
        return true;
    } catch (const std::exception& ex) {
        Logger::Error("Failed to parse engine settings: " + std::string(ex.what()));
        return false;
    }
}

bool ConfigManager::CheckAndReload() {
    bool changed = false;
    try {
        if (std::filesystem::exists(m_rulesPath)) {
            const auto writeTime = std::filesystem::last_write_time(m_rulesPath);
            if (writeTime != m_rulesWriteTime) {
                Logger::Info("Rules change detected.");
                changed = LoadRules() || changed;
            }
        }

        if (std::filesystem::exists(m_settingsPath)) {
            const auto writeTime = std::filesystem::last_write_time(m_settingsPath);
            if (writeTime != m_settingsWriteTime) {
                Logger::Info("Engine settings change detected.");
                changed = LoadSettings() || changed;
            }
        }
    } catch (const std::exception& ex) {
        Logger::Debug("Configuration reload check deferred: " + std::string(ex.what()));
    }
    return changed;
}

std::optional<ProcessRule> ConfigManager::FindRule(const std::string& processName) const {
    const std::string key = ToLower(processName);
    for (const auto& rule : m_rules) {
        if (ToLower(rule.process_name) == key) {
            return rule;
        }
    }
    return std::nullopt;
}

bool ConfigManager::ValidateRule(const ProcessRule& rule, std::string& reason) {
    if (rule.process_name.empty() ||
        !ToLower(rule.process_name).ends_with(".exe") ||
        rule.process_name.find('/') != std::string::npos ||
        rule.process_name.find('\\') != std::string::npos) {
        reason = "process_name must be an executable file name, not a path";
        return false;
    }

    if (rule.cpu_priority &&
        !IsOneOf(*rule.cpu_priority,
                 {"Idle", "Below Normal", "Normal", "Above Normal", "High"})) {
        reason = "unknown CPU priority";
        return false;
    }

    if (rule.io_priority &&
        !IsOneOf(*rule.io_priority, {"Very Low", "Low", "Normal", "High"})) {
        reason = "unknown I/O priority";
        return false;
    }

    if (rule.cpu_limit < 0 || rule.cpu_limit > 99) {
        reason = "cpu_limit must be between 1 and 99, or 0 to disable";
        return false;
    }

    if (rule.smart_trim_threshold_mb &&
        (*rule.smart_trim_threshold_mb < 32 || *rule.smart_trim_threshold_mb > 1048576)) {
        reason = "SmartTrim threshold must be between 32 MB and 1 TB";
        return false;
    }

    if (rule.cpu_throttle_trigger_pct.has_value() !=
        rule.cpu_throttle_duration_secs.has_value()) {
        reason = "CPU limiter threshold and duration must be configured together";
        return false;
    }

    if (rule.cpu_throttle_trigger_pct &&
        (*rule.cpu_throttle_trigger_pct < 1 || *rule.cpu_throttle_trigger_pct > 100)) {
        reason = "CPU limiter threshold must be between 1 and 100";
        return false;
    }

    if (rule.cpu_throttle_duration_secs &&
        (*rule.cpu_throttle_duration_secs < 1 || *rule.cpu_throttle_duration_secs > 3600)) {
        reason = "CPU limiter duration must be between 1 and 3600 seconds";
        return false;
    }

    if (rule.instance_balance && rule.cpu_throttle_trigger_pct) {
        reason = "Instance Balancer and CPU Affinity Limiter cannot be enabled together";
        return false;
    }

    if (rule.disallowed && rule.keep_alive) {
        reason = "Disallowed and Keep Alive cannot be enabled together";
        return false;
    }

    if (rule.keep_alive) {
        if (rule.executable_path.empty()) {
            reason = "Keep Alive requires an executable path";
            return false;
        }
        const std::filesystem::path executable(rule.executable_path);
        if (!executable.is_absolute() || ToLower(executable.extension().string()) != ".exe") {
            reason = "Keep Alive path must be an absolute .exe path";
            return false;
        }
    }

    return true;
}
