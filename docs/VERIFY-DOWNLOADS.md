# Verify a WinAnanicy download

Official WinAnanicy packages are published only at:

<https://github.com/misutesu-desu/win-ananicy/releases>

Every release contains:

- `WinAnanicy-<version>-Setup.exe`
- `WinAnanicy-<version>-Portable.zip`
- `SHA256SUMS.txt`

## Verify SHA-256 on Windows

Download the package and `SHA256SUMS.txt` into the same directory, open
PowerShell there, and run:

```powershell
Get-FileHash .\WinAnanicy-1.0.1-Setup.exe -Algorithm SHA256
Get-Content .\SHA256SUMS.txt
```

For the portable package:

```powershell
Get-FileHash .\WinAnanicy-1.0.1-Portable.zip -Algorithm SHA256
Get-Content .\SHA256SUMS.txt
```

The digest printed by `Get-FileHash` must exactly match the corresponding line
in `SHA256SUMS.txt`.

## Verify GitHub build provenance

Starting with 1.0.1, the release workflow also creates a signed GitHub artifact
attestation. With GitHub CLI installed:

```powershell
gh attestation verify .\WinAnanicy-1.0.1-Setup.exe `
    --repo misutesu-desu/win-ananicy
```

For the portable package:

```powershell
gh attestation verify .\WinAnanicy-1.0.1-Portable.zip `
    --repo misutesu-desu/win-ananicy
```

The attestation proves that the package was produced by this repository's
GitHub Actions release workflow. It complements, rather than replaces, SHA-256
verification.

## Authenticode status

The 1.0.1 community build is not Authenticode-signed. You can confirm the
current status with:

```powershell
Get-AuthenticodeSignature .\WinAnanicy-1.0.1-Setup.exe |
    Select-Object Status, StatusMessage
```

Windows SmartScreen may show an unknown-publisher warning for an unsigned
package. Do not download WinAnanicy from mirrors, link shorteners, file-sharing
sites, or unsolicited messages.

## Build it yourself

The complete release pipeline is available in `scripts/build-release.ps1`.
From a Windows development environment with CMake, a C++20 compiler, .NET 8,
and Inno Setup 6:

```powershell
.\scripts\build-release.ps1 -Version 1.0.1
```

The script runs the C++ tests, publishes the self-contained WPF application,
builds Setup and Portable packages, and generates local SHA-256 checksums.
