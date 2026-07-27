# Security policy

## Supported versions

Only the latest published WinAnanicy release receives security updates.

## Reporting a vulnerability

Please do not publish exploitable details in a public issue. Use GitHub's private
security advisory feature for this repository. Include the affected version,
reproduction steps, expected impact, and any suggested mitigation.

WinAnanicy runs its optimization engine in the signed-in user's session and does
not require administrator privileges. Rule and settings files are stored in that
user's `%LOCALAPPDATA%\WinAnanicy` directory.
