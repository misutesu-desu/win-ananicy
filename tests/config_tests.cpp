#include "config.hpp"
#include "process_utils.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestRuleValidation() {
    ProcessRule valid;
    valid.process_name = "game.exe";
    valid.cpu_priority = "High";
    valid.io_priority = "Normal";
    valid.cpu_affinity = "0,2,4";
    valid.cpu_limit = 60;

    std::string reason;
    Expect(ConfigManager::ValidateRule(valid, reason), "valid rule should pass: " + reason);

    auto invalidName = valid;
    invalidName.process_name = R"(C:\Games\game.exe)";
    Expect(!ConfigManager::ValidateRule(invalidName, reason), "process paths must be rejected");

    auto missingExtension = valid;
    missingExtension.process_name = "game";
    Expect(!ConfigManager::ValidateRule(missingExtension, reason), "process names must end in .exe");

    auto invalidPriority = valid;
    invalidPriority.cpu_priority = "Realtime";
    Expect(!ConfigManager::ValidateRule(invalidPriority, reason), "unsupported priority must be rejected");

    auto partialThrottle = valid;
    partialThrottle.cpu_throttle_trigger_pct = 80;
    Expect(!ConfigManager::ValidateRule(partialThrottle, reason), "partial throttle configuration must be rejected");

    auto conflict = valid;
    conflict.instance_balance = true;
    conflict.cpu_throttle_trigger_pct = 80;
    conflict.cpu_throttle_duration_secs = 5;
    Expect(!ConfigManager::ValidateRule(conflict, reason), "instance balance and throttle must conflict");

    auto watchdogConflict = valid;
    watchdogConflict.disallowed = true;
    watchdogConflict.keep_alive = true;
    watchdogConflict.executable_path = R"(C:\Apps\game.exe)";
    Expect(!ConfigManager::ValidateRule(watchdogConflict, reason), "disallowed and keep-alive must conflict");

    auto validWatchdog = valid;
    validWatchdog.keep_alive = true;
    validWatchdog.executable_path = R"(C:\Apps\game.exe)";
    Expect(ConfigManager::ValidateRule(validWatchdog, reason), "absolute watchdog path should pass");

    auto relativeWatchdog = validWatchdog;
    relativeWatchdog.executable_path = R"(Apps\game.exe)";
    Expect(!ConfigManager::ValidateRule(relativeWatchdog, reason), "relative watchdog path must fail");
}

void TestAffinityParsing() {
    DWORD_PTR mask = 0;
    std::string error;

    Expect(ProcessUtils::TryParseAffinity("0,2,4", mask, error), "core list should parse");
    Expect(mask == (static_cast<DWORD_PTR>(1) |
                    (static_cast<DWORD_PTR>(1) << 2U) |
                    (static_cast<DWORD_PTR>(1) << 4U)),
           "core list should map to the expected bits");

    Expect(ProcessUtils::TryParseAffinity("10", mask, error), "single core index should parse");
    Expect(mask == (static_cast<DWORD_PTR>(1) << 10U), "plain 10 must mean core 10, not decimal mask 10");

    Expect(ProcessUtils::TryParseAffinity("0xA", mask, error), "hex mask should parse");
    Expect(mask == static_cast<DWORD_PTR>(0xA), "hex mask should be preserved");

    Expect(ProcessUtils::TryParseAffinity("mask:10", mask, error), "explicit decimal mask should parse");
    Expect(mask == static_cast<DWORD_PTR>(10), "explicit decimal mask should be preserved");

    Expect(!ProcessUtils::TryParseAffinity("", mask, error), "empty affinity must fail");
    Expect(!ProcessUtils::TryParseAffinity("0,0", mask, error), "duplicate cores must fail");
    Expect(!ProcessUtils::TryParseAffinity("999", mask, error), "out-of-range core index must fail");
}

void TestConfigLoading() {
    const auto testRoot = std::filesystem::temp_directory_path() / "win-ananicy-config-tests";
    std::error_code ignored;
    std::filesystem::remove_all(testRoot, ignored);
    std::filesystem::create_directories(testRoot);

    const auto rulesPath = testRoot / "rules.json";
    const auto settingsPath = testRoot / "settings.json";
    {
        std::ofstream rules(rulesPath);
        rules << R"([{"process_name":"game.exe","cpu_priority":"High","background_only":true}])";
    }
    {
        std::ofstream settings(settingsPath);
        settings << R"({"power_plan_enabled":false,"poll_interval_ms":250,"smart_trim_cooldown_secs":90,"watchdog_max_retries":3})";
    }

    ConfigManager config(rulesPath, settingsPath);
    Expect(config.Load(), "valid files should load");
    Expect(config.GetRules().size() == 1, "one rule should be loaded");
    Expect(config.GetRules().front().background_only, "boolean fields should load");
    Expect(!config.GetSettings().power_plan_enabled, "settings should load");
    Expect(config.GetSettings().poll_interval_ms == 250, "poll interval should load");

    {
        std::ofstream rules(rulesPath, std::ios::trunc);
        rules << R"([{"process_name":"one.exe"},{"process_name":"ONE.EXE"}])";
    }
    ConfigManager duplicateConfig(rulesPath, settingsPath);
    Expect(!duplicateConfig.Load(), "duplicate process names should be rejected case-insensitively");

    {
        std::ofstream rules(rulesPath, std::ios::trunc);
        rules << R"([{"process_name":"game.exe","background_only":"yes"}])";
    }
    ConfigManager wrongTypeConfig(rulesPath, settingsPath);
    Expect(!wrongTypeConfig.Load(), "incorrect field types should be rejected");

    {
        std::ofstream rules(rulesPath, std::ios::trunc);
        rules << R"([])";
    }
    {
        std::ofstream settings(settingsPath, std::ios::trunc);
        settings << R"({"poll_interval_ms":10})";
    }
    ConfigManager invalidSettingsConfig(rulesPath, settingsPath);
    Expect(!invalidSettingsConfig.Load(), "out-of-range engine settings should be rejected");

    std::filesystem::remove_all(testRoot, ignored);
}

} // namespace

int main() {
    TestRuleValidation();
    TestAffinityParsing();
    TestConfigLoading();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }

    std::cout << "All WinAnanicy configuration tests passed.\n";
    return 0;
}
