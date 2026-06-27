param()

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\config.ps1"
. "$PSScriptRoot\common.ps1"

Write-Host "=== Building Compat Mod ==="

$javaInfo = Resolve-Java
if (-not $javaInfo) {
    Write-Error "Java JDK is required to build the compat mod"
    exit 1
}

$compatModSrc = Join-Path $CompatModDir "src\main"
$compatModJava = Join-Path $compatModSrc "java"
$compatModResources = Join-Path $compatModSrc "resources"
$compatModOut = Join-Path $OutputDir "compat_mod"
$compatModBuild = Join-Path $compatModOut "build"
$compatModJar = Join-Path $compatModOut "banditvault-xbox-compat-1.0.0.jar"

Ensure-DirectoryTree $compatModBuild
Ensure-DirectoryTree $compatModOut

$fabricLoaderJar = Join-Path $LibsDir "net\fabricmc\fabric-loader\$FABRIC_LOADER_VERSION\fabric-loader-$FABRIC_LOADER_VERSION-patched.jar"
if (-not (Test-Path $fabricLoaderJar)) {
    $fabricLoaderJar = Join-Path $LibsDir "net\fabricmc\fabric-loader\$FABRIC_LOADER_VERSION\fabric-loader-$FABRIC_LOADER_VERSION.jar"
}
$intermediaryJar = Join-Path $LibsDir "net/fabricmc/intermediary/$INTERMEDIARY_VERSION/intermediary-$INTERMEDIARY_VERSION.jar"
$mixinJar = Join-Path $LibsDir "net/fabricmc/sponge-mixin/0.17.2+mixin.0.8.7/sponge-mixin-0.17.2+mixin.0.8.7.jar"
$serverJar = Join-Path $ServerJarDir "server-$MC_VERSION.jar"

$classpath = @($fabricLoaderJar, $intermediaryJar, $serverJar, $mixinJar) -join ";"

$javaFiles = Get-ChildItem -Path $compatModJava -Recurse -Filter "*.java"
$javaFileArgs = $javaFiles.FullName -join " "

if (-not $javaFiles) {
    Write-Error "No Java source files found in $compatModJava"
    exit 1
}

Write-Host "Compiling $($javaFiles.Count) Java source(s)..."
Write-Host "Classpath:"
foreach ($cp in @($fabricLoaderJar, $intermediaryJar, $serverJar)) {
    $exists = if (Test-Path $cp) { "OK" } else { "MISSING" }
    Write-Host "  [$exists] $cp"
}

# mixin annotation processing needs a fuller compile classpath than runtime mixins do
$javacArgs = @('--release', '21', '-proc:none', '-cp', $classpath, '-d', $compatModBuild) + $javaFiles.FullName
# javac warnings hit stderr; with 2>&1 under EAP=Stop (the CI shell forces it) they throw before the exit-code check, so capture under Continue and gate on $LASTEXITCODE
$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$compileResult = & $javaInfo.JavacExe $javacArgs 2>&1
$ErrorActionPreference = $prev

if ($LASTEXITCODE -ne 0) {
    Write-Host "Compilation failed:" -ForegroundColor Red
    Write-Host $compileResult
    exit 1
}

Write-Host "Compilation successful."

Copy-Item -Path (Join-Path $compatModResources "*") -Destination $compatModBuild -Force

Write-Host "Creating compat mod JAR..."
Push-Location $compatModBuild
try {
    & $javaInfo.JarExe cf $compatModJar *
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to create JAR"
        Pop-Location
        exit 1
    }
} finally {
    Pop-Location
}

Write-Host "Compat mod JAR: $compatModJar"
Write-Host "=== Compat mod build complete ==="
