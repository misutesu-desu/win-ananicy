#pragma once

#include <string>
#include <windows.h>

namespace ProcessUtils {

bool EnableRequiredPrivileges();

bool SetCpuPriority(DWORD pid, const std::string& priorityClass);
bool SetCpuPriorityClass(DWORD pid, DWORD priorityClass);
bool SetCpuAffinity(DWORD pid, const std::string& affinity);
bool SetCpuAffinityMask(DWORD pid, DWORD_PTR affinityMask);

bool SetIoPriority(DWORD pid, const std::string& ioPriority);
bool SetIoPriorityValue(DWORD pid, ULONG ioPriority);
bool GetIoPriority(HANDLE process, ULONG& ioPriority);

bool SetProcessEcoQoS(HANDLE process, bool enabled);
bool GetProcessEcoQoS(HANDLE process, bool& enabled);

bool TrimProcessMemory(HANDLE process);

HANDLE CreateProcessCpuRateLimit(HANDLE process, DWORD percentage);
bool DisableProcessCpuRateLimit(HANDLE job);

bool SetActivePowerScheme(const GUID& schemeGuid);
bool GetActivePowerScheme(GUID& schemeGuid);
bool CreateAndSetupCustomPowerPlan();
bool DeleteCustomPowerPlan();

bool TryParseAffinity(const std::string& text, DWORD_PTR& mask, std::string& error);

} // namespace ProcessUtils
