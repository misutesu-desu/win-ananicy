#include "logger.hpp"
#include "config.hpp"
#include "process_utils.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <string>
#include <atomic>
#include <thread>
#include <iostream>

#define SERVICE_NAME L"WinAnanicy"

std::atomic<bool> g_Running{true};
SERVICE_STATUS g_ServiceStatus = {0};
SERVICE_STATUS_HANDLE g_StatusHandle = nullptr;

// Helper to convert std::wstring to std::string (UTF-8)
std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return "";
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &strTo[0], size_needed, nullptr, nullptr);
    return strTo;
}

// Case-insensitive string comparison helper
bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char charA, char charB) {
        return std::tolower(static_cast<unsigned char>(charA)) == std::tolower(static_cast<unsigned char>(charB));
    });
}

// Helper to convert std::string to lowercase
std::string ToLower(std::string_view str) {
    std::string res(str);
    std::transform(res.begin(), res.end(), res.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return res;
}

// RAII Handle wrapper
struct HandleCloser {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;


// Retrieves the directory containing the current executable
std::wstring GetExecutableDirectory() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::filesystem::path p(buffer);
    return p.parent_path().wstring();
}

struct ProcessState {
    std::string process_name;
    bool rules_applied = false;
    bool is_foreground = false;

    // CPU affinity limiter tracking
    ULONGLONG last_kernel_time = 0;
    ULONGLONG last_user_time = 0;
    ULONGLONG last_system_time = 0;
    bool has_prev_times = false;
    int seconds_above_trigger = 0;
    int seconds_below_restore = 0;
    bool is_throttled = false;
    DWORD_PTR original_affinity_mask = 0;
    bool has_original_affinity = false;

    // Instance balancer tracking
    bool instance_balanced = false;
    DWORD_PTR instance_balanced_mask = 0;
};

// Helper to convert std::string (UTF-8) to std::wstring
std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (size_needed <= 0) return L"";
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &wstrTo[0], size_needed);
    return wstrTo;
}

// Spawns a process using CreateProcessW
bool SpawnProcess(const std::wstring& exePath) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    RtlZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    RtlZeroMemory(&pi, sizeof(pi));

    // CreateProcessW can modify the command line, so copy it
    std::wstring cmdLine = L"\"" + exePath + L"\"";
    std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back(L'\0');

    if (!CreateProcessW(
        nullptr,            // No module name
        cmdLineBuf.data(),  // Command line
        nullptr,            // Process handle not inheritable
        nullptr,            // Thread handle not inheritable
        FALSE,              // Set handle inheritance to FALSE
        0,                  // No creation flags
        nullptr,            // Use parent's environment block
        nullptr,            // Use parent's starting directory 
        &si,                // Pointer to STARTUPINFO structure
        &pi                 // Pointer to PROCESS_INFORMATION structure
    )) {
        Logger::Error("CreateProcessW failed for path: " + WideToUtf8(exePath) + ". Error: " + std::to_string(GetLastError()));
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// Main process optimization loop
void MainLoop(const std::filesystem::path& configPath) {
    Logger::Info("WinAnanicy core engine started.");

    // Enable security privileges for modifying process priorities
    ProcessUtils::EnableRequiredPrivileges();

    // Programmatically create/configure the WinAnanicy Energy Optimizer power plan
    ProcessUtils::CreateAndSetupCustomPowerPlan();

    ConfigManager config(configPath);
    if (!config.Load()) {
        Logger::Warn("Could not load initial rules.json. The tool will wait for rules.json updates.");
    }

    // Power scheme management variables
    GUID originalPowerScheme;
    bool hasOriginalPowerScheme = false;
    bool _wasPowerPlanSwitched = false;

    if (ProcessUtils::GetActivePowerScheme(originalPowerScheme)) {
        hasOriginalPowerScheme = true;
        Logger::Info("Successfully cached active system power plan GUID.");
    } else {
        Logger::Error("Failed to query initial active power scheme.");
    }

    std::unordered_map<DWORD, ProcessState> trackedProcesses;

    while (g_Running) {
        // 1. Hot reload check
        if (config.CheckAndReload()) {
            // Reset applied states so rules get re-evaluated with the new configuration
            for (auto& [pid, state] : trackedProcesses) {
                state.rules_applied = false;

                // Check if the new/updated rule still has CPU throttling. If not, and it was throttled, restore original affinity.
                auto newRuleOpt = config.FindRule(state.process_name);
                if (state.is_throttled) {
                    bool keepThrottling = false;
                    if (newRuleOpt.has_value()) {
                        const auto& nr = newRuleOpt.value();
                        if (nr.cpu_throttle_trigger_pct.has_value() && nr.cpu_throttle_duration_secs.has_value()) {
                            keepThrottling = true;
                        }
                    }
                    if (!keepThrottling) {
                        HANDLE hProcessSet = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
                        if (hProcessSet) {
                            UniqueHandle uhProcessSet(hProcessSet);
                            if (state.has_original_affinity) {
                                SetProcessAffinityMask(uhProcessSet.get(), state.original_affinity_mask);
                            }
                            Logger::Info("[CPU Limiter] Restored original affinity for PID " + std::to_string(pid) + " (" + state.process_name + ") because throttle rule was removed/changed on reload.");
                        }
                        state.is_throttled = false;
                        state.has_original_affinity = false;
                        state.seconds_above_trigger = 0;
                        state.seconds_below_restore = 0;
                    }
                }
            }
        }

        // 2. Fetch the active foreground process ID
        DWORD foregroundPid = 0;
        HWND hwndForeground = GetForegroundWindow();
        if (hwndForeground) {
            GetWindowThreadProcessId(hwndForeground, &foregroundPid);
        }

        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        DWORD numLogicalCores = sysInfo.dwNumberOfProcessors;

        // 3. Take a snapshot of all active processes
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnapshot == INVALID_HANDLE_VALUE) {
            Logger::Error("Failed to create process snapshot. Error: " + std::to_string(GetLastError()));
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);

        if (!Process32FirstW(hSnapshot, &pe32)) {
            CloseHandle(hSnapshot);
            Logger::Error("Failed to query first process from snapshot.");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        std::unordered_set<DWORD> currentPids;
        std::unordered_set<std::string> runningProcessNames;
        std::unordered_map<std::wstring, std::vector<DWORD>> balancedProcesses;

        do {
            DWORD pid = pe32.th32ProcessID;
            if (pid == 0) continue; // Skip idle process

            std::wstring wName = pe32.szExeFile;
            std::string name = WideToUtf8(wName);
            std::string lowerName = ToLower(name);
            currentPids.insert(pid);
            runningProcessNames.insert(lowerName);

            auto ruleOpt = config.FindRule(name);
            if (ruleOpt.has_value()) {
                const auto& rule = ruleOpt.value();

                // Feature 4: Disallowed / Blacklist
                if (rule.disallowed) {
                    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                    if (hProcess) {
                        UniqueHandle uhProcess(hProcess);
                        if (TerminateProcess(uhProcess.get(), 0)) {
                            Logger::Info("[INFO] Instantly terminated disallowed process: " + name + " (PID " + std::to_string(pid) + ")");
                        } else {
                            Logger::Warn("Failed to terminate disallowed process: " + name + " (PID " + std::to_string(pid) + "). Error: " + std::to_string(GetLastError()));
                        }
                    } else {
                        Logger::Warn("Failed to open disallowed process: " + name + " (PID " + std::to_string(pid) + ") for termination.");
                    }
                    continue; // Skip any further processing
                }

                auto& state = trackedProcesses[pid];
                state.process_name = name;

                // Feature 1: SmartTrim
                if (rule.smart_trim_threshold_mb.has_value()) {
                    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA, FALSE, pid);
                    if (hProcess) {
                        UniqueHandle uhProcess(hProcess);
                        PROCESS_MEMORY_COUNTERS pmc;
                        if (K32GetProcessMemoryInfo(uhProcess.get(), &pmc, sizeof(pmc))) {
                            SIZE_T workingSetMb = pmc.WorkingSetSize / (1024 * 1024);
                            if (workingSetMb > static_cast<SIZE_T>(rule.smart_trim_threshold_mb.value())) {
                                if (SetProcessWorkingSetSize(uhProcess.get(), (SIZE_T)-1, (SIZE_T)-1)) {
                                    Logger::Info("[SmartTrim] Trimmed memory for PID " + std::to_string(pid) + " (" + name + ") as working set (" + std::to_string(workingSetMb) + " MB) exceeded threshold (" + std::to_string(rule.smart_trim_threshold_mb.value()) + " MB).");
                                } else {
                                    Logger::Warn("[SmartTrim] Failed to trim memory for PID " + std::to_string(pid) + " (" + name + "). Error: " + std::to_string(GetLastError()));
                                }
                            }
                        }
                    }
                }

                // Feature 2: CPU Affinity Limiter
                if (rule.cpu_throttle_trigger_pct.has_value() && rule.cpu_throttle_duration_secs.has_value()) {
                    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                    if (hProcess) {
                        UniqueHandle uhProcess(hProcess);
                        FILETIME creationTime, exitTime, kernelTime, userTime;
                        if (GetProcessTimes(uhProcess.get(), &creationTime, &exitTime, &kernelTime, &userTime)) {
                            ULARGE_INTEGER kernelReal, userReal;
                            kernelReal.LowPart = kernelTime.dwLowDateTime;
                            kernelReal.HighPart = kernelTime.dwHighDateTime;
                            userReal.LowPart = userTime.dwLowDateTime;
                            userReal.HighPart = userTime.dwHighDateTime;

                            ULONGLONG currentKernel = kernelReal.QuadPart;
                            ULONGLONG currentUser = userReal.QuadPart;

                            FILETIME sysTime;
                            GetSystemTimeAsFileTime(&sysTime);
                            ULARGE_INTEGER sysReal;
                            sysReal.LowPart = sysTime.dwLowDateTime;
                            sysReal.HighPart = sysTime.dwHighDateTime;
                            ULONGLONG currentSystemTime = sysReal.QuadPart;

                            if (state.has_prev_times) {
                                ULONGLONG deltaProcess = (currentKernel - state.last_kernel_time) + (currentUser - state.last_user_time);
                                ULONGLONG deltaReal = currentSystemTime - state.last_system_time;

                                if (deltaReal > 0) {
                                    double cpuUsagePct = (double)deltaProcess / (double)(deltaReal * numLogicalCores) * 100.0;
                                    int triggerPct = rule.cpu_throttle_trigger_pct.value();
                                    int durationSecs = rule.cpu_throttle_duration_secs.value();

                                    if (cpuUsagePct >= triggerPct) {
                                        state.seconds_above_trigger++;
                                        state.seconds_below_restore = 0;

                                        if (state.seconds_above_trigger >= durationSecs && !state.is_throttled) {
                                            // Apply throttle
                                            HANDLE hProcessSet = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid);
                                            if (hProcessSet) {
                                                UniqueHandle uhProcessSet(hProcessSet);
                                                DWORD_PTR processAffinity = 0;
                                                DWORD_PTR systemAffinity = 0;
                                                if (GetProcessAffinityMask(uhProcessSet.get(), &processAffinity, &systemAffinity)) {
                                                    state.original_affinity_mask = processAffinity;
                                                    state.has_original_affinity = true;

                                                    DWORD_PTR restrictedMask = 0;
                                                    if (numLogicalCores > 1) {
                                                        restrictedMask = (static_cast<DWORD_PTR>(1) << (numLogicalCores - 1)) | (static_cast<DWORD_PTR>(1) << (numLogicalCores - 2));
                                                    } else {
                                                        restrictedMask = 0x01;
                                                    }

                                                    if (SetProcessAffinityMask(uhProcessSet.get(), restrictedMask)) {
                                                        state.is_throttled = true;
                                                        Logger::Info("[CPU Limiter] Throttled PID " + std::to_string(pid) + " (" + name + ") due to high CPU usage (" + std::to_string(cpuUsagePct) + "%). Restricting affinity to last cores.");
                                                    }
                                                }
                                            }
                                        }
                                    } else {
                                        state.seconds_above_trigger = 0;

                                        if (state.is_throttled) {
                                            if (cpuUsagePct < (triggerPct / 2.0)) {
                                                state.seconds_below_restore++;
                                                if (state.seconds_below_restore >= 5) {
                                                    // Restore affinity
                                                    HANDLE hProcessSet = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
                                                    if (hProcessSet) {
                                                        UniqueHandle uhProcessSet(hProcessSet);
                                                        if (state.has_original_affinity) {
                                                            if (SetProcessAffinityMask(uhProcessSet.get(), state.original_affinity_mask)) {
                                                                Logger::Info("[CPU Limiter] Restored original affinity for PID " + std::to_string(pid) + " (" + name + ") after CPU usage dropped to " + std::to_string(cpuUsagePct) + "%.");
                                                                state.is_throttled = false;
                                                                state.seconds_below_restore = 0;
                                                            }
                                                        } else {
                                                            DWORD_PTR processAffinity = 0;
                                                            DWORD_PTR systemAffinity = 0;
                                                            if (GetProcessAffinityMask(uhProcessSet.get(), &processAffinity, &systemAffinity)) {
                                                                if (SetProcessAffinityMask(uhProcessSet.get(), systemAffinity)) {
                                                                    Logger::Info("[CPU Limiter] Restored system affinity for PID " + std::to_string(pid) + " (" + name + ").");
                                                                    state.is_throttled = false;
                                                                    state.seconds_below_restore = 0;
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            } else {
                                                state.seconds_below_restore = 0;
                                            }
                                        }
                                    }
                                }
                            }
                            state.last_kernel_time = currentKernel;
                            state.last_user_time = currentUser;
                            state.last_system_time = currentSystemTime;
                            state.has_prev_times = true;
                        }
                    }
                }

                // Feature 3: Instance Balancer
                if (rule.instance_balance) {
                    balancedProcesses[wName].push_back(pid);
                }

                // Standard optimization rules (priority, EcoQoS, limit)
                bool nowForeground = (pid == foregroundPid);

                if (rule.background_only) {
                    if (!state.rules_applied || state.is_foreground != nowForeground) {
                        HANDLE hProcessRaw = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, pid);
                        UniqueHandle hProcess(hProcessRaw);
                        if (nowForeground) {
                            Logger::Info("PID " + std::to_string(pid) + " (" + name + ") is in foreground. Restoring Normal CPU priority and disabling EcoQoS.");
                            ProcessUtils::SetCpuPriority(pid, "Normal");
                            if (hProcess) {
                                if (ProcessUtils::SetProcessEcoQoS(hProcess.get(), false)) {
                                    Logger::Info("Disabled EcoQoS (Efficiency Mode) for PID " + std::to_string(pid) + " (" + name + ").");
                                }
                            }
                        } else {
                            Logger::Info("PID " + std::to_string(pid) + " (" + name + ") is in background. Adjusting priorities.");
                            if (rule.cpu_priority) ProcessUtils::SetCpuPriority(pid, *rule.cpu_priority);
                            if (rule.io_priority) ProcessUtils::SetIoPriority(pid, *rule.io_priority);
                            if (rule.cpu_affinity && !rule.instance_balance) ProcessUtils::SetCpuAffinity(pid, *rule.cpu_affinity);
                            if (hProcess) {
                                if (ProcessUtils::SetProcessEcoQoS(hProcess.get(), true)) {
                                    Logger::Info("Applied EcoQoS (Efficiency Mode) to background process PID " + std::to_string(pid) + " (" + name + ").");
                                }
                                if (rule.cpu_limit > 0 && rule.cpu_limit < 100) {
                                    if (ProcessUtils::LimitProcessCpuRate(hProcess.get(), rule.cpu_limit)) {
                                        Logger::Info("Applied CPU rate limit (" + std::to_string(rule.cpu_limit) + "%) to background process PID " + std::to_string(pid) + " (" + name + ").");
                                    }
                                }
                            }
                        }
                        state.is_foreground = nowForeground;
                        state.rules_applied = true;
                    }
                } else {
                    if (!state.rules_applied) {
                        Logger::Info("Applying rules to PID " + std::to_string(pid) + " (" + name + ").");
                        if (rule.cpu_priority) ProcessUtils::SetCpuPriority(pid, *rule.cpu_priority);
                        if (rule.io_priority) ProcessUtils::SetIoPriority(pid, *rule.io_priority);
                        if (rule.cpu_affinity && !rule.instance_balance) ProcessUtils::SetCpuAffinity(pid, *rule.cpu_affinity);
                        
                        HANDLE hProcessRaw = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, pid);
                        UniqueHandle hProcess(hProcessRaw);
                        if (hProcess) {
                            if (rule.eco_qos) {
                                if (ProcessUtils::SetProcessEcoQoS(hProcess.get(), true)) {
                                    Logger::Info("Applied EcoQoS (Efficiency Mode) to PID " + std::to_string(pid) + " (" + name + ").");
                                }
                            }
                            if (rule.cpu_limit > 0 && rule.cpu_limit < 100) {
                                if (ProcessUtils::LimitProcessCpuRate(hProcess.get(), rule.cpu_limit)) {
                                    Logger::Info("Applied CPU rate limit (" + std::to_string(rule.cpu_limit) + "%) to PID " + std::to_string(pid) + " (" + name + ").");
                                }
                            }
                        }
                        state.rules_applied = true;
                    }
                }
            }
        } while (Process32NextW(hSnapshot, &pe32));

        CloseHandle(hSnapshot);

        // 4. Remove exited PIDs from our tracking table
        for (auto it = trackedProcesses.begin(); it != trackedProcesses.end();) {
            if (currentPids.find(it->first) == currentPids.end()) {
                Logger::Debug("Process exited: PID " + std::to_string(it->first) + " (" + it->second.process_name + ")");
                it = trackedProcesses.erase(it);
            } else {
                ++it;
            }
        }

        // Feature 3: Apply cyclic affinity masks to balanced processes
        for (auto& [wExeName, pids] : balancedProcesses) {
            if (pids.empty()) continue;
            std::sort(pids.begin(), pids.end());
            std::string exeName = WideToUtf8(wExeName);

            for (size_t i = 0; i < pids.size(); ++i) {
                DWORD pid = pids[i];
                DWORD_PTR targetMask = static_cast<DWORD_PTR>(1) << (i % numLogicalCores);

                auto& state = trackedProcesses[pid];
                if (!state.instance_balanced || state.instance_balanced_mask != targetMask) {
                    if (ProcessUtils::SetCpuAffinityMask(pid, targetMask)) {
                        state.instance_balanced = true;
                        state.instance_balanced_mask = targetMask;
                        Logger::Info("[Instance Balancer] Assigned PID " + std::to_string(pid) + " (" + exeName + ") Instance " + std::to_string(i) + " to Core Mask 0x" + std::to_string(targetMask));
                    }
                }
            }
        }

        // 5. Check if any qualifying high-priority process is active
        bool anyQualifyingActive = false;
        std::string triggeringProcessName = "";
        DWORD triggeringProcessPid = 0;
        bool triggeredByForeground = false;

        for (const auto& [pid, state] : trackedProcesses) {
            auto ruleOpt = config.FindRule(state.process_name);
            if (ruleOpt.has_value()) {
                const auto& rule = ruleOpt.value();
                if (rule.cpu_priority) {
                    const std::string& priority = *rule.cpu_priority;
                    bool isHighOrRealtime = EqualsIgnoreCase(priority, "High") || EqualsIgnoreCase(priority, "Realtime");
                    bool isAboveNormal = EqualsIgnoreCase(priority, "Above Normal");
                    
                    bool isForeground = (pid == foregroundPid);
                    
                    // Condition A: running, in foreground (has focus), and priority is High, Realtime, or Above Normal
                    if (isForeground && (isHighOrRealtime || isAboveNormal)) {
                        anyQualifyingActive = true;
                        triggeringProcessName = state.process_name;
                        triggeringProcessPid = pid;
                        triggeredByForeground = true;
                        break; // Foreground match is high priority, stop searching
                    }
                    
                    // Condition B: configured with High or Realtime, and background_only is false
                    if (isHighOrRealtime && !rule.background_only) {
                        anyQualifyingActive = true;
                        triggeringProcessName = state.process_name;
                        triggeringProcessPid = pid;
                        triggeredByForeground = false;
                        // Continue loop in case we find a foreground process (Condition A) which is more descriptive to log
                    }
                }
            }
        }

        // Handle power scheme and launcher memory trimming transitions
        if (anyQualifyingActive && !_wasPowerPlanSwitched) {
            // a7bc678d-d5df-448d-aa00-03f14749eb61
            const GUID GUID_WINANANICY_OPTIMIZER = { 0xa7bc678d, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };
            // 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c
            const GUID GUID_HIGH_PERFORMANCE = { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };

            if (triggeredByForeground) {
                Logger::Info("High priority process [" + triggeringProcessName + "] (PID " + std::to_string(triggeringProcessPid) + ") detected in foreground. Switching to WinAnanicy Energy Optimizer.");
            } else {
                Logger::Info("High priority process [" + triggeringProcessName + "] (PID " + std::to_string(triggeringProcessPid) + ") detected (background_only = false). Switching to WinAnanicy Energy Optimizer.");
            }

            if (ProcessUtils::SetActivePowerScheme(GUID_WINANANICY_OPTIMIZER)) {
                Logger::Info("Switched active power plan to WinAnanicy Energy Optimizer.");
            } else if (ProcessUtils::SetActivePowerScheme(GUID_HIGH_PERFORMANCE)) {
                Logger::Info("WinAnanicy Energy Optimizer not available. Switched active power plan to High Performance.");
            } else {
                Logger::Error("Failed to set power plan to WinAnanicy Energy Optimizer or High Performance.");
            }

            // Trim launchers memory
            Logger::Info("Trimming working sets of launcher processes...");
            for (const auto& [pid, state] : trackedProcesses) {
                auto ruleOpt = config.FindRule(state.process_name);
                if (ruleOpt.has_value() && ruleOpt.value().launcher) {
                    HANDLE hProcess = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                    if (!hProcess) {
                        hProcess = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
                    }
                    if (hProcess) {
                        UniqueHandle uhProcess(hProcess);
                        if (ProcessUtils::TrimProcessMemory(uhProcess.get())) {
                            Logger::Info("Trimmed working set memory for launcher process: " + state.process_name + " (PID " + std::to_string(pid) + ")");
                        } else {
                            Logger::Warn("Failed to trim working set memory for launcher: " + state.process_name + " (PID " + std::to_string(pid) + ")");
                        }
                    }
                }
            }

            _wasPowerPlanSwitched = true;
        } else if (!anyQualifyingActive && _wasPowerPlanSwitched) {
            Logger::Info("Optimization session ended (no active qualifying high priority processes).");
            if (hasOriginalPowerScheme) {
                if (ProcessUtils::SetActivePowerScheme(originalPowerScheme)) {
                    Logger::Info("Restored system's original power plan.");
                } else {
                    Logger::Error("Failed to restore original power plan.");
                }
            }
            _wasPowerPlanSwitched = false;
        }

        // Feature 5: Watchdog / Keep Alive
        static std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastLaunchTimes;
        auto nowTime = std::chrono::steady_clock::now();

        for (const auto& rule : config.GetRules()) {
            if (rule.keep_alive && !rule.executable_path.empty()) {
                std::string lowerName = ToLower(rule.process_name);
                bool isRunning = (runningProcessNames.find(lowerName) != runningProcessNames.end());

                if (!isRunning) {
                    auto it = lastLaunchTimes.find(lowerName);
                    bool cooldownElapsed = true;
                    if (it != lastLaunchTimes.end()) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(nowTime - it->second).count();
                        if (elapsed < 10) {
                            cooldownElapsed = false;
                        }
                    }

                    if (cooldownElapsed) {
                        Logger::Info("[Watchdog] Process " + rule.process_name + " is not running. Resurrecting from path: " + rule.executable_path);
                        std::wstring wExePath = Utf8ToWide(rule.executable_path);
                        if (SpawnProcess(wExePath)) {
                            lastLaunchTimes[lowerName] = nowTime;
                        } else {
                            Logger::Error("[Watchdog] Failed to launch process " + rule.process_name + " from path: " + rule.executable_path);
                            lastLaunchTimes[lowerName] = nowTime; // Prevent rapid retries on launch failure
                        }
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    // Restore power plan scheme on exit
    if (_wasPowerPlanSwitched && hasOriginalPowerScheme) {
        ProcessUtils::SetActivePowerScheme(originalPowerScheme);
        Logger::Info("Daemon shutting down. Restored original power plan.");
    }

    Logger::Info("WinAnanicy core engine stopped.");
}

// Handler for console Ctrl+C / close events
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_CLOSE_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
        Logger::Info("Console termination event received. Stopping...");
        g_Running = false;
        return TRUE;
    }
    return FALSE;
}

// Windows Service installation functions
bool InstallService() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        std::wcerr << L"Failed to obtain binary path. Error: " << GetLastError() << std::endl;
        return false;
    }

    SC_HANDLE schSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!schSCManager) {
        std::wcerr << L"Failed to open SCM (Service Control Manager). Run as Administrator. Error: " << GetLastError() << std::endl;
        return false;
    }

    SC_HANDLE schService = CreateServiceW(
        schSCManager,
        SERVICE_NAME,
        SERVICE_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        path,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr
    );

    if (!schService) {
        std::wcerr << L"Failed to create service. Error: " << GetLastError() << std::endl;
        CloseServiceHandle(schSCManager);
        return false;
    }

    std::wcout << L"WinAnanicy service installed successfully! Start type set to Automatic." << std::endl;
    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return true;
}

bool UninstallService() {
    SC_HANDLE schSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!schSCManager) {
        std::wcerr << L"Failed to open SCM. Run as Administrator. Error: " << GetLastError() << std::endl;
        return false;
    }

    SC_HANDLE schService = OpenServiceW(schSCManager, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (!schService) {
        std::wcerr << L"Failed to locate service. Error: " << GetLastError() << std::endl;
        CloseServiceHandle(schSCManager);
        return false;
    }

    // Attempt to stop service if it's currently running
    SERVICE_STATUS status;
    ControlService(schService, SERVICE_CONTROL_STOP, &status);

    if (!DeleteService(schService)) {
        std::wcerr << L"Failed to uninstall service. Error: " << GetLastError() << std::endl;
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return false;
    }

    // Delete the custom power plan from the Windows subsystem during cleanup/uninstall
    ProcessUtils::DeleteCustomPowerPlan();

    std::wcout << L"WinAnanicy service uninstalled successfully." << std::endl;
    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return true;
}

// Windows Service control handler callback
VOID WINAPI ServiceCtrlHandler(DWORD request) {
    switch (request) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            Logger::Info("Service stop/shutdown requested.");
            g_Running = false;
            g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
            break;
        default:
            break;
    }
}

// Windows Service main function callback
VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    (void)argc; (void)argv;
    g_StatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (!g_StatusHandle) {
        return;
    }

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 0;

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    // Set paths relative to executable location in service mode
    std::filesystem::path exeDir = GetExecutableDirectory();
    Logger::Initialize(exeDir / L"win-ananicy.log");

    Logger::Info("WinAnanicy service started from SCM.");

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    MainLoop(exeDir / L"rules.json");

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

int main(int argc, char* argv[]) {
    // 1. Process command line installation parameters
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--install") {
            return InstallService() ? 0 : 1;
        } else if (arg == "--uninstall") {
            return UninstallService() ? 0 : 1;
        } else if (arg == "--run" || arg == "--background") {
            if (arg == "--background") {
                // Hide the console window for background runs
                HWND hwnd = GetConsoleWindow();
                if (hwnd) ShowWindow(hwnd, SW_HIDE);
            }
            std::filesystem::path exeDir = GetExecutableDirectory();
            Logger::Initialize(exeDir / L"win-ananicy.log");
            Logger::Info("WinAnanicy launched in manual CLI mode.");
            SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
            MainLoop(exeDir / L"rules.json");
            return 0;
        } else {
            std::cout << "WinAnanicy - Lightweight Windows Process Optimizer\n\n";
            std::cout << "Usage:\n";
            std::cout << "  win-ananicy.exe             - Run as Windows Service (when started by SCM)\n";
            std::cout << "  win-ananicy.exe --run       - Run in console (shows console window)\n";
            std::cout << "  win-ananicy.exe --background- Run in background (hides console window)\n";
            std::cout << "  win-ananicy.exe --install   - Install as a Windows Service (requires Admin)\n";
            std::cout << "  win-ananicy.exe --uninstall - Stop and remove Windows Service (requires Admin)\n";
            return 0;
        }
    }

    // 2. Default: Attempt to start as a Windows Service (SCM dispatch)
    SERVICE_TABLE_ENTRYW ServiceTable[] = {
        { const_cast<LPWSTR>(SERVICE_NAME), static_cast<LPSERVICE_MAIN_FUNCTIONW>(ServiceMain) },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(ServiceTable)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            // Started manually from shell, fallback to console run
            std::filesystem::path exeDir = GetExecutableDirectory();
            Logger::Initialize(exeDir / L"win-ananicy.log");
            Logger::Info("WinAnanicy running in console fallback mode (no args provided).");
            SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
            MainLoop(exeDir / L"rules.json");
        } else {
            std::cerr << "Failed to dispatcher services. Error: " << err << std::endl;
            return 1;
        }
    }

    return 0;
}
