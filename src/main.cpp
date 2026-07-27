#include "config.hpp"
#include "logger.hpp"
#include "process_utils.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef WINANANICY_VERSION
#define WINANANICY_VERSION "1.0.1"
#endif

namespace {

constexpr wchar_t EngineMutexName[] = L"Local\\WinAnanicy.Engine.Singleton";
constexpr wchar_t EngineStopEventName[] = L"Local\\WinAnanicy.Engine.Stop";
constexpr GUID OptimizerPowerPlan = {
    0xa7bc678d,
    0xd5df,
    0x448d,
    {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};

std::atomic<bool> g_running{true};
HANDLE g_stopEvent = nullptr;

struct HandleCloser {
    void operator()(HANDLE handle) const {
        if (handle && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct ProcessState {
    std::string process_name;
    bool original_captured = false;
    DWORD original_priority_class = NORMAL_PRIORITY_CLASS;
    DWORD_PTR original_affinity_mask = 0;
    bool has_original_affinity = false;
    ULONG original_io_priority = 2;
    bool has_original_io_priority = false;
    bool original_eco_qos = false;
    bool has_original_eco_qos = false;

    bool rules_applied = false;
    bool rule_apply_succeeded = false;
    bool is_foreground = false;
    std::chrono::steady_clock::time_point last_apply_attempt{};
    HANDLE cpu_rate_job = nullptr;

    ULONGLONG last_kernel_time = 0;
    ULONGLONG last_user_time = 0;
    std::chrono::steady_clock::time_point last_cpu_sample{};
    bool has_cpu_sample = false;
    std::optional<std::chrono::steady_clock::time_point> high_cpu_since;
    std::optional<std::chrono::steady_clock::time_point> low_cpu_since;
    bool is_throttled = false;

    bool instance_balanced = false;
    DWORD_PTR instance_balanced_mask = 0;
    std::chrono::steady_clock::time_point last_trim{};
};

struct WatchdogState {
    std::chrono::steady_clock::time_point last_attempt{};
    int consecutive_failures = 0;
};

std::string ToLower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    return ToLower(left) == ToLower(right);
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.c_str(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return {};
    }
    std::string converted(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.c_str(),
        static_cast<int>(value.size()),
        converted.data(),
        size,
        nullptr,
        nullptr);
    return converted;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.c_str(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (size <= 0) {
        return {};
    }
    std::wstring converted(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.c_str(),
        static_cast<int>(value.size()),
        converted.data(),
        size);
    return converted;
}

std::filesystem::path GetExecutablePath() {
    std::vector<wchar_t> buffer(512);
    while (true) {
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::filesystem::current_path() / L"win-ananicy.exe";
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(
                std::wstring(buffer.data(), static_cast<std::size_t>(length)));
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path GetDataDirectory() {
    PWSTR rawPath = nullptr;
    const HRESULT result = SHGetKnownFolderPath(
            FOLDERID_LocalAppData,
            KF_FLAG_DEFAULT,
            nullptr,
            &rawPath);
    if (SUCCEEDED(result) && rawPath) {
        std::filesystem::path path(rawPath);
        CoTaskMemFree(rawPath);
        return path / L"WinAnanicy";
    }

    std::vector<wchar_t> environmentPath(32768);
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA",
        environmentPath.data(),
        static_cast<DWORD>(environmentPath.size()));
    if (length > 0 && length < environmentPath.size()) {
        return std::filesystem::path(
                   std::wstring(environmentPath.data(), length)) /
               L"WinAnanicy";
    }
    return GetExecutablePath().parent_path() / L"data";
}

std::string UtcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc, &time);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

bool AtomicWriteJson(
    const std::filesystem::path& path,
    const nlohmann::json& document) {
    try {
        std::filesystem::create_directories(path.parent_path());
        const std::filesystem::path temporary = path.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::out | std::ios::trunc);
            if (!stream.is_open()) {
                return false;
            }
            stream << document.dump(2);
            stream.flush();
            if (!stream.good()) {
                return false;
            }
        }
        return MoveFileExW(
                   temporary.c_str(),
                   path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    } catch (...) {
        return false;
    }
}

bool CaptureOriginalState(
    DWORD pid,
    const ProcessRule& rule,
    ProcessState& state) {
    if (state.original_captured) {
        return true;
    }

    UniqueHandle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION |
            PROCESS_SET_INFORMATION |
            PROCESS_SET_QUOTA |
            PROCESS_TERMINATE,
        FALSE,
        pid));
    if (!process) {
        return false;
    }

    const DWORD priority = GetPriorityClass(process.get());
    if (rule.cpu_priority && priority == 0) {
        return false;
    }
    if (priority != 0) {
        state.original_priority_class = priority;
    }

    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    const bool needsAffinity =
        rule.cpu_affinity.has_value() ||
        rule.instance_balance ||
        rule.cpu_throttle_trigger_pct.has_value();
    if (GetProcessAffinityMask(process.get(), &processMask, &systemMask)) {
        state.original_affinity_mask = processMask;
        state.has_original_affinity = true;
    } else if (needsAffinity) {
        return false;
    }

    if (rule.io_priority) {
        state.has_original_io_priority =
            ProcessUtils::GetIoPriority(process.get(), state.original_io_priority);
        if (!state.has_original_io_priority) {
            return false;
        }
    }
    if (rule.eco_qos) {
        state.has_original_eco_qos =
            ProcessUtils::GetProcessEcoQoS(process.get(), state.original_eco_qos);
        if (!state.has_original_eco_qos) {
            return false;
        }
    }
    state.original_captured = true;
    return true;
}

void DisableCpuRate(ProcessState& state) {
    if (!state.cpu_rate_job) {
        return;
    }
    ProcessUtils::DisableProcessCpuRateLimit(state.cpu_rate_job);
    CloseHandle(state.cpu_rate_job);
    state.cpu_rate_job = nullptr;
}

void RestoreProcessState(DWORD pid, ProcessState& state) {
    DisableCpuRate(state);

    if (state.original_captured) {
        ProcessUtils::SetCpuPriorityClass(pid, state.original_priority_class);
        if (state.has_original_affinity) {
            ProcessUtils::SetCpuAffinityMask(pid, state.original_affinity_mask);
        }
        if (state.has_original_io_priority) {
            ProcessUtils::SetIoPriorityValue(pid, state.original_io_priority);
        }

        UniqueHandle process(OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid));
        if (process && state.has_original_eco_qos) {
            ProcessUtils::SetProcessEcoQoS(
                process.get(),
                state.original_eco_qos);
        }
    }

    state.rules_applied = false;
    state.rule_apply_succeeded = false;
    state.original_captured = false;
    state.has_original_affinity = false;
    state.has_original_io_priority = false;
    state.has_original_eco_qos = false;
    state.has_cpu_sample = false;
    state.high_cpu_since.reset();
    state.low_cpu_since.reset();
    state.is_throttled = false;
    state.instance_balanced = false;
    state.instance_balanced_mask = 0;
}

bool SpawnProcess(const std::wstring& executablePath) {
    const std::filesystem::path executable(executablePath);
    if (!std::filesystem::exists(executable) ||
        !std::filesystem::is_regular_file(executable)) {
        return false;
    }

    std::wstring commandLine = L"\"" + executable.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(),
        commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring workingDirectory =
        executable.parent_path().wstring();

    if (!CreateProcessW(
            executable.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startup,
            &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool ApplyBaseRule(
    DWORD pid,
    const ProcessRule& rule,
    ProcessState& state,
    const std::chrono::steady_clock::time_point now) {
    state.last_apply_attempt = now;
    if (!CaptureOriginalState(pid, rule, state)) {
        state.rule_apply_succeeded = false;
        return false;
    }

    bool success = true;
    if (rule.cpu_priority) {
        success = ProcessUtils::SetCpuPriority(pid, *rule.cpu_priority) && success;
    }
    if (rule.io_priority) {
        success = ProcessUtils::SetIoPriority(pid, *rule.io_priority) && success;
    }
    if (rule.cpu_affinity && !rule.instance_balance) {
        success = ProcessUtils::SetCpuAffinity(pid, *rule.cpu_affinity) && success;
    }

    UniqueHandle process(OpenProcess(
        PROCESS_SET_INFORMATION |
            PROCESS_SET_QUOTA |
            PROCESS_TERMINATE |
            PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        pid));
    if (process) {
        if (rule.eco_qos) {
            success =
                ProcessUtils::SetProcessEcoQoS(process.get(), true) && success;
        }
        if (rule.cpu_limit > 0 && !state.cpu_rate_job) {
            state.cpu_rate_job =
                ProcessUtils::CreateProcessCpuRateLimit(process.get(), rule.cpu_limit);
            success = state.cpu_rate_job != nullptr && success;
        } else if (rule.cpu_limit == 0) {
            DisableCpuRate(state);
        }
    } else if (rule.eco_qos || rule.cpu_limit > 0) {
        success = false;
    }

    state.rules_applied = true;
    state.rule_apply_succeeded = success;
    return success;
}

bool RuleHasOptimizationEffect(const ProcessRule& rule) {
    return rule.cpu_priority.has_value() ||
           rule.io_priority.has_value() ||
           rule.cpu_affinity.has_value() ||
           rule.eco_qos ||
           rule.cpu_limit > 0 ||
           rule.smart_trim_threshold_mb.has_value() ||
           rule.cpu_throttle_trigger_pct.has_value() ||
           rule.instance_balance ||
           rule.launcher;
}

double SampleCpuUsage(
    HANDLE process,
    ProcessState& state,
    DWORD logicalCores,
    const std::chrono::steady_clock::time_point now) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return -1.0;
    }

    ULARGE_INTEGER kernelValue{};
    kernelValue.LowPart = kernel.dwLowDateTime;
    kernelValue.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER userValue{};
    userValue.LowPart = user.dwLowDateTime;
    userValue.HighPart = user.dwHighDateTime;

    double usage = -1.0;
    if (state.has_cpu_sample && logicalCores > 0) {
        const auto elapsed =
            std::chrono::duration<double>(now - state.last_cpu_sample).count();
        if (elapsed > 0.0) {
            const ULONGLONG processDelta =
                (kernelValue.QuadPart - state.last_kernel_time) +
                (userValue.QuadPart - state.last_user_time);
            constexpr double hundredNanosecondsPerSecond = 10000000.0;
            usage =
                (static_cast<double>(processDelta) /
                 hundredNanosecondsPerSecond /
                 elapsed /
                 static_cast<double>(logicalCores)) *
                100.0;
        }
    }

    state.last_kernel_time = kernelValue.QuadPart;
    state.last_user_time = userValue.QuadPart;
    state.last_cpu_sample = now;
    state.has_cpu_sample = true;
    return usage;
}

void ApplyCpuLimiter(
    DWORD pid,
    const ProcessRule& rule,
    ProcessState& state,
    DWORD logicalCores,
    const std::chrono::steady_clock::time_point now) {
    if (!rule.cpu_throttle_trigger_pct ||
        !rule.cpu_throttle_duration_secs) {
        return;
    }

    UniqueHandle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION,
        FALSE,
        pid));
    if (!process) {
        return;
    }

    const double usage = SampleCpuUsage(process.get(), state, logicalCores, now);
    if (usage < 0.0) {
        return;
    }

    const double trigger =
        static_cast<double>(*rule.cpu_throttle_trigger_pct);
    if (usage >= trigger) {
        state.low_cpu_since.reset();
        if (!state.high_cpu_since) {
            state.high_cpu_since = now;
        }
        const auto duration = std::chrono::duration_cast<std::chrono::seconds>(
            now - *state.high_cpu_since);
        if (!state.is_throttled &&
            duration.count() >= *rule.cpu_throttle_duration_secs) {
            const unsigned int affinityCoreCount = std::max(
                1U,
                std::min(
                    static_cast<unsigned int>(logicalCores),
                    static_cast<unsigned int>(sizeof(DWORD_PTR) * 8U)));
            DWORD_PTR restricted = 1;
            if (affinityCoreCount >= 2) {
                restricted =
                    (static_cast<DWORD_PTR>(1) << (affinityCoreCount - 1)) |
                    (static_cast<DWORD_PTR>(1) << (affinityCoreCount - 2));
            }
            if (ProcessUtils::SetCpuAffinityMask(pid, restricted)) {
                state.is_throttled = true;
                Logger::Info(
                    "[CPU Limiter] Restricted " + rule.process_name +
                    " after sustained load.");
            }
        }
        return;
    }

    state.high_cpu_since.reset();
    if (!state.is_throttled || usage >= trigger / 2.0) {
        state.low_cpu_since.reset();
        return;
    }

    if (!state.low_cpu_since) {
        state.low_cpu_since = now;
    }
    if (now - *state.low_cpu_since < std::chrono::seconds(5)) {
        return;
    }

    bool restored = false;
    if (rule.cpu_affinity) {
        restored = ProcessUtils::SetCpuAffinity(pid, *rule.cpu_affinity);
    } else if (state.has_original_affinity) {
        restored =
            ProcessUtils::SetCpuAffinityMask(pid, state.original_affinity_mask);
    }
    if (restored) {
        state.is_throttled = false;
        state.low_cpu_since.reset();
        Logger::Info("[CPU Limiter] Restored affinity for " + rule.process_name + ".");
    }
}

void ApplySmartTrim(
    DWORD pid,
    const ProcessRule& rule,
    ProcessState& state,
    int cooldownSeconds,
    const std::chrono::steady_clock::time_point now) {
    if (!rule.smart_trim_threshold_mb ||
        (state.last_trim.time_since_epoch().count() != 0 &&
         now - state.last_trim < std::chrono::seconds(cooldownSeconds))) {
        return;
    }

    UniqueHandle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA,
        FALSE,
        pid));
    if (!process) {
        return;
    }

    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(process.get(), &counters, sizeof(counters))) {
        return;
    }

    const SIZE_T workingSetMb = counters.WorkingSetSize / (1024U * 1024U);
    if (workingSetMb <=
        static_cast<SIZE_T>(*rule.smart_trim_threshold_mb)) {
        return;
    }

    if (ProcessUtils::TrimProcessMemory(process.get())) {
        state.last_trim = now;
        Logger::Info(
            "[SmartTrim] Reclaimed memory from " + rule.process_name +
            " at " + std::to_string(workingSetMb) + " MB.");
    }
}

void RestoreAll(std::unordered_map<DWORD, ProcessState>& tracked) {
    for (auto& [pid, state] : tracked) {
        RestoreProcessState(pid, state);
    }
}

void WriteEngineStatus(
    const std::filesystem::path& statusPath,
    bool running,
    const ConfigManager& config,
    DWORD foregroundPid,
    std::size_t matchedProcesses,
    std::size_t appliedProcesses,
    bool powerPlanActive,
    const std::string& startedAt) {
    nlohmann::json status = {
        {"version", WINANANICY_VERSION},
        {"running", running},
        {"pid", GetCurrentProcessId()},
        {"started_at_utc", startedAt},
        {"last_cycle_utc", UtcTimestamp()},
        {"rules_path", WideToUtf8(config.GetRulesPath().wstring())},
        {"settings_path", WideToUtf8(config.GetSettingsPath().wstring())},
        {"rules_loaded", config.GetRules().size()},
        {"matched_processes", matchedProcesses},
        {"applied_processes", appliedProcesses},
        {"foreground_pid", foregroundPid},
        {"power_plan_active", powerPlanActive}};
    AtomicWriteJson(statusPath, status);
}

void MainLoop(
    const std::filesystem::path& rulesPath,
    const std::filesystem::path& settingsPath,
    const std::filesystem::path& statusPath) {
    Logger::Info(
        "WinAnanicy " + std::string(WINANANICY_VERSION) + " engine started.");
    ProcessUtils::EnableRequiredPrivileges();

    ConfigManager config(rulesPath, settingsPath);
    config.Load();

    bool powerPlanReady = false;
    bool powerPlanActive = false;
    bool hasPreviousPowerPlan = false;
    GUID previousPowerPlan{};
    std::unordered_map<DWORD, ProcessState> tracked;
    std::unordered_map<std::string, WatchdogState> watchdogs;
    const std::string startedAt = UtcTimestamp();

    while (g_running) {
        if (g_stopEvent &&
            WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) {
            break;
        }

        if (config.CheckAndReload()) {
            RestoreAll(tracked);
            if (!config.GetSettings().power_plan_enabled && powerPlanActive) {
                if (hasPreviousPowerPlan) {
                    ProcessUtils::SetActivePowerScheme(previousPowerPlan);
                }
                powerPlanActive = false;
            }
        }

        DWORD foregroundPid = 0;
        if (HWND foreground = GetForegroundWindow()) {
            GetWindowThreadProcessId(foreground, &foregroundPid);
        }

        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        const DWORD logicalCores =
            std::max<DWORD>(1, systemInfo.dwNumberOfProcessors);

        UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) {
            Logger::Warn("Process snapshot failed.");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snapshot.get(), &entry)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        std::unordered_set<DWORD> currentPids;
        std::unordered_set<std::string> runningNames;
        std::unordered_map<std::string, std::vector<DWORD>> balanceGroups;
        std::size_t matchedProcesses = 0;
        std::size_t appliedProcesses = 0;

        do {
            const DWORD pid = entry.th32ProcessID;
            if (pid == 0 || pid == GetCurrentProcessId()) {
                continue;
            }

            const std::string name = WideToUtf8(entry.szExeFile);
            const std::string lowerName = ToLower(name);
            currentPids.insert(pid);
            runningNames.insert(lowerName);

            const auto ruleOptional = config.FindRule(name);
            if (!ruleOptional) {
                continue;
            }
            ++matchedProcesses;
            const ProcessRule& rule = *ruleOptional;

            if (rule.disallowed) {
                UniqueHandle process(OpenProcess(PROCESS_TERMINATE, FALSE, pid));
                if (process && TerminateProcess(process.get(), 0)) {
                    Logger::Info("[Blocklist] Terminated " + name + ".");
                }
                continue;
            }

            auto& state = tracked[pid];
            if (!state.process_name.empty() &&
                !EqualsIgnoreCase(state.process_name, name)) {
                RestoreProcessState(pid, state);
                state = ProcessState{};
            }
            state.process_name = name;

            const bool foreground = pid == foregroundPid;
            const bool eligible = !rule.background_only || !foreground;

            if (!eligible) {
                if (state.rules_applied) {
                    RestoreProcessState(pid, state);
                    Logger::Info(
                        "Restored foreground process " + name + " to its baseline.");
                }
                state.is_foreground = true;
                continue;
            }

            const bool retryFailedApply =
                state.rules_applied &&
                !state.rule_apply_succeeded &&
                now - state.last_apply_attempt >= std::chrono::seconds(5);
            if (!state.rules_applied || retryFailedApply) {
                ApplyBaseRule(pid, rule, state, now);
            }
            state.is_foreground = foreground;
            if (state.rules_applied &&
                state.rule_apply_succeeded &&
                RuleHasOptimizationEffect(rule)) {
                ++appliedProcesses;
            }

            ApplySmartTrim(
                pid,
                rule,
                state,
                config.GetSettings().smart_trim_cooldown_secs,
                now);
            if (state.rule_apply_succeeded) {
                ApplyCpuLimiter(pid, rule, state, logicalCores, now);
            }
            if (rule.instance_balance &&
                state.rule_apply_succeeded &&
                !state.is_throttled) {
                balanceGroups[lowerName].push_back(pid);
            }
        } while (Process32NextW(snapshot.get(), &entry));

        for (auto& [name, pids] : balanceGroups) {
            const unsigned int affinityCoreCount = std::max(
                1U,
                std::min(
                    static_cast<unsigned int>(logicalCores),
                    static_cast<unsigned int>(sizeof(DWORD_PTR) * 8U)));
            std::sort(pids.begin(), pids.end());
            for (std::size_t index = 0; index < pids.size(); ++index) {
                const DWORD pid = pids[index];
                const DWORD_PTR target =
                    static_cast<DWORD_PTR>(1)
                    << static_cast<unsigned int>(index % affinityCoreCount);
                auto& state = tracked[pid];
                if ((!state.instance_balanced ||
                     state.instance_balanced_mask != target) &&
                    ProcessUtils::SetCpuAffinityMask(pid, target)) {
                    state.instance_balanced = true;
                    state.instance_balanced_mask = target;
                }
            }
        }

        for (auto iterator = tracked.begin(); iterator != tracked.end();) {
            if (!currentPids.contains(iterator->first)) {
                DisableCpuRate(iterator->second);
                iterator = tracked.erase(iterator);
            } else {
                ++iterator;
            }
        }

        bool qualifyingPerformanceProcess = false;
        for (const auto& [pid, state] : tracked) {
            if (!state.rules_applied || !state.rule_apply_succeeded) {
                continue;
            }
            const auto rule = config.FindRule(state.process_name);
            if (!rule || !rule->cpu_priority) {
                continue;
            }
            const bool high =
                EqualsIgnoreCase(*rule->cpu_priority, "High");
            const bool aboveNormalForeground =
                pid == foregroundPid &&
                EqualsIgnoreCase(*rule->cpu_priority, "Above Normal");
            if (high || aboveNormalForeground) {
                qualifyingPerformanceProcess = true;
                break;
            }
        }

        if (config.GetSettings().power_plan_enabled &&
            qualifyingPerformanceProcess &&
            !powerPlanActive) {
            if (!powerPlanReady) {
                powerPlanReady = ProcessUtils::CreateAndSetupCustomPowerPlan();
            }
            hasPreviousPowerPlan =
                ProcessUtils::GetActivePowerScheme(previousPowerPlan);
            if (powerPlanReady &&
                ProcessUtils::SetActivePowerScheme(OptimizerPowerPlan)) {
                powerPlanActive = true;
                for (const auto& [pid, state] : tracked) {
                    if (!state.rules_applied || !state.rule_apply_succeeded) {
                        continue;
                    }
                    const auto rule = config.FindRule(state.process_name);
                    if (!rule || !rule->launcher) {
                        continue;
                    }
                    UniqueHandle process(OpenProcess(
                        PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE,
                        pid));
                    if (process) {
                        ProcessUtils::TrimProcessMemory(process.get());
                    }
                }
            }
        } else if ((!qualifyingPerformanceProcess ||
                    !config.GetSettings().power_plan_enabled) &&
                   powerPlanActive) {
            if (hasPreviousPowerPlan) {
                ProcessUtils::SetActivePowerScheme(previousPowerPlan);
            }
            powerPlanActive = false;
        }

        for (const auto& rule : config.GetRules()) {
            if (!rule.keep_alive) {
                continue;
            }

            const std::string key = ToLower(rule.process_name);
            auto& state = watchdogs[key];
            if (runningNames.contains(key)) {
                state.consecutive_failures = 0;
                continue;
            }

            if (config.GetSettings().watchdog_max_retries > 0 &&
                state.consecutive_failures >=
                    config.GetSettings().watchdog_max_retries) {
                continue;
            }

            const int exponent = std::min(state.consecutive_failures, 6);
            const auto delay =
                std::chrono::seconds(std::min(300, 5 * (1 << exponent)));
            if (state.last_attempt.time_since_epoch().count() != 0 &&
                now - state.last_attempt < delay) {
                continue;
            }

            state.last_attempt = now;
            if (SpawnProcess(Utf8ToWide(rule.executable_path))) {
                state.consecutive_failures = 0;
                Logger::Info("[Watchdog] Restarted " + rule.process_name + ".");
            } else {
                ++state.consecutive_failures;
                Logger::Warn(
                    "[Watchdog] Failed to restart " + rule.process_name +
                    " (attempt " +
                    std::to_string(state.consecutive_failures) + ").");
            }
        }

        WriteEngineStatus(
            statusPath,
            true,
            config,
            foregroundPid,
            matchedProcesses,
            appliedProcesses,
            powerPlanActive,
            startedAt);

        const DWORD waitMilliseconds =
            static_cast<DWORD>(config.GetSettings().poll_interval_ms);
        if (g_stopEvent &&
            WaitForSingleObject(g_stopEvent, waitMilliseconds) ==
                WAIT_OBJECT_0) {
            break;
        }
        if (!g_stopEvent) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(waitMilliseconds));
        }
    }

    RestoreAll(tracked);
    if (powerPlanActive && hasPreviousPowerPlan) {
        ProcessUtils::SetActivePowerScheme(previousPowerPlan);
    }
    WriteEngineStatus(
        statusPath,
        false,
        config,
        0,
        0,
        0,
        false,
        startedAt);
    Logger::Info("WinAnanicy engine stopped and process baselines were restored.");
}

BOOL WINAPI ConsoleControlHandler(DWORD controlType) {
    if (controlType == CTRL_C_EVENT ||
        controlType == CTRL_BREAK_EVENT ||
        controlType == CTRL_CLOSE_EVENT ||
        controlType == CTRL_SHUTDOWN_EVENT) {
        g_running = false;
        if (g_stopEvent) {
            SetEvent(g_stopEvent);
        }
        return TRUE;
    }
    return FALSE;
}

void PrintHelp() {
    std::cout
        << "WinAnanicy " << WINANANICY_VERSION << "\n"
        << "Lightweight Windows process optimizer / Windows surec iyilestirici\n\n"
        << "Usage / Kullanim:\n"
        << "  win-ananicy.exe --run                 Console mode / Konsol modu\n"
        << "  win-ananicy.exe --background          Hidden engine / Gizli motor\n"
        << "  win-ananicy.exe --stop                Stop engine / Motoru durdur\n"
        << "  win-ananicy.exe --validate            Validate config / Ayarlari dogrula\n"
        << "  win-ananicy.exe --version             Show version / Surumu goster\n"
        << "  --config <path> --settings <path>     Override data paths\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    bool background = false;
    bool validateOnly = false;
    std::filesystem::path dataDirectory = GetDataDirectory();
    std::filesystem::path rulesPath = dataDirectory / L"rules.json";
    std::filesystem::path settingsPath = dataDirectory / L"settings.json";

    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--help" || argument == L"-h") {
            PrintHelp();
            return 0;
        }
        if (argument == L"--version") {
            std::cout << WINANANICY_VERSION << '\n';
            return 0;
        }
        if (argument == L"--stop") {
            UniqueHandle event(OpenEventW(
                EVENT_MODIFY_STATE,
                FALSE,
                EngineStopEventName));
            if (!event) {
                return 1;
            }
            return SetEvent(event.get()) ? 0 : 1;
        }
        if (argument == L"--background") {
            background = true;
            continue;
        }
        if (argument == L"--run") {
            continue;
        }
        if (argument == L"--validate") {
            validateOnly = true;
            continue;
        }
        if (argument == L"--config" && index + 1 < argc) {
            rulesPath = argv[++index];
            continue;
        }
        if (argument == L"--settings" && index + 1 < argc) {
            settingsPath = argv[++index];
            continue;
        }
        std::wcerr << L"Unknown argument / Bilinmeyen parametre: "
                   << argument << L'\n';
        return 2;
    }

    std::filesystem::path logDirectory = dataDirectory / L"logs";
    try {
        std::filesystem::create_directories(logDirectory);
    } catch (...) {
        logDirectory = GetExecutablePath().parent_path() / L"data" / L"logs";
        std::filesystem::create_directories(logDirectory);
    }

    Logger::Initialize((logDirectory / L"win-ananicy.log").wstring());

    if (validateOnly) {
        ConfigManager config(rulesPath, settingsPath);
        return config.Load() ? 0 : 1;
    }

    UniqueHandle mutex(CreateMutexW(nullptr, FALSE, EngineMutexName));
    if (!mutex) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Logger::Info("Another WinAnanicy engine instance is already running.");
        return 0;
    }

    UniqueHandle stopEvent(CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        EngineStopEventName));
    g_stopEvent = stopEvent.get();
    ResetEvent(g_stopEvent);
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);

    if (background) {
        if (HWND window = GetConsoleWindow()) {
            ShowWindow(window, SW_HIDE);
        }
    }

    MainLoop(
        rulesPath,
        settingsPath,
        dataDirectory / L"state.json");
    g_stopEvent = nullptr;
    return 0;
}
