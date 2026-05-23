#include "logger.hpp"
#include "config.hpp"
#include "process_utils.hpp"
#include <windows.h>
#include <tlhelp32.h>
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
};

// Main process optimization loop
void MainLoop(const std::filesystem::path& configPath) {
    Logger::Info("WinAnanicy core engine started.");

    // Enable security privileges for modifying process priorities
    ProcessUtils::EnableRequiredPrivileges();

    ConfigManager config(configPath);
    if (!config.Load()) {
        Logger::Warn("Could not load initial rules.json. The tool will wait for rules.json updates.");
    }

    // Power scheme management variables
    GUID originalPowerScheme;
    bool hasOriginalPowerScheme = false;
    bool isHighPerformanceActive = false;

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
            }
        }

        // 2. Fetch the active foreground process ID
        DWORD foregroundPid = 0;
        HWND hwndForeground = GetForegroundWindow();
        if (hwndForeground) {
            GetWindowThreadProcessId(hwndForeground, &foregroundPid);
        }

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

        do {
            DWORD pid = pe32.th32ProcessID;
            if (pid == 0) continue; // Skip idle process

            std::wstring wName = pe32.szExeFile;
            std::string name = WideToUtf8(wName);
            currentPids.insert(pid);

            auto ruleOpt = config.FindRule(name);
            if (ruleOpt.has_value()) {
                const auto& rule = ruleOpt.value();
                auto& state = trackedProcesses[pid];
                state.process_name = name;

                bool nowForeground = (pid == foregroundPid);

                if (rule.background_only) {
                    // Apply if rules haven't been applied yet, or if foreground focus transitioned
                    if (!state.rules_applied || state.is_foreground != nowForeground) {
                        HANDLE hProcess = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, pid);
                        if (nowForeground) {
                            Logger::Info("PID " + std::to_string(pid) + " (" + name + ") is in foreground. Restoring Normal CPU priority and disabling EcoQoS.");
                            ProcessUtils::SetCpuPriority(pid, "Normal");
                            if (hProcess) {
                                if (ProcessUtils::SetProcessEcoQoS(hProcess, false)) {
                                    Logger::Info("Disabled EcoQoS (Efficiency Mode) for PID " + std::to_string(pid) + " (" + name + ").");
                                }
                            }
                        } else {
                            Logger::Info("PID " + std::to_string(pid) + " (" + name + ") is in background. Adjusting priorities.");
                            if (rule.cpu_priority) ProcessUtils::SetCpuPriority(pid, *rule.cpu_priority);
                            if (rule.io_priority) ProcessUtils::SetIoPriority(pid, *rule.io_priority);
                            if (rule.cpu_affinity) ProcessUtils::SetCpuAffinity(pid, *rule.cpu_affinity);
                            if (hProcess) {
                                // Enable EcoQoS (either because background_only is active, or explicitly marked eco_qos)
                                if (ProcessUtils::SetProcessEcoQoS(hProcess, true)) {
                                    Logger::Info("Applied EcoQoS (Efficiency Mode) to background process PID " + std::to_string(pid) + " (" + name + ").");
                                }
                                if (rule.cpu_limit > 0 && rule.cpu_limit < 100) {
                                    if (ProcessUtils::LimitProcessCpuRate(hProcess, rule.cpu_limit)) {
                                        Logger::Info("Applied CPU rate limit (" + std::to_string(rule.cpu_limit) + "%) to background process PID " + std::to_string(pid) + " (" + name + ").");
                                    }
                                }
                            }
                        }
                        if (hProcess) {
                            CloseHandle(hProcess);
                        }
                        state.is_foreground = nowForeground;
                        state.rules_applied = true;
                    }
                } else {
                    // Standard rules: apply once
                    if (!state.rules_applied) {
                        Logger::Info("Applying rules to PID " + std::to_string(pid) + " (" + name + ").");
                        if (rule.cpu_priority) ProcessUtils::SetCpuPriority(pid, *rule.cpu_priority);
                        if (rule.io_priority) ProcessUtils::SetIoPriority(pid, *rule.io_priority);
                        if (rule.cpu_affinity) ProcessUtils::SetCpuAffinity(pid, *rule.cpu_affinity);
                        
                        HANDLE hProcess = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, pid);
                        if (hProcess) {
                            if (rule.eco_qos) {
                                if (ProcessUtils::SetProcessEcoQoS(hProcess, true)) {
                                    Logger::Info("Applied EcoQoS (Efficiency Mode) to PID " + std::to_string(pid) + " (" + name + ").");
                                }
                            }
                            if (rule.cpu_limit > 0 && rule.cpu_limit < 100) {
                                if (ProcessUtils::LimitProcessCpuRate(hProcess, rule.cpu_limit)) {
                                    Logger::Info("Applied CPU rate limit (" + std::to_string(rule.cpu_limit) + "%) to PID " + std::to_string(pid) + " (" + name + ").");
                                }
                            }
                            CloseHandle(hProcess);
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

        // 5. Check if any game (High or Realtime priority) is currently running and active
        bool anyHighPriorityActive = false;
        for (const auto& [pid, state] : trackedProcesses) {
            auto ruleOpt = config.FindRule(state.process_name);
            if (ruleOpt.has_value()) {
                const auto& rule = ruleOpt.value();
                bool isGame = rule.cpu_priority && (EqualsIgnoreCase(*rule.cpu_priority, "High") || EqualsIgnoreCase(*rule.cpu_priority, "Realtime"));
                if (isGame) {
                    if (rule.background_only) {
                        if (!state.is_foreground) {
                            anyHighPriorityActive = true;
                        }
                    } else {
                        anyHighPriorityActive = true;
                    }
                }
            }
        }

        // Handle power scheme and launcher memory trimming transitions
        if (anyHighPriorityActive && !isHighPerformanceActive) {
            // e9a42b02-d5df-448d-aa00-03f14749eb61
            const GUID GUID_ULTIMATE_PERFORMANCE = { 0xe9a42b02, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };
            // 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c
            const GUID GUID_HIGH_PERFORMANCE = { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };

            Logger::Info("Game session started (active high priority process detected).");
            if (ProcessUtils::SetActivePowerScheme(GUID_ULTIMATE_PERFORMANCE)) {
                Logger::Info("Switched active power plan to Ultimate Performance.");
            } else if (ProcessUtils::SetActivePowerScheme(GUID_HIGH_PERFORMANCE)) {
                Logger::Info("Ultimate Performance not available. Switched active power plan to High Performance.");
            } else {
                Logger::Error("Failed to set power plan to High or Ultimate Performance.");
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
                        if (ProcessUtils::TrimProcessMemory(hProcess)) {
                            Logger::Info("Trimmed working set memory for launcher process: " + state.process_name + " (PID " + std::to_string(pid) + ")");
                        } else {
                            Logger::Warn("Failed to trim working set memory for launcher: " + state.process_name + " (PID " + std::to_string(pid) + ")");
                        }
                        CloseHandle(hProcess);
                    }
                }
            }

            isHighPerformanceActive = true;
        } else if (!anyHighPriorityActive && isHighPerformanceActive) {
            Logger::Info("Game session ended (no active high priority processes).");
            if (hasOriginalPowerScheme) {
                if (ProcessUtils::SetActivePowerScheme(originalPowerScheme)) {
                    Logger::Info("Restored system's original power plan.");
                } else {
                    Logger::Error("Failed to restore original power plan.");
                }
            }
            isHighPerformanceActive = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    // Restore power plan scheme on exit
    if (isHighPerformanceActive && hasOriginalPowerScheme) {
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
