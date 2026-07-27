#include "process_utils.hpp"
#include "logger.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <memory>
#include <powrprof.h>
#include <psapi.h>
#include <sstream>
#include <vector>

namespace ProcessUtils {

namespace {

struct HandleCloser {
    void operator()(HANDLE handle) const {
        if (handle && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

using NtSetInformationProcessFn =
    NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
using NtQueryInformationProcessFn =
    NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

constexpr ULONG ProcessIoPriorityInformation = 0x21;

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

bool SetTokenPrivilege(HANDLE token, LPCWSTR privilege, bool enabled) {
    TOKEN_PRIVILEGES state{};
    if (!LookupPrivilegeValueW(nullptr, privilege, &state.Privileges[0].Luid)) {
        return false;
    }

    state.PrivilegeCount = 1;
    state.Privileges[0].Attributes = enabled ? SE_PRIVILEGE_ENABLED : 0;
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(
            token, FALSE, &state, sizeof(state), nullptr, nullptr)) {
        return false;
    }
    return GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

template <typename T>
T GetNtFunction(const char* name) {
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    if (!module) {
        module = LoadLibraryW(L"ntdll.dll");
    }
    if (!module) {
        return nullptr;
    }
    const FARPROC address = GetProcAddress(module, name);
    return address ? std::bit_cast<T>(address) : nullptr;
}

bool ParseUnsignedStrict(
    const std::string& value,
    int base,
    unsigned long long& result) {
    try {
        std::size_t consumed = 0;
        result = std::stoull(value, &consumed, base);
        return consumed == value.size();
    } catch (...) {
        return false;
    }
}

bool SetPowerValue(
    const GUID& scheme,
    const GUID& subgroup,
    const GUID& setting,
    DWORD value,
    std::string_view description) {
    const DWORD acResult =
        PowerWriteACValueIndex(nullptr, &scheme, &subgroup, &setting, value);
    const DWORD dcResult =
        PowerWriteDCValueIndex(nullptr, &scheme, &subgroup, &setting, value);
    if (acResult != ERROR_SUCCESS || dcResult != ERROR_SUCCESS) {
        Logger::Debug(
            "Power setting is unsupported or could not be written: " +
            std::string(description));
        return false;
    }
    return true;
}

} // namespace

bool TryParseAffinity(
    const std::string& text,
    DWORD_PTR& mask,
    std::string& error) {
    mask = 0;
    if (text.empty()) {
        error = "affinity cannot be empty";
        return false;
    }

    constexpr unsigned int bitCount = sizeof(DWORD_PTR) * 8U;
    auto addCore = [&](unsigned long long core) {
        if (core >= bitCount) {
            error = "core index is outside the current processor group";
            return false;
        }
        const DWORD_PTR bit =
            static_cast<DWORD_PTR>(1) << static_cast<unsigned int>(core);
        if ((mask & bit) != 0) {
            error = "core index is listed more than once";
            return false;
        }
        mask |= bit;
        return true;
    };

    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) {
        unsigned long long value = 0;
        if (!ParseUnsignedStrict(text.substr(2), 16, value) || value == 0) {
            error = "invalid hexadecimal affinity mask";
            return false;
        }
        mask = static_cast<DWORD_PTR>(value);
        return true;
    }

    if (text.rfind("mask:", 0) == 0 || text.rfind("MASK:", 0) == 0) {
        unsigned long long value = 0;
        if (!ParseUnsignedStrict(text.substr(5), 10, value) || value == 0) {
            error = "invalid decimal affinity mask";
            return false;
        }
        mask = static_cast<DWORD_PTR>(value);
        return true;
    }

    std::stringstream stream(text);
    std::string token;
    while (std::getline(stream, token, ',')) {
        token.erase(
            std::remove_if(token.begin(), token.end(), [](unsigned char c) {
                return std::isspace(c) != 0;
            }),
            token.end());
        unsigned long long core = 0;
        if (token.empty() || !ParseUnsignedStrict(token, 10, core) || !addCore(core)) {
            if (error.empty()) {
                error = "invalid core list";
            }
            mask = 0;
            return false;
        }
    }

    if (mask == 0) {
        error = "affinity mask resolved to zero";
        return false;
    }
    return true;
}

bool EnableRequiredPrivileges() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
            &rawToken)) {
        Logger::Warn("Unable to open the engine token for privilege adjustment.");
        return false;
    }
    UniqueHandle token(rawToken);

    if (!SetTokenPrivilege(token.get(), L"SeDebugPrivilege", true)) {
        Logger::Warn(
            "SeDebugPrivilege is unavailable. Elevated processes may be skipped.");
        return false;
    }
    return true;
}

bool SetCpuPriorityClass(DWORD pid, DWORD priorityClass) {
    UniqueHandle process(OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid));
    if (!process) {
        return false;
    }
    return SetPriorityClass(process.get(), priorityClass) != FALSE;
}

bool SetCpuPriority(DWORD pid, const std::string& priorityClass) {
    DWORD value = NORMAL_PRIORITY_CLASS;
    if (EqualsIgnoreCase(priorityClass, "Idle")) {
        value = IDLE_PRIORITY_CLASS;
    } else if (EqualsIgnoreCase(priorityClass, "Below Normal")) {
        value = BELOW_NORMAL_PRIORITY_CLASS;
    } else if (EqualsIgnoreCase(priorityClass, "Normal")) {
        value = NORMAL_PRIORITY_CLASS;
    } else if (EqualsIgnoreCase(priorityClass, "Above Normal")) {
        value = ABOVE_NORMAL_PRIORITY_CLASS;
    } else if (EqualsIgnoreCase(priorityClass, "High")) {
        value = HIGH_PRIORITY_CLASS;
    } else {
        Logger::Warn("Unknown CPU priority: " + priorityClass);
        return false;
    }

    if (SetCpuPriorityClass(pid, value)) {
        return true;
    }
    Logger::Warn("Failed to set CPU priority for PID " + std::to_string(pid));
    return false;
}

bool SetCpuAffinityMask(DWORD pid, DWORD_PTR affinityMask) {
    if (affinityMask == 0) {
        return false;
    }

    UniqueHandle process(OpenProcess(
        PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        pid));
    if (!process) {
        return false;
    }

    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (GetProcessAffinityMask(process.get(), &processMask, &systemMask)) {
        affinityMask &= systemMask;
        if (affinityMask == 0) {
            Logger::Warn(
                "Requested affinity has no available cores for PID " +
                std::to_string(pid));
            return false;
        }
    }
    return SetProcessAffinityMask(process.get(), affinityMask) != FALSE;
}

bool SetCpuAffinity(DWORD pid, const std::string& affinity) {
    DWORD_PTR mask = 0;
    std::string error;
    if (!TryParseAffinity(affinity, mask, error)) {
        Logger::Warn("Invalid affinity '" + affinity + "': " + error);
        return false;
    }
    return SetCpuAffinityMask(pid, mask);
}

bool SetIoPriorityValue(DWORD pid, ULONG ioPriority) {
    auto function =
        GetNtFunction<NtSetInformationProcessFn>("NtSetInformationProcess");
    if (!function) {
        return false;
    }

    UniqueHandle process(OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid));
    if (!process) {
        return false;
    }
    return function(
               process.get(),
               ProcessIoPriorityInformation,
               &ioPriority,
               sizeof(ioPriority)) >= 0;
}

bool SetIoPriority(DWORD pid, const std::string& ioPriority) {
    ULONG value = 2;
    if (EqualsIgnoreCase(ioPriority, "Very Low")) {
        value = 0;
    } else if (EqualsIgnoreCase(ioPriority, "Low")) {
        value = 1;
    } else if (EqualsIgnoreCase(ioPriority, "Normal")) {
        value = 2;
    } else if (EqualsIgnoreCase(ioPriority, "High")) {
        value = 3;
    } else {
        Logger::Warn("Unknown I/O priority: " + ioPriority);
        return false;
    }
    return SetIoPriorityValue(pid, value);
}

bool GetIoPriority(HANDLE process, ULONG& ioPriority) {
    auto function =
        GetNtFunction<NtQueryInformationProcessFn>("NtQueryInformationProcess");
    if (!function) {
        return false;
    }
    ULONG returned = 0;
    return function(
               process,
               ProcessIoPriorityInformation,
               &ioPriority,
               sizeof(ioPriority),
               &returned) >= 0;
}

bool SetProcessEcoQoS(HANDLE process, bool enabled) {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask =
        enabled ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    return SetProcessInformation(
               process,
               ProcessPowerThrottling,
               &state,
               sizeof(state)) != FALSE;
}

bool GetProcessEcoQoS(HANDLE process, bool& enabled) {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if (!GetProcessInformation(
            process,
            ProcessPowerThrottling,
            &state,
            sizeof(state))) {
        return false;
    }
    enabled =
        (state.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0;
    return true;
}

bool TrimProcessMemory(HANDLE process) {
    return EmptyWorkingSet(process) != FALSE;
}

HANDLE CreateProcessCpuRateLimit(HANDLE process, DWORD percentage) {
    if (percentage == 0 || percentage >= 100) {
        return nullptr;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        return nullptr;
    }

    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION information{};
    information.ControlFlags =
        JOB_OBJECT_CPU_RATE_CONTROL_ENABLE |
        JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    information.CpuRate = percentage * 100;

    if (!SetInformationJobObject(
            job,
            JobObjectCpuRateControlInformation,
            &information,
            sizeof(information)) ||
        !AssignProcessToJobObject(job, process)) {
        Logger::Warn(
            "CPU rate limit could not be attached. The process may already be in "
            "an incompatible job.");
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

bool DisableProcessCpuRateLimit(HANDLE job) {
    if (!job || job == INVALID_HANDLE_VALUE) {
        return true;
    }
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION information{};
    return SetInformationJobObject(
               job,
               JobObjectCpuRateControlInformation,
               &information,
               sizeof(information)) != FALSE;
}

bool SetActivePowerScheme(const GUID& schemeGuid) {
    return PowerSetActiveScheme(nullptr, &schemeGuid) == ERROR_SUCCESS;
}

bool GetActivePowerScheme(GUID& schemeGuid) {
    GUID* current = nullptr;
    if (PowerGetActiveScheme(nullptr, &current) != ERROR_SUCCESS || !current) {
        return false;
    }
    schemeGuid = *current;
    LocalFree(current);
    return true;
}

bool CreateAndSetupCustomPowerPlan() {
    const GUID balanced = {
        0x381b4222,
        0xf694,
        0x41f0,
        {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
    const GUID optimizer = {
        0xa7bc678d,
        0xd5df,
        0x448d,
        {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};

    wchar_t nameBuffer[256]{};
    DWORD nameSize = sizeof(nameBuffer);
    const bool exists =
        PowerReadFriendlyName(
            nullptr,
            &optimizer,
            nullptr,
            nullptr,
            reinterpret_cast<PUCHAR>(nameBuffer),
            &nameSize) == ERROR_SUCCESS;

    if (!exists) {
        GUID requested = optimizer;
        GUID* requestedPointer = &requested;
        const DWORD result =
            PowerDuplicateScheme(nullptr, &balanced, &requestedPointer);
        if (result != ERROR_SUCCESS) {
            Logger::Warn(
                "Custom power plan could not be created. Error " +
                std::to_string(result));
            return false;
        }
        if (requestedPointer != &requested) {
            LocalFree(requestedPointer);
        }
    }

    const wchar_t* name = L"WinAnanicy Energy Optimizer";
    const wchar_t* description =
        L"Responsive performance profile managed by WinAnanicy.";
    PowerWriteFriendlyName(
        nullptr,
        &optimizer,
        nullptr,
        nullptr,
        reinterpret_cast<UCHAR*>(const_cast<wchar_t*>(name)),
        static_cast<DWORD>((wcslen(name) + 1) * sizeof(wchar_t)));
    PowerWriteDescription(
        nullptr,
        &optimizer,
        nullptr,
        nullptr,
        reinterpret_cast<UCHAR*>(const_cast<wchar_t*>(description)),
        static_cast<DWORD>((wcslen(description) + 1) * sizeof(wchar_t)));

    const GUID processorGroup = {
        0x54533251,
        0x82be,
        0x4824,
        {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
    const GUID minimumState = {
        0x893dee8e,
        0x2bef,
        0x41e0,
        {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c}};
    const GUID maximumState = {
        0xbc5038f7,
        0x23e0,
        0x4960,
        {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}};
    const GUID boostMode = {
        0xbe337238,
        0x0d82,
        0x4146,
        {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};

    SetPowerValue(optimizer, processorGroup, minimumState, 5, "minimum CPU state");
    SetPowerValue(optimizer, processorGroup, maximumState, 100, "maximum CPU state");
    SetPowerValue(optimizer, processorGroup, boostMode, 4, "efficient boost mode");

    GUID previous{};
    if (GetActivePowerScheme(previous) && SetActivePowerScheme(optimizer)) {
        SetActivePowerScheme(previous);
    }
    return true;
}

bool DeleteCustomPowerPlan() {
    const GUID optimizer = {
        0xa7bc678d,
        0xd5df,
        0x448d,
        {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};
    const DWORD result = PowerDeleteScheme(nullptr, &optimizer);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

} // namespace ProcessUtils
