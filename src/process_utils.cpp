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

bool CreateAndSetupCustomPowerPlan() {
    const GUID GUID_BALANCED = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
    const GUID GUID_WINANANICY_OPTIMIZER = { 0xa7bc678d, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };

    // Subgroups
    const GUID GUID_PROCESSOR_SUBGROUP = { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
    const GUID GUID_GRAPHICS_SUBGROUP_LOCAL = { 0x5fb4938d, 0x1ee8, 0x4b0f, { 0x9a, 0x3c, 0x50, 0x36, 0xb0, 0xab, 0x99, 0x5c } };
    const GUID GUID_INTEL_GRAPHICS_SUBGROUP = { 0x44f3beca, 0xa7c0, 0x460e, { 0x9d, 0xf2, 0xbb, 0x8b, 0x99, 0xe0, 0xcb, 0xa6 } };

    // Settings
    const GUID GUID_PROCESSOR_MIN_STATE = { 0x893dee8e, 0x2bef, 0x41e0, { 0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c } };
    const GUID GUID_PROCESSOR_MAX_STATE = { 0xbc5038f7, 0x23e0, 0x4960, { 0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec } };
    const GUID GUID_PROCESSOR_BOOST_MODE = { 0xbe337238, 0x0d82, 0x4146, { 0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7 } };
    const GUID GUID_GPU_PREFERENCE = { 0xdd848b2a, 0x8a5d, 0x4451, { 0x9a, 0xe2, 0x39, 0xcd, 0x41, 0x65, 0x8f, 0x6c } };
    const GUID GUID_INTEL_GRAPHICS_PLAN = { 0x3619c3f2, 0xafb2, 0x4afc, { 0xb0, 0xe9, 0xe7, 0xfe, 0xf3, 0x72, 0xde, 0x36 } };

    // 1. Check if the plan already exists (either by GUID or by name)
    bool planExists = false;
    
    // Quick check by reading name from our target GUID
    wchar_t quickCheckName[256] = {0};
    DWORD quickCheckNameSize = sizeof(quickCheckName);
    if (PowerReadFriendlyName(nullptr, &GUID_WINANANICY_OPTIMIZER, nullptr, nullptr, (PUCHAR)quickCheckName, &quickCheckNameSize) == ERROR_SUCCESS) {
        planExists = true;
        Logger::Info("WinAnanicy Energy Optimizer power plan already exists (verified by GUID).");
    }

    // Enumeration check (by name) just in case it was created with a different GUID but same name
    if (!planExists) {
        GUID enumeratedGuid;
        DWORD bufferSize = sizeof(GUID);
        DWORD index = 0;
        while (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index, (UCHAR*)&enumeratedGuid, &bufferSize) == ERROR_SUCCESS) {
            wchar_t nameBuffer[256] = {0};
            DWORD nameBufferSize = sizeof(nameBuffer);
            if (PowerReadFriendlyName(nullptr, &enumeratedGuid, nullptr, nullptr, (PUCHAR)nameBuffer, &nameBufferSize) == ERROR_SUCCESS) {
                std::wstring friendlyName(nameBuffer);
                if (friendlyName == L"WinAnanicy Energy Optimizer") {
                    planExists = true;
                    Logger::Info("WinAnanicy Energy Optimizer power plan already exists (found by name during enumeration).");
                    
                    // If it has a different GUID, we should delete it first so we can recreate it with our desired GUID!
                    if (enumeratedGuid != GUID_WINANANICY_OPTIMIZER) {
                        Logger::Warn("Found power plan with matching name but different GUID. Deleting it to enforce custom GUID.");
                        PowerDeleteScheme(nullptr, &enumeratedGuid);
                        planExists = false;
                    }
                    break;
                }
            }
            index++;
            bufferSize = sizeof(GUID);
        }
    }

    if (planExists) {
        return true;
    }

    Logger::Info("Creating custom WinAnanicy Energy Optimizer power plan...");

    // 2. Duplicate from Balanced scheme to our custom GUID
    GUID customGuid = GUID_WINANANICY_OPTIMIZER;
    GUID* pCustomGuid = &customGuid;
    DWORD res = PowerDuplicateScheme(nullptr, &GUID_BALANCED, &pCustomGuid);
    if (res != ERROR_SUCCESS) {
        Logger::Error("Failed to duplicate Balanced power plan. Error: " + std::to_string(res));
        return false;
    }

    // 3. Write friendly name and description
    const wchar_t* friendlyName = L"WinAnanicy Energy Optimizer";
    DWORD friendlyNameSize = static_cast<DWORD>((wcslen(friendlyName) + 1) * sizeof(wchar_t));
    res = PowerWriteFriendlyName(nullptr, &GUID_WINANANICY_OPTIMIZER, nullptr, nullptr, (UCHAR*)friendlyName, friendlyNameSize);
    if (res != ERROR_SUCCESS) {
        Logger::Warn("Failed to write friendly name for custom power scheme. Error: " + std::to_string(res));
    }

    const wchar_t* description = L"Optimized performance and thermals for gaming, managed by WinAnanicy.";
    DWORD descriptionSize = static_cast<DWORD>((wcslen(description) + 1) * sizeof(wchar_t));
    res = PowerWriteDescription(nullptr, &GUID_WINANANICY_OPTIMIZER, nullptr, nullptr, (UCHAR*)description, descriptionSize);
    if (res != ERROR_SUCCESS) {
        Logger::Warn("Failed to write description for custom power scheme. Error: " + std::to_string(res));
    }

    // Helper to log and set settings
    auto ApplyTweak = [&](const GUID& subgroup, const GUID& setting, DWORD val, std::string_view desc) {
        DWORD acRes = PowerWriteACValueIndex(nullptr, &GUID_WINANANICY_OPTIMIZER, &subgroup, &setting, val);
        DWORD dcRes = PowerWriteDCValueIndex(nullptr, &GUID_WINANANICY_OPTIMIZER, &subgroup, &setting, val);
        if (acRes != ERROR_SUCCESS || dcRes != ERROR_SUCCESS) {
            Logger::Debug("Skipped/Failed to apply power setting override (" + std::string(desc) + "). This setting may not be supported on this system hardware configuration.");
        } else {
            Logger::Info("Configured custom power plan: Set " + std::string(desc) + " to " + std::to_string(val));
        }
    };

    // Apply specific tweaks:
    // Minimum Processor State: 5% (AC and DC)
    ApplyTweak(GUID_PROCESSOR_SUBGROUP, GUID_PROCESSOR_MIN_STATE, 5, "Minimum Processor State");

    // Maximum Processor State: 100% (AC and DC)
    ApplyTweak(GUID_PROCESSOR_SUBGROUP, GUID_PROCESSOR_MAX_STATE, 100, "Maximum Processor State");

    // Processor Performance Boost Mode: 4 (Efficient Enabled)
    ApplyTweak(GUID_PROCESSOR_SUBGROUP, GUID_PROCESSOR_BOOST_MODE, 4, "Processor Performance Boost Mode");

    // GPU Preference: 2 (High Performance)
    ApplyTweak(GUID_GRAPHICS_SUBGROUP_LOCAL, GUID_GPU_PREFERENCE, 2, "GPU Preference Policy");

    // Intel Graphics Settings: 2 (Maximum Performance)
    ApplyTweak(GUID_INTEL_GRAPHICS_SUBGROUP, GUID_INTEL_GRAPHICS_PLAN, 2, "Intel Graphics Power Plan");

    // 4. Force reload by temporarily activating and restoring
    GUID currentActiveScheme;
    if (GetActivePowerScheme(currentActiveScheme)) {
        if (SetActivePowerScheme(GUID_WINANANICY_OPTIMIZER)) {
            SetActivePowerScheme(currentActiveScheme);
            Logger::Info("Successfully initialized and saved tweaks for 'WinAnanicy Energy Optimizer' plan.");
        } else {
            Logger::Error("Failed to temporarily activate custom power plan to apply updates.");
        }
    }

    return true;
}

bool DeleteCustomPowerPlan() {
    const GUID GUID_WINANANICY_OPTIMIZER = { 0xa7bc678d, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };
    DWORD res = PowerDeleteScheme(nullptr, &GUID_WINANANICY_OPTIMIZER);
    if (res == ERROR_SUCCESS) {
        Logger::Info("Deleted custom WinAnanicy Energy Optimizer power plan.");
        return true;
    } else if (res == ERROR_FILE_NOT_FOUND) {
        Logger::Debug("WinAnanicy Energy Optimizer power plan not found for deletion.");
        return true;
    }
    Logger::Error("Failed to delete custom power plan. Error: " + std::to_string(res));
    return false;
}

} // namespace ProcessUtils


