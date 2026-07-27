# Changelog

All notable changes to WinAnanicy are documented here.

## [1.0.0] - 2026-07-27

### Added

- Premium WPF control center with Dashboard, Processes, Rules, Activity, and Settings pages.
- Complete English and Turkish localization for the application and installer.
- Per-user background engine with live health reporting and notification-area controls.
- One-click Game, Creative, Background, and Strict Saver profiles.
- Atomic configuration writes, automatic backups, import/export, and strict validation.
- Bilingual Inno Setup installer, portable package, Windows startup option, and clean upgrades.
- CI builds, C++ regression tests, release automation, and SHA-256 checksums.

### Changed

- Replaced the privileged Windows service with a safer interactive user-session engine.
- Rules and preferences now live under `%LOCALAPPDATA%\WinAnanicy`.
- Affinity syntax is unambiguous: plain numbers are core indexes; masks use `0x...` or `mask:...`.
- All process properties are restored when a rule stops applying or the engine exits.

### Fixed

- Foreground/background detection now works in the actual signed-in desktop session.
- Background-only rules restore CPU, I/O, affinity, EcoQoS, and CPU limits consistently.
- Watchdog launches use a validated absolute executable path and bounded retry backoff.
- CPU limit jobs, SmartTrim cooldown, instance balancing, and adaptive power-plan restoration.
