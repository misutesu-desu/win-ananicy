[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$englishPath = Join-Path $repositoryRoot "gui\Resources\Strings.en.xaml"
$turkishPath = Join-Path $repositoryRoot "gui\Resources\Strings.tr.xaml"

function Get-ResourceKeys([string]$Path) {
    [xml]$document = Get-Content -Raw -LiteralPath $Path
    return @(
        $document.ResourceDictionary.ChildNodes |
            ForEach-Object {
                $_.GetAttribute(
                    "Key",
                    "http://schemas.microsoft.com/winfx/2006/xaml")
            } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )
}

$englishKeys = Get-ResourceKeys $englishPath
$turkishKeys = Get-ResourceKeys $turkishPath
$difference = Compare-Object $englishKeys $turkishKeys
if ($difference) {
    $difference | Format-Table | Out-String | Write-Error
    throw "English and Turkish localization keys do not match."
}
if ($englishKeys.Count -ne ($englishKeys | Sort-Object -Unique).Count) {
    throw "The English resource dictionary contains duplicate keys."
}
if ($turkishKeys.Count -ne ($turkishKeys | Sort-Object -Unique).Count) {
    throw "The Turkish resource dictionary contains duplicate keys."
}

Write-Host "Localization verified: $($englishKeys.Count) keys in each language."
