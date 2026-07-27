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

## Release verification

Official binaries are published only through this repository's GitHub Releases
page. Each release includes `SHA256SUMS.txt`. Follow
[`docs/VERIFY-DOWNLOADS.md`](docs/VERIFY-DOWNLOADS.md) before running a
downloaded package.

The current community build is not Authenticode-signed. Windows SmartScreen may
therefore show an unknown-publisher warning. A signature warning is not a
substitute for checksum verification; do not run a package if its source or
SHA-256 digest does not match the official release.
