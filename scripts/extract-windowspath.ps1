$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\common.ps1"
$javaHome = $env:JAVA_HOME
if (-not $javaHome) {
    $java = Resolve-Java
    if ($java) { $javaHome = $java.JavaHome }
}
if (-not $javaHome) { throw 'set JAVA_HOME to a jdk with lib\src.zip' }
$src = Join-Path $javaHome 'lib\src.zip'
$entry = 'java.base/sun/nio/fs/WindowsPath.java'
$dest = Join-Path $projectRoot 'patch\java.base\sun\nio\fs\WindowsPath.java'

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($src)
try {
    $e = $zip.Entries | Where-Object { $_.FullName -eq $entry }
    if (-not $e) {
        Write-Output 'ENTRY NOT FOUND, candidates:'
        $zip.Entries | Where-Object { $_.FullName -like '*WindowsPath*' } | ForEach-Object { Write-Output $_.FullName }
    } else {
        New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dest, $true)
        Write-Output ('EXTRACTED ' + $dest + ' bytes=' + $e.Length)
    }
} finally {
    $zip.Dispose()
}
