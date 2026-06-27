param(
    [Parameter(Mandatory = $true)] [string] $PaperJar,
    [Parameter(Mandatory = $true)] [string] $OutDir
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $PaperJar)) { Write-Error "paper jar not found: $PaperJar"; exit 1 }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Add-Type -AssemblyName System.IO.Compression.FileSystem

function Read-ZipEntryText($zipPath, $entryName) {
    $z = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $e = $z.Entries | Where-Object { $_.FullName -eq $entryName } | Select-Object -First 1
        if (-not $e) { return $null }
        $r = New-Object System.IO.StreamReader($e.Open())
        try { return $r.ReadToEnd() } finally { $r.Close() }
    } finally { $z.Dispose() }
}

$listText = Read-ZipEntryText $PaperJar 'META-INF/libraries.list'
if (-not $listText) { Write-Error "could not read META-INF/libraries.list from $PaperJar"; exit 1 }

$jnaLine = ($listText -split "`n") | Where-Object { $_ -match "`tnet\.java\.dev\.jna:jna:" } | Select-Object -First 1
if (-not $jnaLine) { Write-Error "no jna entry in libraries.list"; exit 1 }

$cols = $jnaLine.Trim() -split "`t"
$sha = $cols[0].ToLower()
$coord = $cols[1]
$mavenPath = $cols[2]
$version = ($coord -split ':')[2]

$dllOut = Join-Path $OutDir 'jnidispatch.dll'
$verOut = Join-Path $OutDir 'jna-version.txt'

if ((Test-Path $dllOut) -and (Test-Path $verOut) -and ((Get-Content $verOut -Raw).Trim() -eq $version)) {
    Write-Host "  jnidispatch.dll cached (jna $version)"
    exit 0
}

$url = "https://repo.maven.apache.org/maven2/$mavenPath"
$tmpJar = Join-Path $OutDir 'jna-download.jar'
Write-Host "  Downloading jna $version for jnidispatch.dll"
Invoke-WebRequest -Uri $url -OutFile $tmpJar -UseBasicParsing

$actual = (Get-FileHash -Algorithm SHA256 $tmpJar).Hash.ToLower()
if ($actual -ne $sha) { Remove-Item $tmpJar -Force; Write-Error "jna sha mismatch: got $actual expected $sha"; exit 1 }

$entryName = 'com/sun/jna/win32-x86-64/jnidispatch.dll'
$z = [System.IO.Compression.ZipFile]::OpenRead($tmpJar)
try {
    $e = $z.Entries | Where-Object { $_.FullName -eq $entryName } | Select-Object -First 1
    if (-not $e) { Write-Error "no $entryName in jna jar"; exit 1 }
    if (Test-Path $dllOut) { Remove-Item $dllOut -Force }
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dllOut, $true)
} finally {
    $z.Dispose()
}
Remove-Item $tmpJar -Force
Set-Content -Path $verOut -Value $version -NoNewline
Write-Host "  Extracted jnidispatch.dll (jna $version)"
exit 0
