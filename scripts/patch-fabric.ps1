param()

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\config.ps1"
. "$PSScriptRoot\common.ps1"

Write-Host "=== Patching Fabric Loader ==="

$fabricLoaderDir = Join-Path $LibsDir "net\fabricmc\fabric-loader\$FABRIC_LOADER_VERSION"
$loaderJar = Join-Path $fabricLoaderDir "fabric-loader-$FABRIC_LOADER_VERSION.jar"
$loaderSrcJar = Join-Path $fabricLoaderDir "fabric-loader-$FABRIC_LOADER_VERSION-sources.jar"

$patchSource = Join-Path $ProjectRoot "patch\LoaderUtil.java"
if (-not (Test-Path $patchSource)) {
    Write-Error "Patch source not found: $patchSource"
    exit 1
}

$javaInfo = Resolve-Java
if (-not $javaInfo) {
    Write-Error "Java JDK required for patching Fabric Loader"
    exit 1
}

$patchWorkDir = Join-Path $OutputDir "fabric-patch-work"
Ensure-DirectoryTree $patchWorkDir

# jar catches zip damage that dotnet ziparchive can create
function Test-JarReadable {
    param([string]$JarPath)
    if (-not (Test-Path $JarPath)) { return $false }
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $null = & $javaInfo.JarExe tf $JarPath 2>&1
        $ok = ($LASTEXITCODE -eq 0)
    } catch {
        $ok = $false
    } finally {
        $ErrorActionPreference = $prev
    }
    return $ok
}

if (-not (Test-JarReadable $loaderJar)) {
    Write-Warning "fabric-loader JAR is missing or unreadable by `jar`; re-downloading from FabricMC maven."
    $fabricBase = "$FABRIC_MAVEN_URL/net/fabricmc/fabric-loader/$FABRIC_LOADER_VERSION"
    $loaderUrl = "$fabricBase/fabric-loader-$FABRIC_LOADER_VERSION.jar"
    Ensure-DirectoryTree $fabricLoaderDir
    Remove-Item $loaderJar -Force -ErrorAction SilentlyContinue
    Invoke-WebRequest -Uri $loaderUrl -OutFile $loaderJar -UseBasicParsing
    Write-Host "Refetched: $loaderJar"

    if (-not (Test-JarReadable $loaderJar)) {
        Write-Error "Re-downloaded JAR is still unreadable. Check network / URL: $loaderUrl"
        exit 1
    }
}

if (-not (Test-Path $loaderSrcJar)) {
    Write-Warning "fabric-loader sources JAR missing; re-downloading."
    $fabricBase = "$FABRIC_MAVEN_URL/net/fabricmc/fabric-loader/$FABRIC_LOADER_VERSION"
    $loaderSrcUrl = "$fabricBase/fabric-loader-$FABRIC_LOADER_VERSION-sources.jar"
    Invoke-WebRequest -Uri $loaderSrcUrl -OutFile $loaderSrcJar -UseBasicParsing
}

$srcExtractDir = Join-Path $patchWorkDir "fabric-loader-src"
if (Test-Path $srcExtractDir) {
    Remove-Item -Recurse -Force $srcExtractDir
}
Write-Host "Extracting fabric-loader sources..."
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($loaderSrcJar, $srcExtractDir)

$targetFile = Join-Path $srcExtractDir "net\fabricmc\loader\impl\util\LoaderUtil.java"
$launcherBaseFile = Join-Path $srcExtractDir "net\fabricmc\loader\impl\launch\FabricLauncherBase.java"
if (-not (Test-Path $targetFile)) {
    Write-Error "Could not find LoaderUtil.java in fabric-loader sources"
    exit 1
}
if (-not (Test-Path $launcherBaseFile)) {
    Write-Error "Could not find FabricLauncherBase.java in fabric-loader sources"
    exit 1
}

$originalContent = Get-Content $targetFile -Raw
$bakFile = Join-Path $patchWorkDir "LoaderUtil.java.bak"
Set-Content -Path $bakFile -Value $originalContent

Write-Host "Applying LoaderUtil patch..."
$patchContent = Get-Content $patchSource -Raw
Set-Content -Path $targetFile -Value $patchContent

Write-Host "Applying FabricLauncherBase diagnostics patch..."
$launcherBaseContent = Get-Content $launcherBaseFile -Raw
$launcherBaseContent = $launcherBaseContent.Replace("import org.jetbrains.annotations.VisibleForTesting;`r`n", "")
$launcherBaseContent = $launcherBaseContent.Replace("import org.jetbrains.annotations.VisibleForTesting;`n", "")
$launcherBaseContent = $launcherBaseContent.Replace("	@VisibleForTesting`r`n", "")
$launcherBaseContent = $launcherBaseContent.Replace("	@VisibleForTesting`n", "")
$launcherBaseMarker = "		Log.error(LogCategory.GENERAL, exc.getMainText(), actualExc);"
$launcherBaseReplacement = @'
		Log.error(LogCategory.GENERAL, exc.getMainText(), actualExc);
		try {
			String banditVaultLaunchLog = System.getProperty("banditvault.launchLog");
			if (banditVaultLaunchLog != null && !banditVaultLaunchLog.isEmpty()) {
				java.io.StringWriter banditVaultError = new java.io.StringWriter();
				PrintWriter banditVaultWriter = new PrintWriter(banditVaultError);
				banditVaultWriter.println("[FabricLoader] " + exc.getMainText());
				banditVaultWriter.println(exc.getDisplayedText());
				if (actualExc != null) {
					actualExc.printStackTrace(banditVaultWriter);
				}
				banditVaultWriter.flush();
				java.nio.file.Files.writeString(
						java.nio.file.Path.of(banditVaultLaunchLog),
						banditVaultError.toString(),
						java.nio.file.StandardOpenOption.CREATE,
						java.nio.file.StandardOpenOption.APPEND);
			}
		} catch (Throwable banditVaultLogFailure) {
			banditVaultLogFailure.printStackTrace(System.err);
		}
		System.err.println("[FabricLoader] " + exc.getMainText());
		System.err.println(exc.getDisplayedText());
		if (actualExc != null) {
			actualExc.printStackTrace(System.err);
		}
		System.err.flush();
'@
if (-not $launcherBaseContent.Contains($launcherBaseMarker)) {
    Write-Error "Could not find FabricLauncherBase diagnostics insertion point"
    exit 1
}
$launcherBaseContent = $launcherBaseContent.Replace($launcherBaseMarker, $launcherBaseReplacement)
Set-Content -Path $launcherBaseFile -Value $launcherBaseContent

$classOutputDir = Join-Path $patchWorkDir "classes"
if (Test-Path $classOutputDir) {
    Remove-Item -Recurse -Force $classOutputDir
}
Ensure-DirectoryTree $classOutputDir

Write-Host "Compiling patched Fabric Loader classes..."
$mixinJar = Join-Path $LibsDir "net\fabricmc\sponge-mixin\0.17.2+mixin.0.8.7\sponge-mixin-0.17.2+mixin.0.8.7.jar"
$patchCompileClasspath = @($loaderJar)
if (Test-Path $mixinJar) {
    $patchCompileClasspath += $mixinJar
}
$patchCompileClasspath = $patchCompileClasspath -join ";"
$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$compileOut = & $javaInfo.JavacExe -cp $patchCompileClasspath --release 21 -proc:none -d $classOutputDir $targetFile $launcherBaseFile 2>&1
$ErrorActionPreference = $prev

$classFile = Join-Path $classOutputDir "net\fabricmc\loader\impl\util\LoaderUtil.class"
if (-not (Test-Path $classFile)) {
    Write-Host $compileOut
    Write-Error "Failed to compile patched LoaderUtil.java"
    exit 1
}
$launcherBaseClassFile = Join-Path $classOutputDir "net\fabricmc\loader\impl\launch\FabricLauncherBase.class"
if (-not (Test-Path $launcherBaseClassFile)) {
    Write-Host $compileOut
    Write-Error "Failed to compile patched FabricLauncherBase.java"
    exit 1
}

# small class means the patch source lost fabric loader methods
$classSize = (Get-Item $classFile).Length
if ($classSize -lt 1500) {
    Write-Error "Compiled LoaderUtil.class is only $classSize bytes; the patch source is missing required methods (verifyNotInTargetCl, verifyClasspath, normalizePath, getClassFileName, hasMacOs, hasAwtSupport). Refusing to ship a broken patch."
    exit 1
}
Write-Host "LoaderUtil.class compiled ($classSize bytes)"

$jarExtractDir = Join-Path $patchWorkDir "fabric-loader-jar"
if (Test-Path $jarExtractDir) {
    Remove-Item -Recurse -Force $jarExtractDir
}
Ensure-DirectoryTree $jarExtractDir

Write-Host "Extracting fabric-loader JAR contents..."
Push-Location $jarExtractDir
try {
    & $javaInfo.JarExe xf $loaderJar
    if ($LASTEXITCODE -ne 0) {
        Pop-Location
        Write-Error "Failed to extract fabric-loader JAR"
        exit 1
    }
} finally {
    Pop-Location
}

$jarLoaderUtilPath = Join-Path $jarExtractDir "net\fabricmc\loader\impl\util\LoaderUtil.class"
Copy-Item $classFile $jarLoaderUtilPath -Force
Write-Host "Replaced LoaderUtil.class in extracted JAR contents"

$jarLauncherBasePath = Join-Path $jarExtractDir "net\fabricmc\loader\impl\launch\FabricLauncherBase.class"
Copy-Item $launcherBaseClassFile $jarLauncherBasePath -Force
Write-Host "Replaced FabricLauncherBase.class in extracted JAR contents"

# modified signed classes fail verification
$metaInfDir = Join-Path $jarExtractDir "META-INF"
if (Test-Path $metaInfDir) {
    $sigFiles = Get-ChildItem $metaInfDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '\.(SF|RSA|DSA|EC)$' }
    foreach ($sf in $sigFiles) {
        Write-Host "  Removing signature: META-INF/$($sf.Name)"
        Remove-Item $sf.FullName -Force
    }
    if ($sigFiles.Count -eq 0) {
        Write-Host "  No signature files present"
    }
}

Write-Host "Repacking patched fabric-loader JAR..."
Remove-Item $loaderJar -Force
Push-Location $jarExtractDir
try {
    & $javaInfo.JarExe cMf $loaderJar .
    if ($LASTEXITCODE -ne 0) {
        Pop-Location
        Write-Error "Failed to repack fabric-loader JAR"
        exit 1
    }
} finally {
    Pop-Location
}

if (-not (Test-JarReadable $loaderJar)) {
    Write-Error "Repacked JAR is not readable by `jar`. This is a bug in patch-fabric.ps1."
    exit 1
}

$stalePatched = Join-Path $fabricLoaderDir "fabric-loader-$FABRIC_LOADER_VERSION-patched.jar"
if (Test-Path $stalePatched) {
    Remove-Item $stalePatched -Force
    Write-Host "Removed stale: $stalePatched"
}

Write-Host "Patched fabric-loader (in place, unsigned, repacked): $loaderJar"
Write-Host "=== Fabric patch complete ==="
