param(
    [switch]$All = $false
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\config.ps1"

Write-Host "=== Cleaning build output ==="

if (Test-Path $OutputDir) {
    Remove-Item -Recurse -Force $OutputDir
    Write-Host "Removed: $OutputDir"
}

if ($All) {
    $appxOut = Join-Path $ProjectRoot "MC.Server.appx"
    if (Test-Path $appxOut) {
        Remove-Item -Force $appxOut
        Write-Host "Removed: $appxOut"
    }
}

Write-Host "=== Clean complete ==="
