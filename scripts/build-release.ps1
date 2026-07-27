[CmdletBinding()]
param(
    [string]$Version = "",
    [string]$Generator = "",
    [string]$JsonSource = "",
    [switch]$SkipInstaller
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$versionFile = Join-Path $repositoryRoot "VERSION"
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (Get-Content -Raw -LiteralPath $versionFile).Trim()
}
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][0-9A-Za-z.-]+)?$') {
    throw "Invalid semantic version: $Version"
}

$artifactsDirectory = Join-Path $repositoryRoot "artifacts"
$applicationDirectory = Join-Path $artifactsDirectory "app"
$coreBuildDirectory = Join-Path $repositoryRoot "build-release"

foreach ($target in @($artifactsDirectory, $coreBuildDirectory)) {
    $fullTarget = [System.IO.Path]::GetFullPath($target)
    if (-not $fullTarget.StartsWith($repositoryRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a path outside the repository: $fullTarget"
    }
    if (Test-Path -LiteralPath $fullTarget) {
        Remove-Item -LiteralPath $fullTarget -Recurse -Force
    }
}
New-Item -ItemType Directory -Force -Path $applicationDirectory | Out-Null

if ([string]::IsNullOrWhiteSpace($Generator)) {
    if (Get-Command g++ -ErrorAction SilentlyContinue) {
        $Generator = "MinGW Makefiles"
    }
    else {
        $Generator = "Visual Studio 17 2022"
    }
}

$configureArguments = @(
    "-S", $repositoryRoot,
    "-B", $coreBuildDirectory,
    "-G", $Generator,
    "-DBUILD_TESTING=ON"
)
if ($Generator -eq "MinGW Makefiles") {
    $configureArguments += "-DCMAKE_BUILD_TYPE=Release"
}
if (-not [string]::IsNullOrWhiteSpace($JsonSource)) {
    $jsonPath = (Resolve-Path -LiteralPath $JsonSource).Path.Replace("\", "/")
    $configureArguments += "-DFETCHCONTENT_SOURCE_DIR_JSON=$jsonPath"
}

& cmake @configureArguments
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
& cmake --build $coreBuildDirectory --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "C++ build failed." }
& ctest --test-dir $coreBuildDirectory -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "C++ tests failed." }

& dotnet restore (Join-Path $repositoryRoot "gui\WinAnanicyGui.csproj")
if ($LASTEXITCODE -ne 0) { throw ".NET restore failed." }
& dotnet publish (Join-Path $repositoryRoot "gui\WinAnanicyGui.csproj") `
    -c Release `
    --no-restore `
    --self-contained true `
    -r win-x64 `
    -o $applicationDirectory `
    "-p:Version=$Version" `
    "-p:AssemblyVersion=$Version.0" `
    "-p:FileVersion=$Version.0"
if ($LASTEXITCODE -ne 0) { throw ".NET publish failed." }

$coreCandidates = @(
    (Join-Path $coreBuildDirectory "win-ananicy.exe"),
    (Join-Path $coreBuildDirectory "Release\win-ananicy.exe")
)
$coreExecutable = $coreCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $coreExecutable) {
    throw "The C++ engine executable was not produced."
}
Copy-Item -LiteralPath $coreExecutable -Destination (Join-Path $applicationDirectory "win-ananicy.exe") -Force

$dataDirectory = Join-Path $applicationDirectory "data"
New-Item -ItemType Directory -Force -Path $dataDirectory | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot "rules.json") -Destination (Join-Path $dataDirectory "rules.json") -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot "settings.json") -Destination (Join-Path $dataDirectory "settings.json") -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot "rules.example.json") -Destination (Join-Path $dataDirectory "rules.example.json") -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot "README.md") -Destination (Join-Path $applicationDirectory "README.md") -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot "LICENSE") -Destination (Join-Path $applicationDirectory "LICENSE.txt") -Force

$portableArchive = Join-Path $artifactsDirectory "WinAnanicy-$Version-Portable.zip"
Compress-Archive -Path (Join-Path $applicationDirectory "*") -DestinationPath $portableArchive -CompressionLevel Optimal

if (-not $SkipInstaller) {
    $isccCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
        (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe")
    )
    $iscc = $isccCandidates | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_)
    } | Select-Object -First 1
    if (-not $iscc) {
        throw "Inno Setup 6 was not found. Install it or run with -SkipInstaller."
    }
    & $iscc `
        "/DAppVersion=$Version" `
        "/DSourceDir=$applicationDirectory" `
        "/DOutputDir=$artifactsDirectory" `
        (Join-Path $repositoryRoot "installer\WinAnanicy.iss")
    if ($LASTEXITCODE -ne 0) { throw "Installer compilation failed." }
}

$checksumLines = Get-ChildItem -LiteralPath $artifactsDirectory -File |
    Where-Object { $_.Extension -in ".exe", ".zip" } |
    Sort-Object Name |
    ForEach-Object {
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash *$($_.Name)"
    }
$checksumLines | Set-Content -LiteralPath (Join-Path $artifactsDirectory "SHA256SUMS.txt") -Encoding ascii

Write-Host ""
Write-Host "WinAnanicy $Version release artifacts:"
Get-ChildItem -LiteralPath $artifactsDirectory -File |
    Select-Object Name, Length, LastWriteTime
