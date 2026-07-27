#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct ProcessRule {
    std::string process_name;
    std::optional<std::string> cpu_priority;
    std::optional<std::string> io_priority;
    std::optional<std::string> cpu_affinity;
    bool background_only = false;
    bool eco_qos = false;
    bool launcher = false;
    int cpu_limit = 0;
    std::optional<int> smart_trim_threshold_mb;
    std::optional<int> cpu_throttle_trigger_pct;
    std::optional<int> cpu_throttle_duration_secs;
    bool instance_balance = false;
    bool disallowed = false;
    bool keep_alive = false;
    std::string executable_path;
};

struct EngineSettings {
    bool power_plan_enabled = true;
    int poll_interval_ms = 1000;
    int smart_trim_cooldown_secs = 30;
    int watchdog_max_retries = 5;
};

class ConfigManager {
public:
    ConfigManager(std::filesystem::path rulesPath, std::filesystem::path settingsPath);

    bool Load();
    bool CheckAndReload();

    [[nodiscard]] const std::vector<ProcessRule>& GetRules() const { return m_rules; }
    [[nodiscard]] const EngineSettings& GetSettings() const { return m_settings; }
    [[nodiscard]] std::optional<ProcessRule> FindRule(const std::string& processName) const;
    [[nodiscard]] const std::filesystem::path& GetRulesPath() const { return m_rulesPath; }
    [[nodiscard]] const std::filesystem::path& GetSettingsPath() const { return m_settingsPath; }

    static bool ValidateRule(const ProcessRule& rule, std::string& reason);

private:
    bool LoadRules();
    bool LoadSettings();

    std::filesystem::path m_rulesPath;
    std::filesystem::path m_settingsPath;
    std::vector<ProcessRule> m_rules;
    EngineSettings m_settings;
    std::filesystem::file_time_type m_rulesWriteTime{};
    std::filesystem::file_time_type m_settingsWriteTime{};
};
