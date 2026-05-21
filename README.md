# WinAnanicy

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011-lightgrey.svg)](#)
[![Language](https://img.shields.io/badge/Languages-C%2B%2B%20%2F%20C%23-orange.svg)](#)

WinAnanicy is a lightweight, high-performance, open-source background process optimizer for Windows. Inspired by Linux's "Ananicy-cpp" and Process Lasso, it runs silently in the background, automatically adjusting process CPU priorities, logical processor affinities, and I/O priorities based on a human-readable JSON configuration file. It also includes a modern, Fluent-designed WPF GUI client to monitor live processes, check the Windows Service status, and construct rules interactively.

---

## Table of Contents
- [Key Features](#key-features)
- [How It Works](#how-it-works)
  - [State-Based Optimization](#state-based-optimization)
  - [Win32 & Native APIs Used](#win32--native-apis-used)
- [Installation & Compilation](#installation--compilation)
  - [Prerequisites](#prerequisites)
  - [Compiling the C++ Core Daemon](#compiling-the-c-core-daemon)
  - [Compiling the C# WPF GUI Client](#compiling-the-c-wpf-gui-client)
- [Operating Instructions](#operating-instructions)
  - [Running the Core Daemon](#running-the-core-daemon)
  - [Running as a Windows Service](#running-as-a-windows-service)
  - [Running the GUI Client](#running-the-gui-client)
- [Configuration Guide (rules.json)](#configuration-guide-rulesjson)
  - [Rule Parameters](#rule-parameters)
  - [Example Configuration](#example-configuration)
- [Contributing](#contributing)
- [License](#license)

---

## Key Features

*   **Low Resource Footprint**: The C++ background daemon consumes **< 7MB RAM** and **0% CPU** when idling.
*   **State-Based Optimization**: Rules are applied on process creation and foreground-to-background transitions, preventing constant polling and unnecessary kernel API calls.
*   **Dynamic I/O Prioritization**: Implements dynamic I/O scheduling class modifications for optimized disk access.
*   **CPU Core Affinity Control**: Restricts processes to specific logical processor cores to prevent resource starvation or core thrashing.
*   **Modern Control Panel**: A Fluent Dark Mode C# WPF desktop GUI designed for Windows 11.
*   **Quick Presets Dropdown**: Supports 1-click optimization profiles in the GUI to instantly configure rules for games, overlays, dynamic web applications, and strict background throttles.
*   **Visual Optimization Badges**: Displays a green "Optimized" badge next to processes with active rules, turning the action button from a default blue "Optimize" to a distinct green-bordered "Edit Rule".
*   **Preset Auto-Detection**: When editing a rule, the GUI automatically maps properties (priorities, affinity, and background settings) back to the matching preset.
*   **Configuration Hot-Reloading**: Automatically detects edits to `rules.json` and updates the active rules list in real-time.

---

## How It Works

### State-Based Optimization
Unlike tools that poll the kernel constantly, WinAnanicy tracks optimization status using state flags. When a process matching a rule starts, the daemon applies its settings once. If a rule specifies `background_only = true`, settings are toggled dynamically on foreground/background status changes (determined via the active window thread process).

### Win32 & Native APIs Used
*   **Process Traversal**: Uses `CreateToolhelp32Snapshot` to walk active processes with minimal overhead.
*   **CPU Priority**: Uses Win32 `SetPriorityClass` to modify priority classes (from Idle to Realtime).
*   **CPU Affinity**: Uses `SetProcessAffinityMask` using bitmasks computed from logical core mappings.
*   **I/O Priority**: Dynamically binds `NtSetInformationProcess` from `ntdll.dll` to set target processes to low or high I/O scheduling priority (using internal information class `0x21`).
*   **Foreground Tracking**: Monitors focus changes via `GetForegroundWindow` and `GetWindowThreadProcessId`.

---

## Installation & Compilation

### Prerequisites
*   **CMake 3.20 or newer** (for the C++ Core Engine)
*   **Visual Studio 2022** with "Desktop development with C++" workload
*   **.NET 8.0 SDK** (for the C# WPF GUI Client)

### Compiling the C++ Core Daemon
1. Open PowerShell and navigate to the project directory:
   ```powershell
   cd win-ananicy
   ```
2. Generate the build files:
   ```powershell
   cmake -B build -S .
   ```
3. Compile the standalone executable in Release mode:
   ```powershell
   cmake --build build --config Release
   ```
The compiled binary `win-ananicy.exe` will be located at `build/Release/win-ananicy.exe`.

### Compiling the C# WPF GUI Client
1. Build the .NET project:
   ```powershell
   dotnet build gui/WinAnanicyGui.csproj -c Release
   ```
The compiled GUI client will be located at `gui/bin/Release/net8.0-windows/WinAnanicyGui.exe`.

---

## Operating Instructions

### Running the Core Daemon
To run the C++ core engine manually in console mode (useful for viewing stdout logs):
```powershell
.\build\Release\win-ananicy.exe --run
```
To run the C++ core engine as a hidden background daemon (detached from any visible console):
```powershell
.\build\Release\win-ananicy.exe --background
```

### Running as a Windows Service
To run WinAnanicy in production, it can be registered as a native Windows service that launches automatically at system boot:
1. Open PowerShell **as Administrator**.
2. Install and register the service:
   ```powershell
   .\build\Release\win-ananicy.exe --install
   ```
3. Start the service:
   ```powershell
   Start-Service WinAnanicy
   ```
4. To stop and uninstall the service at a later date:
   ```powershell
   Stop-Service WinAnanicy
   .\build\Release\win-ananicy.exe --uninstall
   ```

### Running the GUI Client
Simply launch `WinAnanicyGui.exe`. 
*   **No Service Mode**: If the service is not installed or running, you can still edit rules in the GUI, which will automatically update the config file.
*   **Service Mode**: Start the service to apply changes automatically in the background. The GUI will detect and display the service's current running status.
*   **Elevation**: Management of Windows services (Start/Stop/Restart) requires administrator permissions. If the app is launched as a standard user, click the **Restart as Administrator** button in the status bar to elevate.

#### Quick Presets Feature
The "Configure Rule" overlay in the GUI contains an interactive **Quick Presets** dropdown allowing users to select pre-configured profiles:
*   **Game / High Performance**: Optimizes CPU & I/O priority (`High`) and utilizes all logical cores for heavy gaming.
*   **Helper / Tool / Overlay**: Perfect for utility applications (e.g. Lossless Scaling, OBS, Discord overlay), giving them a slight priority boost (`Above Normal` CPU, `Normal` I/O) without throttling the main game.
*   **Web / Chat (Dynamic)**: Designed for web browsers (Chrome, Thorium) and chat apps. Throttles priority to `Below Normal` and restricts CPU affinity to the last 4 cores *only* when the process is minimized/in the background (enabled via `background_only`).
*   **Strict Saver / Background**: Heavily throttles background launchers and updaters to `Below Normal` CPU, `Low` I/O, and isolates them to the last 2 logical cores to completely free up primary gaming cores.

#### Intelligent Safeguards
*   **Active Rules Indicators**: The process grid automatically matches running processes against your `rules.json` configuration, showing a green **Optimized** badge next to the process name and transitioning the action button from a blue "Optimize" style to a green-accented "Edit Rule" style.
*   **Preset Auto-Detector**: When opening the configuration dialog for an existing rule, the GUI automatically compares CPU priority, I/O priority, background modes, and core affinity mappings (handling low-core boundaries dynamically). If a perfect match is found, the **Quick Presets** dropdown auto-selects that profile, allowing you to instantly see and tweak preset profiles.
*   **Smart Core-Count Scaling**: Presets targeting the last 4 or 2 logical cores automatically scale down safely on low-spec CPUs (e.g. if a system has fewer than 4 or 2 cores, it checks all available cores instead of throwing exceptions).
*   **Manual Override Detection**: If you modify any checkbox or priority dropdown manually after selecting a preset, the preset selector automatically resets to `-- Select a Preset (Optional) --` to clearly indicate custom overrides.

---

## Configuration Guide (rules.json)

The background daemon reads rules from `rules.json` located in the same directory as the executable.

### Rule Parameters

| Parameter | Type | Required | Description |
| :--- | :--- | :--- | :--- |
| `process_name` | String | Yes | File name of the target process (e.g. `"chrome.exe"`). Case-insensitive. |
| `cpu_priority` | String | No | Priority class: `Idle`, `Below Normal`, `Normal`, `Above Normal`, `High`, or `Realtime`. |
| `io_priority` | String | No | I/O scheduling class: `Very Low`, `Low`, `Normal`, or `High`. |
| `cpu_affinity` | String | No | Logical core list (e.g., `"0,1,2"`), decimal mask (e.g. `15`), or hex mask (e.g., `"0x0F"`). |
| `background_only`| Boolean| No | If `true`, rules only apply when the process is not in the foreground. |

### Example Configuration

```json
[
  {
    "process_name": "cmd.exe",
    "cpu_priority": "Below Normal",
    "io_priority": "Low",
    "cpu_affinity": "0",
    "background_only": true
  },
  {
    "process_name": "obs64.exe",
    "cpu_priority": "Above Normal",
    "io_priority": "High",
    "cpu_affinity": "2,3"
  }
]
```

---

## Contributing
Contributions are welcome! Please open an issue or submit a pull request with any improvements, bug fixes, or enhancements.

## License
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
