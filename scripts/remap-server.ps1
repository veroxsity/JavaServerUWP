param([Parameter(Mandatory=$true)][string]$StagingDir)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\config.ps1"
. "$PSScriptRoot\common.ps1"

Write-Host "=== Pre-remapping server jar ==="

$javaInfo = Resolve-Java
if (-not $javaInfo) {
    Write-Error "Java JDK required for pre-remap"
    exit 1
}

$workDir = Join-Path $OutputDir "remap-work"
if (Test-Path $workDir) { Remove-Item -Recurse -Force $workDir }
Ensure-DirectoryTree $workDir

# runtime remap deadlocks in the uwp sandbox
$intermediaryJar = Join-Path $LibsDir "net\fabricmc\intermediary\$MC_VERSION\intermediary-$MC_VERSION.jar"
if (-not (Test-Path $intermediaryJar)) {
    Write-Error "intermediary jar not found: $intermediaryJar"
    exit 1
}
Write-Host "Extracting mappings.tiny from $intermediaryJar"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$mappingsPath = Join-Path $workDir "mappings.tiny"
$zip = [System.IO.Compression.ZipFile]::OpenRead($intermediaryJar)
try {
    $entry = $zip.Entries | Where-Object { $_.FullName -eq "mappings/mappings.tiny" } | Select-Object -First 1
    if (-not $entry) {
        Write-Error "mappings/mappings.tiny not found inside intermediary jar"
        exit 1
    }
    $in = $entry.Open()
    $out = [System.IO.File]::Create($mappingsPath)
    try { $in.CopyTo($out) } finally { $out.Close(); $in.Close() }
} finally {
    $zip.Dispose()
}
$mapBytes = (Get-Item $mappingsPath).Length
Write-Host "  mappings.tiny: $mapBytes bytes"

$fabricLoaderJar = Join-Path $LibsDir "net\fabricmc\fabric-loader\$FABRIC_LOADER_VERSION\fabric-loader-$FABRIC_LOADER_VERSION.jar"
$asmJars = Get-ChildItem (Join-Path $LibsDir "org\ow2\asm") -Recurse -Filter "*.jar" -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch 'sources|javadoc' }
$cpEntries = @($fabricLoaderJar) + ($asmJars | ForEach-Object { $_.FullName })
$classpath = $cpEntries -join ';'

$helperSrc = Join-Path $ProjectRoot "patch\RemapHelper.java"
if (-not (Test-Path $helperSrc)) {
    Write-Error "RemapHelper.java not found: $helperSrc"
    exit 1
}
$classesDir = Join-Path $workDir "classes"
Ensure-DirectoryTree $classesDir
Write-Host "Compiling RemapHelper.java..."
& $javaInfo.JavacExe --release 21 -cp $classpath -d $classesDir $helperSrc
if ($LASTEXITCODE -ne 0) {
    Write-Error "RemapHelper compile failed"
    exit 1
}

$inputJar = Join-Path $StagingDir "server\server.jar"
if (-not (Test-Path $inputJar)) {
    Write-Error "Staged server.jar not found: $inputJar"
    exit 1
}
$inputBytes = (Get-Item $inputJar).Length
Write-Host "Input jar: $inputJar ($inputBytes bytes)"

$outputDir = Join-Path $StagingDir "prebuilt-remap"
Ensure-DirectoryTree $outputDir
$outputJar = Join-Path $outputDir "server-intermediary.jar"

$runCp = "$classesDir;$classpath"
Write-Host "Running RemapHelper..."
& $javaInfo.JavaExe -cp $runCp banditvault.uwpremap.RemapHelper $inputJar $mappingsPath $outputJar
if ($LASTEXITCODE -ne 0) {
    Write-Error "RemapHelper run failed"
    exit 1
}

if (-not (Test-Path $outputJar)) {
    Write-Error "RemapHelper exited 0 but did not produce output: $outputJar"
    exit 1
}

$outBytes = (Get-Item $outputJar).Length
Write-Host "  Output: $outputJar ($outBytes bytes)"
Write-Host "=== Pre-remap complete ==="
