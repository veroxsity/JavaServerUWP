param(
    [switch]$SkipServer = $false,
    [switch]$SkipFabric = $false
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\config.ps1"
. "$PSScriptRoot\common.ps1"

Write-Host "=== Downloading Server Libraries ==="

Ensure-DirectoryTree $LibsDir
Ensure-DirectoryTree $ServerJarDir

if (-not $SkipFabric) {
    Write-Host "Downloading Fabric Loader $FABRIC_LOADER_VERSION..."

    $fabricGroup = $FABRIC_LOADER_MAVEN.Split(':')[0].Replace('.', '/')
    $fabricArtifact = $FABRIC_LOADER_MAVEN.Split(':')[1]
    $fabricVer = $FABRIC_LOADER_MAVEN.Split(':')[2]

    $fabricBase = "$FABRIC_MAVEN_URL/$fabricGroup/$fabricArtifact/$fabricVer"
    $fabricLibDir = Join-Path $LibsDir "net\fabricmc\$fabricArtifact\$fabricVer"
    Ensure-DirectoryTree $fabricLibDir

    $loaderJar = "$fabricArtifact-$fabricVer.jar"
    $loaderUrl = "$fabricBase/$loaderJar"
    $loaderOut = Join-Path $fabricLibDir $loaderJar
    if (-not (Test-Path $loaderOut)) {
        Write-Host "  Fetching $loaderUrl"
        Invoke-WebRequest -Uri $loaderUrl -OutFile $loaderOut -UseBasicParsing
    } else {
        Write-Host "  Already cached: $loaderJar"
    }

    $loaderSrcJar = "$fabricArtifact-$fabricVer-sources.jar"
    $loaderSrcUrl = "$fabricBase/$loaderSrcJar"
    $loaderSrcOut = Join-Path $fabricLibDir $loaderSrcJar
    if (-not (Test-Path $loaderSrcOut)) {
        Write-Host "  Fetching $loaderSrcUrl"
        try {
            Invoke-WebRequest -Uri $loaderSrcUrl -OutFile $loaderSrcOut -UseBasicParsing
        } catch {
            Write-Warning "  Sources JAR not available; LoaderUtil patch will need manual JAR"
        }
    }

    $intermediaryGroup = "net.fabricmc"
    $intermediaryGroupPath = $intermediaryGroup.Replace('.', '/')
    $intermediaryArtifact = "intermediary"
    $intermediaryBase = "$FABRIC_MAVEN_URL/$intermediaryGroupPath/$intermediaryArtifact/$INTERMEDIARY_VERSION"
    $intermediaryLibDir = Join-Path $LibsDir "$intermediaryGroupPath\$intermediaryArtifact\$INTERMEDIARY_VERSION"
    Ensure-DirectoryTree $intermediaryLibDir

    $intermediaryJar = "$intermediaryArtifact-$INTERMEDIARY_VERSION.jar"
    $intermediaryUrl = "$intermediaryBase/$intermediaryJar"
    $intermediaryOut = Join-Path $intermediaryLibDir $intermediaryJar
    if (-not (Test-Path $intermediaryOut)) {
        Write-Host "  Fetching $intermediaryUrl"
        Invoke-WebRequest -Uri $intermediaryUrl -OutFile $intermediaryOut -UseBasicParsing
    } else {
        Write-Host "  Already cached: $intermediaryJar"
    }
}

if (-not $SkipServer) {
    Write-Host "Downloading Minecraft Server $MC_VERSION..."

    $serverOut = Join-Path $ServerJarDir "server-$MC_VERSION.jar"

    if (-not (Test-Path $serverOut)) {

        try {
            $versionManifestUrl = "$MC_META_URL/mc/game/version_manifest_v2.json"
            Write-Host "  Fetching version manifest..."
            $manifest = Invoke-RestMethod -Uri $versionManifestUrl

            $versionEntry = $manifest.versions | Where-Object { $_.id -eq $MC_VERSION }
            if (-not $versionEntry) {
                throw "Minecraft version $MC_VERSION not found in manifest"
            }

            $versionJson = Invoke-RestMethod -Uri $versionEntry.url
            $serverDownload = $versionJson.downloads.server

            if (-not $serverDownload) {
                throw "No server download available for version $MC_VERSION"
            }

            Write-Host "  Downloading from $($serverDownload.url)"
            Invoke-WebRequest -Uri $serverDownload.url -OutFile $serverOut -UseBasicParsing
            Write-Host "  Downloaded $(($serverDownload.size / 1MB).ToString('F1')) MB"
        } catch {
            Write-Warning "  Could not download server JAR: $_"
            Write-Warning "  Place server-$MC_VERSION.jar in $ServerJarDir manually"
        }
    } else {
        Write-Host "  Already cached: server-$MC_VERSION.jar"
    }
}

if (-not $SkipFabric) {
    Write-Host "Downloading Mixin..."

    $mixinGroup = "net.fabricmc"
    $mixinGroupPath = $mixinGroup.Replace('.', '/')
    $mixinArtifact = "sponge-mixin"
    $mixinVer = "0.17.2+mixin.0.8.7"
    $mixinBase = "$FABRIC_MAVEN_URL/$mixinGroupPath/$mixinArtifact/$mixinVer"
    $mixinLibDir = Join-Path $LibsDir "$mixinGroupPath\$mixinArtifact\$mixinVer"
    Ensure-DirectoryTree $mixinLibDir

    $mixinJar = "$mixinArtifact-$mixinVer.jar"
    $mixinUrl = "$mixinBase/$mixinJar"
    $mixinOut = Join-Path $mixinLibDir $mixinJar
    if (-not (Test-Path $mixinOut)) {
        Write-Host "  Fetching $mixinUrl"
        try {
            Invoke-WebRequest -Uri $mixinUrl -OutFile $mixinOut -UseBasicParsing
        } catch {
            Write-Warning "  Mixin download failed; compat mod may not compile"
        }
    } else {
        Write-Host "  Already cached: $mixinJar"
    }
}

# fabric loader probes asm at runtime
Write-Host "Downloading ASM..."
$asmVer = "9.9"
$asmBase = "https://repo1.maven.org/maven2/org/ow2/asm"
$asmArtifacts = @("asm", "asm-tree", "asm-commons", "asm-util", "asm-analysis")

foreach ($artifact in $asmArtifacts) {
    $asmDir = Join-Path $LibsDir "org\ow2\asm\$artifact\$asmVer"
    Ensure-DirectoryTree $asmDir
    $asmJar = "$artifact-$asmVer.jar"
    $asmUrl = "$asmBase/$artifact/$asmVer/$asmJar"
    $asmOut = Join-Path $asmDir $asmJar
    if (-not (Test-Path $asmOut)) {
        Write-Host "  Fetching $asmUrl"
        Invoke-WebRequest -Uri $asmUrl -OutFile $asmOut -UseBasicParsing
    } else {
        Write-Host "  Already cached: $asmJar"
    }
}

Write-Host "=== Downloads complete ==="
