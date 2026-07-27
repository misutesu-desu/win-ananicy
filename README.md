<p align="center">
  <img src="assets/win-ananicy-icon.png" width="128" alt="WinAnanicy logo">
</p>

<h1 align="center">WinAnanicy</h1>

<p align="center">
  Reversible process control for Windows 10 and 11.<br>
  No administrator access. No privileged service. No telemetry.
</p>

<p align="center">
  <a href="https://github.com/misutesu-desu/win-ananicy/actions/workflows/build.yml"><img alt="Build and test" src="https://github.com/misutesu-desu/win-ananicy/actions/workflows/build.yml/badge.svg"></a>
  <a href="https://github.com/misutesu-desu/win-ananicy/actions/workflows/codeql.yml"><img alt="CodeQL" src="https://github.com/misutesu-desu/win-ananicy/actions/workflows/codeql.yml/badge.svg"></a>
  <a href="https://github.com/misutesu-desu/win-ananicy/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/misutesu-desu/win-ananicy"></a>
  <a href="LICENSE"><img alt="GPL-3.0-or-later" src="https://img.shields.io/github/license/misutesu-desu/win-ananicy"></a>
  <a href="https://github.com/misutesu-desu/win-ananicy/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/misutesu-desu/win-ananicy/total"></a>
</p>

<p align="center">
  <a href="https://github.com/misutesu-desu/win-ananicy/releases/latest"><strong>Download</strong></a>
  · <a href="#how-it-works">How it works</a>
  · <a href="docs/VERIFY-DOWNLOADS.md">Verify a download</a>
  · <a href="ROADMAP.md">Roadmap</a>
  · <a href="#türkçe">Türkçe</a>
</p>

<p align="center">
  <img src="docs/images/winananicy-demo.gif" width="960" alt="WinAnanicy applying and monitoring a reversible process rule">
</p>

WinAnanicy is an open-source Windows process rule engine with a native C++ core
and a WPF control center. It applies a rule only while its conditions match,
shows what is active, and restores the process's original state when the rule
stops applying or the engine exits.

## Install

1. Download the current **Setup** package from
   [GitHub Releases](https://github.com/misutesu-desu/win-ananicy/releases/latest).
2. Choose English or Turkish.
3. Optionally allow the engine to start with Windows.
4. Open **Processes**, select an application, and choose **Add rule**.

The installer is per-user and does not request elevation. A self-contained
Portable ZIP is available for users who prefer extract-and-run software.

> [!IMPORTANT]
> The current community build is not Authenticode-signed, so Windows SmartScreen
> may show an unknown-publisher warning. Download only from this repository and
> verify the package against `SHA256SUMS.txt` by following
> [Verify a download](docs/VERIFY-DOWNLOADS.md).

## What WinAnanicy controls

| Control | Behavior |
|---|---|
| CPU priority | Applies `Idle` through `High` without using `Realtime` |
| I/O priority | Applies `Very Low` through `High` |
| CPU affinity | Selects logical cores by list or explicit bit mask |
| EcoQoS | Requests Windows efficiency behavior for suitable processes |
| CPU rate limit | Uses a Windows job object to enforce a configured limit |
| Background-only rules | Applies the complete rule only while the app is not foreground |
| SmartTrim | Trims a matching process after a configured memory threshold |
| Sustained-load throttle | Responds only after both load and duration thresholds match |
| Instance balancing | Spreads matching instances across available logical cores |
| Keep Alive / Disallowed | Restarts a trusted executable or terminates a matching process |

Quick presets provide practical starting points for games, creative tools,
background applications, and strict power saving. Every preset remains visible
and editable as a normal rule.

## What it does not claim

WinAnanicy is not a registry cleaner, driver updater, overclocking utility, or a
magic FPS booster. It does not disable Windows security features or apply hidden
system tweaks. Process scheduling can improve consistency in the right workload,
but results depend on the application, hardware, and bottleneck.

## How it works

```text
rules.json ──► validate ──► match process ──► capture original state
                                                      │
                                                      ▼
                                               apply exact rule
                                                      │
                  rule removed / focus changed / engine exits
                                                      │
                                                      ▼
                                             restore original state
```

- The engine runs in the signed-in user's interactive session.
- Configuration lives under `%LOCALAPPDATA%\WinAnanicy`.
- Changes are applied only when the configured rule matches.
- CPU priority, I/O priority, affinity, EcoQoS, and CPU rate state are captured
  before modification and restored at each lifecycle boundary.
- Configuration writes are atomic, validated, and backed up.
- The application contains no telemetry and makes no network connections.

`Disallowed` and `Keep Alive` intentionally affect process lifetime. Use them
only with applications you trust; the editor prevents both from being enabled
on the same rule.

## Rule example

```json
[
  {
    "process_name": "example-game.exe",
    "cpu_priority": "High",
    "io_priority": "High",
    "cpu_affinity": null,
    "background_only": false,
    "eco_qos": false,
    "cpu_limit": 0
  }
]
```

| Field | Accepted value |
|---|---|
| `process_name` | Executable file name ending in `.exe`; paths are rejected |
| `cpu_priority` | `Idle`, `Below Normal`, `Normal`, `Above Normal`, `High` |
| `io_priority` | `Very Low`, `Low`, `Normal`, `High` |
| `cpu_affinity` | Core list, `0x` hexadecimal mask, `mask:` decimal mask, or `null` |
| `background_only` | Apply and restore the complete rule as focus changes |
| `eco_qos` | Windows efficiency mode |
| `cpu_limit` | `0` to disable or `1`–`99` percent |
| `smart_trim_threshold_mb` | `32`–`1048576`, or `null` |
| `cpu_throttle_trigger_pct` | `1`–`100`; requires duration |
| `cpu_throttle_duration_secs` | `1`–`3600`; requires trigger |
| `instance_balance` | Spread matching instances across logical cores |
| `disallowed` | Terminate matching processes |
| `keep_alive` | Restart the executable at `executable_path` with bounded backoff |

JSON-aware editors can use
[`rules.schema.json`](schemas/rules.schema.json) and
[`settings.schema.json`](schemas/settings.schema.json).

## Build from source

Requirements: Windows 10/11 x64, CMake 3.20+, a C++20 compiler, .NET 8 SDK,
and Inno Setup 6.

```powershell
.\scripts\build-release.ps1
```

The release script builds and tests the C++ engine, publishes the self-contained
WPF application, creates Setup and Portable packages, and writes SHA-256
checksums to `artifacts`.

For a development build:

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
dotnet build .\gui\WinAnanicyGui.csproj -c Release
```

See [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md), and the
public [roadmap](ROADMAP.md) before proposing a large change.

## Türkçe

WinAnanicy, Windows 10 ve 11 için açık kaynaklı ve geri alınabilir bir süreç
kural motorudur. Yerel C++ motoru ile WPF kontrol merkezi; CPU/G/Ç önceliği,
çekirdek seçimi, EcoQoS ve CPU sınırı gibi ayarları yalnızca kural eşleştiğinde
uygular. Kural devreden çıktığında veya motor kapandığında sürecin özgün
durumunu geri yükler.

### Neden farklı?

- Yönetici izni ve ayrıcalıklı Windows servisi istemez.
- Telemetri veya ağ bağlantısı içermez.
- Gizli kayıt defteri ayarları ya da “tek tıkla FPS” iddiası kullanmaz.
- Yapılan değişiklikleri canlı gösterir ve yaşam döngüsü sınırlarında geri alır.
- Türkçe ve İngilizce arayüz, kurucu ve taşınabilir paket sunar.

Kurulum için
[son sürümü indirin](https://github.com/misutesu-desu/win-ananicy/releases/latest),
**Süreçler** sayfasından bir uygulama seçin ve **Kural ekle** düğmesini kullanın.
Mevcut topluluk derlemesi dijital imzalı değildir; yalnızca resmî GitHub
sürümünü kullanın ve
[SHA-256 doğrulamasını](docs/VERIFY-DOWNLOADS.md) tamamlayın.

## Contributing

Issues and pull requests are welcome. User-facing resources must remain complete
in both English and Turkish, and behavior changes must include appropriate
tests. Security reports belong in a private GitHub security advisory.

If WinAnanicy is useful to you, starring the repository helps other Windows
users discover it.

## License

Copyright © 2026 Abdullah Çafer (misutesu-desu).

WinAnanicy is licensed under the
[GNU General Public License v3.0 or later](LICENSE).
