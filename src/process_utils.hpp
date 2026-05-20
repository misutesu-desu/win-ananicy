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

} // namespace ProcessUtils
