#include "process_utils.hpp"
#include "logger.hpp"
#include <sstream>
#include <vector>
#include <algorithm>
#include <memory>
#include <psapi.h>
#include <powrprof.h>

namespace ProcessUtils {

namespace {
    // Custom RAII closer for process handles
    struct HandleCloser {
        void operator()(HANDLE h) const {
            if (h && h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
            }
        }
    };
    using UniqueHandle = std::unique_ptr<void, HandleCloser>;

    // Helper to set a specific privilege on our process token
    bool SetTokenPrivilege(HANDLE hToken, LPCWSTR lpszPrivilege, bool bEnable) {
        TOKEN_PRIVILEGES tp;
        LUID luid;

        if (!LookupPrivilegeValueW(nullptr, lpszPrivilege, &luid)) {
            return false;
        }

        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = bEnable ? SE_PRIVILEGE_ENABLED : 0;

        if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr)) {
            return false;
        }

        return GetLastError() != ERROR_NOT_ALL_ASSIGNED;
    }

    // Case-insensitive string comparison helper
    bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char charA, char charB) {
            return std::tolower(static_cast<unsigned char>(charA)) == std::tolower(static_cast<unsigned char>(charB));
        });
    }

    // Helper to parse the affinity string
    DWORD_PTR ParseAffinity(const std::string& str) {
        if (str.empty()) return 0;

        // 1. Hex Bitmask (e.g., "0x0F" or "0Xf")
        if (str.rfind("0x", 0) == 0 || str.rfind("0X", 0) == 0) {
            try {
                return static_cast<DWORD_PTR>(std::stoull(str, nullptr, 16));
            } catch (...) {
                Logger::Warn("Failed to parse hex affinity: " + str);
                return 0;
            }
        }

        // 2. Comma-separated Core List (e.g., "0,1,2,3")
        if (str.find(',') != std::string::npos) {
            DWORD_PTR mask = 0;
            std::stringstream ss(str);
            std::string token;
            while (std::getline(ss, token, ',')) {
                try {
                    int core = std::stoi(token);
                    if (core >= 0 && core < static_cast<int>(sizeof(DWORD_PTR) * 8)) {
                        mask |= (static_cast<DWORD_PTR>(1) << core);
                    } else {
                        Logger::Warn("Core index out of bounds: " + token);
                    }
                } catch (...) {
                    Logger::Warn("Invalid core token: " + token);
                }
            }
            return mask;
        }

        // 3. Single number
        try {
            unsigned long long val = std::stoull(str);
            if (str.length() == 1 && val < 10) {
                // Treated as single core index (e.g., "4" -> Core 4)
                return (static_cast<DWORD_PTR>(1) << val);
            } else {
                // Treated as decimal bitmask (e.g., "15" -> Cores 0,1,2,3)
                return static_cast<DWORD_PTR>(val);
            }
        } catch (...) {
            Logger::Warn("Failed to parse single-value affinity: " + str);
            return 0;
        }
    }
}

bool EnableRequiredPrivileges() {
    HANDLE hTokenRaw;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hTokenRaw)) {
        Logger::Error("Failed to open current process token to adjust privileges.");
        return false;
    }
    UniqueHandle hToken(hTokenRaw);

    bool success = true;

    // SeDebugPrivilege allows us to open handles to processes owned by other users (including system accounts)
    if (SetTokenPrivilege(hToken.get(), L"SeDebugPrivilege", true)) {
        Logger::Debug("Successfully enabled SeDebugPrivilege.");
    } else {
        Logger::Warn("Failed to enable SeDebugPrivilege. May not be able to adjust elevated processes.");
        success = false;
    }

    // SeIncreaseBasePriorityPrivilege allows us to set REALTIME_PRIORITY_CLASS
    if (SetTokenPrivilege(hToken.get(), L"SeIncreaseBasePriorityPrivilege", true)) {
        Logger::Debug("Successfully enabled SeIncreaseBasePriorityPrivilege.");
    } else {
        Logger::Warn("Failed to enable SeIncreaseBasePriorityPrivilege. Realtime priority may fail.");
        success = false;
    }

    return success;
}

bool SetCpuPriority(DWORD pid, const std::string& priorityClassStr) {
    DWORD dwPriorityClass = NORMAL_PRIORITY_CLASS;

    if (EqualsIgnoreCase(priorityClassStr, "Idle")) {
        dwPriorityClass = IDLE_PRIORITY_CLASS;
    } else if (EqualsIgnoreCase(priorityClassStr, "Below Normal")) {
        dwPriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
    } else if (EqualsIgnoreCase(priorityClassStr, "Normal")) {
        dwPriorityClass = NORMAL_PRIORITY_CLASS;
    } else if (EqualsIgnoreCase(priorityClassStr, "Above Normal")) {
        dwPriorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
    } else if (EqualsIgnoreCase(priorityClassStr, "High")) {
        dwPriorityClass = HIGH_PRIORITY_CLASS;
    } else if (EqualsIgnoreCase(priorityClassStr, "Realtime")) {
        dwPriorityClass = REALTIME_PRIORITY_CLASS;
    } else {
        Logger::Warn("Unknown CPU priority class string: " + priorityClassStr + ". Using Normal.");
        return false;
    }

    HANDLE hProcessRaw = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!hProcessRaw) {
        Logger::Warn("Failed to open process (PID " + std::to_string(pid) + ") with PROCESS_SET_INFORMATION for CPU priority.");
        return false;
    }
    UniqueHandle hProcess(hProcessRaw);

    if (!SetPriorityClass(hProcess.get(), dwPriorityClass)) {
        DWORD err = GetLastError();
        // If Realtime failed, try to fallback to High
        if (dwPriorityClass == REALTIME_PRIORITY_CLASS && err == ERROR_PRIVILEGE_NOT_HELD) {
            Logger::Warn("Realtime priority denied for PID " + std::to_string(pid) + ". Falling back to High priority.");
            if (SetPriorityClass(hProcess.get(), HIGH_PRIORITY_CLASS)) {
                return true;
            }
        }
        Logger::Error("Failed to set CPU Priority Class for PID " + std::to_string(pid) + ". Error: " + std::to_string(err));
        return false;
    }

    Logger::Info("Applied CPU priority (" + priorityClassStr + ") to PID " + std::to_string(pid));
    return true;
}

bool SetCpuAffinity(DWORD pid, const std::string& affinityStr) {
    DWORD_PTR affinityMask = ParseAffinity(affinityStr);
    if (affinityMask == 0) {
        Logger::Error("Affinity mask resolved to 0 for string: " + affinityStr + ". Skipping.");
        return false;
    }

    HANDLE hProcessRaw = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcessRaw) {
        Logger::Warn("Failed to open process (PID " + std::to_string(pid) + ") with PROCESS_SET_INFORMATION for CPU affinity.");
        return false;
    }
    UniqueHandle hProcess(hProcessRaw);

    // Validate system affinity mask to prevent setting cores the process is not allowed to run on
    DWORD_PTR processAffinity = 0;
    DWORD_PTR systemAffinity = 0;
    if (GetProcessAffinityMask(hProcess.get(), &processAffinity, &systemAffinity)) {
        // Intersect requested affinity with system affinity
        DWORD_PTR finalMask = affinityMask & systemAffinity;
        if (finalMask == 0) {
            Logger::Warn("Requested affinity mask " + std::to_string(affinityMask) + " has no overlap with system affinity " + std::to_string(systemAffinity) + ". Using system mask.");
            finalMask = systemAffinity;
        }

        if (!SetProcessAffinityMask(hProcess.get(), finalMask)) {
            Logger::Error("Failed to set CPU Affinity Mask for PID " + std::to_string(pid) + ". Error: " + std::to_string(GetLastError()));
            return false;
        }
    } else {
        // Fallback to setting directly if we can't query
        if (!SetProcessAffinityMask(hProcess.get(), affinityMask)) {
            Logger::Error("Failed to set CPU Affinity Mask for PID " + std::to_string(pid) + " (fallback). Error: " + std::to_string(GetLastError()));
            return false;
        }
    }

    Logger::Info("Applied CPU affinity mask (" + affinityStr + ") to PID " + std::to_string(pid));
    return true;
}

typedef NTSTATUS(NTAPI* pfnNtSetInformationProcess)(
    HANDLE ProcessHandle,
    ULONG ProcessInformationClass, // 0x21 for ProcessIoPriority
    PVOID ProcessInformation,
    ULONG ProcessInformationLength
);

bool SetIoPriority(DWORD pid, const std::string& ioPriorityStr) {
    ULONG ioPriority = 2; // Default to Normal (2)

    if (EqualsIgnoreCase(ioPriorityStr, "Very Low")) {
        ioPriority = 0;
    } else if (EqualsIgnoreCase(ioPriorityStr, "Low")) {
        ioPriority = 1;
    } else if (EqualsIgnoreCase(ioPriorityStr, "Normal")) {
        ioPriority = 2;
    } else if (EqualsIgnoreCase(ioPriorityStr, "High")) {
        ioPriority = 3;
    } else {
        Logger::Warn("Unknown I/O priority class string: " + ioPriorityStr + ". Using Normal.");
        return false;
    }

    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) {
        hNtdll = LoadLibraryW(L"ntdll.dll");
    }
    if (!hNtdll) {
        Logger::Error("Failed to load ntdll.dll. Cannot set I/O priority.");
        return false;
    }

    auto NtSetInformationProcess = reinterpret_cast<pfnNtSetInformationProcess>(
        GetProcAddress(hNtdll, "NtSetInformationProcess")
    );
    if (!NtSetInformationProcess) {
        Logger::Error("Failed to locate NtSetInformationProcess in ntdll.dll.");
        return false;
    }

    HANDLE hProcessRaw = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!hProcessRaw) {
        Logger::Warn("Failed to open process (PID " + std::to_string(pid) + ") with PROCESS_SET_INFORMATION for I/O priority.");
        return false;
    }
    UniqueHandle hProcess(hProcessRaw);

    // 0x21 is the undocumented ProcessIoPriority value
    NTSTATUS status = NtSetInformationProcess(hProcess.get(), 0x21, &ioPriority, sizeof(ioPriority));
    if (status < 0) { // NT_SUCCESS macro is status >= 0
        Logger::Error("NtSetInformationProcess failed with status code " + std::to_string(status) + " for PID " + std::to_string(pid));
        return false;
    }

    Logger::Info("Applied I/O priority (" + ioPriorityStr + ") to PID " + std::to_string(pid));
    return true;
}

bool SetProcessEcoQoS(HANDLE hProcess, bool enable) {
    PROCESS_POWER_THROTTLING_STATE PowerThrottling;
    RtlZeroMemory(&PowerThrottling, sizeof(PowerThrottling));
    PowerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;

    if (enable) {
        PowerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        PowerThrottling.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    } else {
        PowerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        PowerThrottling.StateMask = 0;
    }

    if (!SetProcessInformation(hProcess, ProcessPowerThrottling, &PowerThrottling, sizeof(PowerThrottling))) {
        Logger::Error("Failed to set EcoQoS/PowerThrottling. Error: " + std::to_string(GetLastError()));
        return false;
    }
    return true;
}

bool TrimProcessMemory(HANDLE hProcess) {
    if (!EmptyWorkingSet(hProcess)) {
        return false;
    }
    return true;
}

bool LimitProcessCpuRate(HANDLE hProcess, DWORD limitPercentage) {
    if (limitPercentage == 0 || limitPercentage >= 100) {
        return false;
    }

    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (!hJob) {
        Logger::Error("Failed to create job object for CPU limiting. Error: " + std::to_string(GetLastError()));
        return false;
    }

    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpuRateInfo;
    RtlZeroMemory(&cpuRateInfo, sizeof(cpuRateInfo));
    cpuRateInfo.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    cpuRateInfo.CpuRate = limitPercentage * 100;

    if (!SetInformationJobObject(hJob, JobObjectCpuRateControlInformation, &cpuRateInfo, sizeof(cpuRateInfo))) {
        Logger::Error("Failed to set Job Object CPU rate control information. Error: " + std::to_string(GetLastError()));
        CloseHandle(hJob);
        return false;
    }

    if (!AssignProcessToJobObject(hJob, hProcess)) {
        DWORD err = GetLastError();
        Logger::Warn("Failed to assign process to Job Object. Error: " + std::to_string(err));
        CloseHandle(hJob);
        return false;
    }

    CloseHandle(hJob);
    return true;
}

bool SetActivePowerScheme(const GUID& schemeGuid) {
    if (PowerSetActiveScheme(nullptr, &schemeGuid) != ERROR_SUCCESS) {
        return false;
    }
    return true;
}

bool GetActivePowerScheme(GUID& schemeGuid) {
    GUID* pScheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &pScheme) == ERROR_SUCCESS) {
        schemeGuid = *pScheme;
        LocalFree(pScheme);
        return true;
    }
    return false;
}

} // namespace ProcessUtils
