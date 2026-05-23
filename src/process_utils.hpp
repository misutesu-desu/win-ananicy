#pragma once

#include <string>
#include <windows.h>

namespace ProcessUtils {

// Attempts to enable SeDebugPrivilege and SeIncreaseBasePriorityPrivilege for this application.
bool EnableRequiredPrivileges();

// Sets CPU Priority class for the target process.
bool SetCpuPriority(DWORD pid, const std::string& priorityClassStr);

// Sets CPU Affinity mask for the target process (e.g., "0,1,2,3" -> Core 0 to 3).
bool SetCpuAffinity(DWORD pid, const std::string& affinityStr);

// Sets I/O priority class for the target process via NtSetInformationProcess.
bool SetIoPriority(DWORD pid, const std::string& ioPriorityStr);

// Enables or disables EcoQoS (Efficiency Mode / E-Core Lock)
bool SetProcessEcoQoS(HANDLE hProcess, bool enable);

// Trims the working set memory of the process
bool TrimProcessMemory(HANDLE hProcess);

// Limits the CPU rate of the process using Job Objects
bool LimitProcessCpuRate(HANDLE hProcess, DWORD limitPercentage);

// Sets system power scheme active by GUID
bool SetActivePowerScheme(const GUID& schemeGuid);

// Gets current active power scheme GUID
bool GetActivePowerScheme(GUID& schemeGuid);

} // namespace ProcessUtils
