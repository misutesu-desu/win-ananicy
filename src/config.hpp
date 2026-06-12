#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>

struct ProcessRule {
    std::string process_name;
    std::optional<std::string> cpu_priority; // "Idle", "Below Normal", "Normal", "Above Normal", "High", "Realtime"
    std::optional<std::string> io_priority;  // "Very Low", "Low", "Normal", "High"
    std::optional<std::string> cpu_affinity; // e.g., "0,1,2,3"
    bool background_only = false;
    bool eco_qos = false;
    bool launcher = false;
    int cpu_limit = 0;

    // Advanced Process Lasso-style fields
    std::optional<int> smart_trim_threshold_mb;
    std::optional<int> cpu_throttle_trigger_pct;
    std::optional<int> cpu_throttle_duration_secs;
    bool instance_balance = false;
    bool disallowed = false;
    bool keep_alive = false;
    std::string executable_path;
};

class ConfigManager {
public:
    explicit ConfigManager(std::filesystem::path configPath);

    // Loads the configuration file. Returns true if successful.
    bool Load();

    // Checks if the configuration file has been modified, and reloads it if so.
    // Returns true if it was reloaded.
    bool CheckAndReload();

    // Returns the list of parsed rules.
    const std::vector<ProcessRule>& GetRules() const { return m_rules; }

    // Searches for a matching rule for a process name (case-insensitive).
    std::optional<ProcessRule> FindRule(const std::string& processName) const;

private:
    std::filesystem::path m_configPath;
    std::vector<ProcessRule> m_rules;
    std::filesystem::file_time_type m_lastWriteTime;
};
