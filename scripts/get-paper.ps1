param(
    [string]$McVersion = "",
    [string]$Channel = "STABLE"
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\config.ps1"

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ua = @{ "User-Agent" = "BanditVault-JavaServerUWP/1.0" }
$api = "https://fill.papermc.io/v3/projects/paper"

if (-not $McVersion) { $McVersion = $MC_VERSION }

function Get-Builds($ver) {
    try {
        return Invoke-RestMethod -Uri "$api/versions/$ver/builds" -Headers $ua
    } catch {
        return $null
    }
}

$builds = Get-Builds $McVersion
if (-not $builds) {
    Write-Warning "No Paper builds for $McVersion. Discovering available versions..."
    try {
        $proj = Invoke-RestMethod -Uri $api -Headers $ua
        $verList = @()
        foreach ($v in $proj.versions) {
            if ($v -is [string]) { $verList += $v }
            elseif ($v.version) { $verList += $v.version }
            elseif ($v.id) { $verList += $v.id }
        }
        if (-not $verList -and $proj.versions.PSObject.Properties) {
            $verList = $proj.versions.PSObject.Properties.Name
        }
        if ($verList) {
            $McVersion = $verList | Select-Object -Last 1
            Write-Host "Falling back to newest Paper version: $McVersion"
            $builds = Get-Builds $McVersion
        }
    } catch {
        Write-Error "Could not reach Paper API to discover versions: $_"
        exit 1
    }
}
if (-not $builds) {
    Write-Error "No Paper builds available. Pass a known version, e.g. .\scripts\get-paper.ps1 -McVersion 1.21.8"
    exit 1
}

# api order is not the contract
$stable = $builds | Where-Object { $_.channel -ieq $Channel } | Sort-Object { [int]$_.id } -Descending
$chosen = if ($stable) { @($stable)[0] } else { @($builds | Sort-Object { [int]$_.id } -Descending)[0] }
if (-not $stable) {
    Write-Warning "No $Channel build for $McVersion, using newest build #$($chosen.id) (channel $($chosen.channel))"
}

$dl = $chosen.downloads.'server:default'
if (-not $dl -or -not $dl.url) {
    Write-Error "Build #$($chosen.id) has no server:default download"
    exit 1
}

if (-not (Test-Path $ServerJarDir)) { New-Item -ItemType Directory -Force -Path $ServerJarDir | Out-Null }
$jarPath = Join-Path $ServerJarDir "paper.jar"
Write-Host "Downloading Paper $McVersion build #$($chosen.id) -> paper.jar"
Invoke-WebRequest -Uri $dl.url -Headers $ua -OutFile $jarPath

$want = $null
if ($dl.checksums -and $dl.checksums.sha256) { $want = $dl.checksums.sha256 }
if ($want) {
    $got = (Get-FileHash -Algorithm SHA256 -Path $jarPath).Hash.ToLower()
    if ($got -ne $want.ToLower()) {
        Write-Error "sha256 mismatch: got $got want $want"
        exit 1
    }
    Write-Host "sha256 verified"
}

# paperclip main class can move
Add-Type -AssemblyName System.IO.Compression.FileSystem
$mainClass = $null
$zip = [System.IO.Compression.ZipFile]::OpenRead($jarPath)
try {
    $entry = $zip.Entries | Where-Object { $_.FullName -eq "META-INF/MANIFEST.MF" } | Select-Object -First 1
    if ($entry) {
        $reader = New-Object System.IO.StreamReader($entry.Open())
        try { $manifest = $reader.ReadToEnd() } finally { $reader.Close() }
        foreach ($line in $manifest -split "`r?`n") {
            if ($line -match '^Main-Class:\s*(.+?)\s*$') { $mainClass = $Matches[1]; break }
        }
    }
} finally { $zip.Dispose() }

if (-not $mainClass) {
    Write-Warning "Main-Class not found in manifest, defaulting to io.papermc.paperclip.Main"
    $mainClass = "io.papermc.paperclip.Main"
}

Set-Content -Path (Join-Path $ServerJarDir "mainclass.txt") -Value $mainClass -NoNewline
Set-Content -Path (Join-Path $ServerJarDir "paper-version.txt") -Value "$McVersion build $($chosen.id)" -NoNewline

Write-Host ""
Write-Host "Paper ready: $McVersion build #$($chosen.id)"
Write-Host "  jar:        $jarPath"
Write-Host "  Main-Class: $mainClass"
exit 0
