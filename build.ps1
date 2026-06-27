param(
    [switch]$Clean = $false,
    [switch]$SkipDownload = $false,
    [switch]$Paper = $false,
    [string]$McVersion = "",
    [string]$AppxVersion = "",
    [switch]$Debug = $false
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\scripts\config.ps1"
. "$PSScriptRoot\scripts\common.ps1"

$StartTime = Get-Date

function Assert-AppxVersion {
    param([Parameter(Mandatory = $true)][string]$Version)
    $parts = $Version -split '\.'
    if ($parts.Count -ne 4) { throw "APPX version must have four numeric fields: $Version" }
    foreach ($part in $parts) {
        $value = 0
        if (-not [int]::TryParse($part, [ref]$value) -or $value -lt 0 -or $value -gt 65535) {
            throw "APPX version field is out of range 0..65535: $Version"
        }
    }
}

# xbox refuses same-version appx upgrades
$manifestSourcePath = Join-Path $ServerDir "Package.appxmanifest"
[xml]$sourceManifest = Get-Content $manifestSourcePath
$baseVersionParts = ([string]$sourceManifest.Package.Identity.Version) -split '\.'
if ($baseVersionParts.Count -ne 4) { throw "Package.appxmanifest Identity Version must have four numeric fields." }
$appVersionBase = "$($baseVersionParts[0]).$($baseVersionParts[1]).$($baseVersionParts[2])"
$verFile = Join-Path $ProjectRoot ".local\app_build.txt"
if ($AppxVersion) {
    Assert-AppxVersion $AppxVersion
    $appVersion = $AppxVersion
} else {
    $verRev = [int]$baseVersionParts[3]
    if (Test-Path $verFile) {
        $prevParts = ((Get-Content $verFile -Raw).Trim()) -split '\.'
        $prevBase = if ($prevParts.Count -eq 4) { "$($prevParts[0]).$($prevParts[1]).$($prevParts[2])" } else { "" }
        if ($prevBase -eq $appVersionBase) { $verRev = [int]$prevParts[3] + 1 }
    }
    if ($verRev -gt 65535) { $verRev = 65535 }
    $appVersion = "$appVersionBase.$verRev"
}
New-Item -ItemType Directory -Force -Path (Split-Path $verFile) | Out-Null
Set-Content -Path $verFile -Value $appVersion -NoNewline

$serverType = if ($Paper) { "Paper" } else { "Fabric $FABRIC_LOADER_VERSION" }

Write-Host @"

========================================
  JavaServerUWP Build Pipeline
  MC $MC_VERSION | $serverType
========================================

"@

if ($Clean) {
    & "$PSScriptRoot\scripts\clean.ps1"
}

Write-Host "[1/8] Resolving toolchain..."

$msvc = Resolve-MSVC
if (-not $msvc) { exit 1 }

$sdk = Resolve-WindowsSdk
if (-not $sdk) { exit 1 }

$java = Resolve-Java
if (-not $java) {
    Write-Warning "Java not found - compat mod build will be skipped"
}

Write-Host ""

if (-not $SkipDownload) {
    Write-Host "[2/8] Downloading dependencies..."
    if ($Paper) {
        $paperArgs = @{}
        if ($McVersion) { $paperArgs.McVersion = $McVersion }
        & "$PSScriptRoot\scripts\get-paper.ps1" @paperArgs
        if (-not (Test-Path (Join-Path $ServerJarDir "paper.jar"))) {
            Write-Error "Paper download failed (no paper.jar produced)"
            exit 1
        }
    } else {
        & "$PSScriptRoot\scripts\download-libs.ps1"
    }
    Write-Host ""
}

Write-Host "[3/8] Generating runtime_config.h..."

$template = Get-Content "$ServerDir\runtime_config.h.in" -Raw
$generated = $template `
    -replace '@MC_VERSION@', $MC_VERSION `
    -replace '@FABRIC_LOADER_VERSION@', $FABRIC_LOADER_VERSION

$runtimeConfigOut = Join-Path $OutputDir "gen\runtime_config.h"
Ensure-DirectoryTree (Split-Path $runtimeConfigOut -Parent)
Set-Content -Path $runtimeConfigOut -Value $generated
Write-Host "  -> $runtimeConfigOut"
Write-Host ""

Write-Host "[4/8] Building compat mod..."

if ($Paper) {
    Write-Host "  Skipped (Paper needs no Fabric compat mod)"
} elseif ($java) {
    & "$PSScriptRoot\scripts\build_compat_mod.ps1"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Compat mod build failed (exit code $LASTEXITCODE). Halting."
        exit 1
    }
} else {
    Write-Warning "Skipping compat mod (no Java)"
}
Write-Host ""

Write-Host "[5/8] Patching Fabric Loader..."
if ($Paper) {
    Write-Host "  Skipped (Paper does not use Fabric Loader)"
} else {
    & "$PSScriptRoot\scripts\patch-fabric.ps1"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Fabric Loader patch failed (exit code $LASTEXITCODE). Halting."
        exit 1
    }
}
Write-Host ""

Write-Host "[6/8] Compiling MC.Server.exe..."

$exeOutputDir = Join-Path $OutputDir "bin"
$genDir = Join-Path $OutputDir "gen"
Ensure-DirectoryTree $exeOutputDir

$exeFile = Join-Path $exeOutputDir "MC.Server.exe"
$srcFiles = @(Get-ChildItem -Path $ServerDir -Filter '*.cpp' | Sort-Object Name | ForEach-Object { $_.FullName })

if ($srcFiles.Count -eq 0) {
    Write-Error "No .cpp source files found in $ServerDir"
    exit 1
}

# powershell 5.1 misparses long interpolated path strings here
$msvcRoot   = $msvc.MsvcRoot
$sdkInclude = $sdk.IncludeDir
$sdkLib     = $sdk.LibDir

$includeParts = @(
    $genDir,
    (Join-Path $msvcRoot 'include'),
    (Join-Path $sdkInclude 'ucrt'),
    (Join-Path $sdkInclude 'shared'),
    (Join-Path $sdkInclude 'um'),
    (Join-Path $sdkInclude 'winrt'),
    (Join-Path $sdkInclude 'cppwinrt')
)
if ($java) {
    $includeParts += (Join-Path $java.JavaHome 'include')
    $includeParts += (Join-Path $java.JavaHome 'include\win32')
}
$env:INCLUDE = ($includeParts -join ';')

$libParts = @(
    (Join-Path $msvcRoot 'lib\x64'),
    (Join-Path $sdkLib 'ucrt\x64'),
    (Join-Path $sdkLib 'um\x64')
)
$env:LIB = ($libParts -join ';')

$serverTypeDefine = @()
if ($Paper) { $serverTypeDefine = @('/DSERVER_TYPE_PAPER') }
Push-Location $exeOutputDir
& $msvc.ClExe @srcFiles @serverTypeDefine /std:c++17 /EHsc /W3 /O2 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX `
    /DWINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP `
    /link /SUBSYSTEM:WINDOWS /MACHINE:X64 `
    /OUT:"$exeFile" `
    kernel32.lib user32.lib shell32.lib runtimeobject.lib windowsapp.lib ole32.lib oleaut32.lib `
    d2d1.lib dwrite.lib dxgi.lib d3d11.lib windowscodecs.lib
if ($LASTEXITCODE -ne 0) { Write-Error "Compilation failed"; Pop-Location; exit 1 }
Pop-Location

Write-Host "  -> $exeFile"
Write-Host ""

Write-Host "[7/8] Assembling package content..."

$stagingDir = Join-Path $OutputDir "staging\package"
$certDir = Join-Path $OutputDir "certs"

Remove-Item -Recurse -Force $stagingDir -ErrorAction SilentlyContinue
Ensure-DirectoryTree $stagingDir
Ensure-DirectoryTree (Join-Path $stagingDir "Assets")
Ensure-DirectoryTree $certDir

Write-Host "  Generating tile assets..."
& "$PSScriptRoot\scripts\generate-assets.ps1" -OutputDir (Join-Path $stagingDir "Assets")

Copy-Item $exeFile (Join-Path $stagingDir "MC.Server.exe")

$manifestText = [System.IO.File]::ReadAllText($manifestSourcePath)
$manifestText = [regex]::Replace($manifestText, '(<Identity\b[^>]*\bVersion=")\d+\.\d+\.\d+\.\d+(")', ('${1}' + $appVersion + '${2}'))
[System.IO.File]::WriteAllText((Join-Path $stagingDir "AppxManifest.xml"), $manifestText)
Write-Host "  App package version: $appVersion"

if ($java) {
    Write-Host "  Copying JRE from $($java.JavaHome)..."
    $stagingJre = Join-Path $stagingDir "jre"
    Copy-Item -Recurse $java.JavaHome $stagingJre

    $jreExtras = @(
        "src.zip", "man", "include", "lib\src.zip",
        "bin\jmc.exe", "bin\javap.exe", "bin\jdeprscan.exe",
        "bin\jwebserver.exe", "bin\jpackage.exe", "bin\jstatd.exe",
        "bin\jconsole.exe", "bin\jhsdb.exe", "bin\jimage.exe",
        "bin\jlink.exe", "bin\jmod.exe", "bin\keytool.exe",
        "bin\kinit.exe", "bin\klist.exe", "bin\ktab.exe",
        "bin\rmiregistry.exe", "bin\serialver.exe"
    )
    foreach ($extra in $jreExtras) {
        $extraPath = Join-Path $stagingJre $extra
        if (Test-Path $extraPath) {
            Remove-Item -Recurse -Force $extraPath -ErrorAction SilentlyContinue
        }
    }
}

$stagingLibs = Join-Path $stagingDir "libraries"
if ((-not $Paper) -and (Test-Path $LibsDir)) {
    Write-Host "  Copying libraries..."
    Copy-Item -Recurse $LibsDir $stagingLibs
}

# fabric remap deadlocks in the uwp sandbox so ship the inner jar instead
$stagingServerDir = Join-Path $stagingDir "server"
Ensure-DirectoryTree $stagingServerDir

if ($Paper) {
    $paperJar = Join-Path $ServerJarDir "paper.jar"
    if (-not (Test-Path $paperJar)) {
        Write-Error "Paper jar not found: $paperJar (run get-paper.ps1, or build without -SkipDownload)"
        exit 1
    }
    Copy-Item $paperJar (Join-Path $stagingServerDir "paper.jar") -Force
    foreach ($meta in @("mainclass.txt", "paper-version.txt")) {
        $metaSrc = Join-Path $ServerJarDir $meta
        if (Test-Path $metaSrc) { Copy-Item $metaSrc (Join-Path $stagingServerDir $meta) -Force }
    }
    Write-Host "  Staged paper.jar"

    # paperclip hits jdk zipfs torealpath denied under uwp
    $zipfsSrc = Join-Path $ProjectRoot "patch\jdk.zipfs"
    $zipfsOut = Join-Path $stagingDir "jdk-patch\jdk.zipfs"
    Ensure-DirectoryTree $zipfsOut
    & $java.JavacExe --patch-module "jdk.zipfs=$zipfsSrc" -d $zipfsOut (Join-Path $zipfsSrc "jdk\nio\zipfs\ZipFileSystemProvider.java")
    if ($LASTEXITCODE -ne 0) { Write-Error "jdk.zipfs patch compile failed"; exit 1 }
    Write-Host "  Staged patched jdk.zipfs"

    # world storage hits windowspath torealpath also denied under uwp
    $javaBaseSrc = Join-Path $ProjectRoot "patch\java.base"
    $javaBaseOut = Join-Path $stagingDir "jdk-patch\java.base"
    Ensure-DirectoryTree $javaBaseOut
    & $java.JavacExe --patch-module "java.base=$javaBaseSrc" -d $javaBaseOut (Join-Path $javaBaseSrc "sun\nio\fs\WindowsPath.java")
    if ($LASTEXITCODE -ne 0) { Write-Error "java.base patch compile failed"; exit 1 }
    Write-Host "  Staged patched java.base"

    # uwp blocks dll loads from writable temp dirs
    & "$PSScriptRoot\scripts\get-jnidispatch.ps1" -PaperJar $paperJar -OutDir $ServerJarDir
    if ($LASTEXITCODE -ne 0) { Write-Error "jnidispatch fetch failed"; exit 1 }
    $jnaStage = Join-Path $stagingDir "jna"
    Ensure-DirectoryTree $jnaStage
    Copy-Item (Join-Path $ServerJarDir "jnidispatch.dll") (Join-Path $jnaStage "jnidispatch.dll") -Force
    Write-Host "  Staged jnidispatch.dll"

    # system.exit would kill the native host
    $exitTrapSrc = Join-Path $ProjectRoot "compat_mod\src\main\java\banditvault\xboxcompat\ExitTrap.java"
    $exitTrapOut = Join-Path $stagingDir "exittrap"
    Ensure-DirectoryTree $exitTrapOut
    & $java.JavacExe -d $exitTrapOut $exitTrapSrc
    if ($LASTEXITCODE -ne 0) { Write-Error "ExitTrap compile failed"; exit 1 }
    Write-Host "  Staged exit trap"
} else {
    $serverJarSrc = Join-Path $ServerJarDir "server-$MC_VERSION.jar"
    if (-not (Test-Path $serverJarSrc)) {
        Write-Error "Server jar not found: $serverJarSrc"
        exit 1
    }
    $innerEntry = "META-INF/versions/$MC_VERSION/server-$MC_VERSION.jar"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($serverJarSrc)
    try {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq $innerEntry } | Select-Object -First 1
        if (-not $entry) {
            Write-Error "Could not find inner server jar entry '$innerEntry' inside $serverJarSrc"
            exit 1
        }
        $outPath = Join-Path $stagingServerDir "server.jar"
        if (Test-Path $outPath) { Remove-Item $outPath -Force }
        $in = $entry.Open()
        $out = [System.IO.File]::Create($outPath)
        try { $in.CopyTo($out) } finally { $out.Close(); $in.Close() }
        Write-Host "  Staged server.jar (inner jar extracted from bundled server-$MC_VERSION.jar, $($entry.Length) bytes)"
    } finally {
        $zip.Dispose()
    }

    if ($java) {
        & "$PSScriptRoot\scripts\remap-server.ps1" -StagingDir $stagingDir
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Server jar pre-remap failed (exit code $LASTEXITCODE). Halting."
            exit 1
        }
    } else {
        Write-Warning "Skipping server jar pre-remap (no Java); runtime remap will be attempted instead and will likely fail under UWP"
    }

    $compatModJar = Join-Path $OutputDir "compat_mod\banditvault-xbox-compat-1.0.0.jar"
    if (Test-Path $compatModJar) {
        Ensure-DirectoryTree (Join-Path $stagingDir "bundled-mods")
        Copy-Item $compatModJar (Join-Path $stagingDir "bundled-mods\")
        Write-Host "  Copied compat mod"
    }
}

Ensure-DirectoryTree (Join-Path $stagingDir "log_configs")
$logConfigSrc = Join-Path $ProjectRoot "log_configs\server-uwp.xml"
if (Test-Path $logConfigSrc) {
    Copy-Item $logConfigSrc (Join-Path $stagingDir "log_configs\server-uwp.xml")
}

# java accepts the full security override most reliably inside jre conf
$secPropsSrc = Join-Path $ProjectRoot "xbox_security.properties"
if (Test-Path $secPropsSrc) {
    $jreSecDir = Join-Path $stagingDir "jre\conf\security"
    Ensure-DirectoryTree $jreSecDir
    Copy-Item $secPropsSrc (Join-Path $jreSecDir "xbox.properties") -Force
}

Write-Host "  Package content assembled"
Write-Host ""

Write-Host "[8/8] Packaging APPX..."

$appx = Join-Path $OutputDir ("MC.Server_{0}.appx" -f $appVersion)

$makeappx = Get-ChildItem "$($sdk.SDKPath)bin\$($sdk.SDKVersion)\x64\makeappx.exe", "$($sdk.SDKPath)bin\10.0.26100.0\x64\makeappx.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if (-not $makeappx) {
    $cmd = Get-Command makeappx -ErrorAction SilentlyContinue
    if ($cmd) { $makeappx = $cmd.Source }
}
if (-not $makeappx) { Write-Error "makeappx.exe not found. Add Windows SDK bin to PATH."; exit 1 }

$signtool = Get-ChildItem "$($sdk.SDKPath)bin\$($sdk.SDKVersion)\x64\signtool.exe", "$($sdk.SDKPath)bin\10.0.26100.0\x64\signtool.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if (-not $signtool) { $signtool = "signtool" }

Write-Host "  Packing: $appx"
$makeappxStdout = Join-Path $OutputDir "makeappx.stdout.log"
$makeappxStderr = Join-Path $OutputDir "makeappx.stderr.log"
Remove-Item $makeappxStdout, $makeappxStderr -Force -ErrorAction SilentlyContinue
$makeappxArgs = @("pack", "/d", $stagingDir, "/p", $appx, "/o", "/nv", "/nfv", "/nc", "/np")
$makeappxProc = Start-Process -FilePath $makeappx -ArgumentList $makeappxArgs `
    -NoNewWindow -RedirectStandardOutput $makeappxStdout -RedirectStandardError $makeappxStderr -PassThru
if (-not $makeappxProc.WaitForExit(600000)) {
    Stop-Process -Id $makeappxProc.Id -Force -ErrorAction SilentlyContinue
    Write-Warning "MakeAppx timed out after 10 minutes. Last output:"
    if (Test-Path $makeappxStdout) { Get-Content $makeappxStdout -Tail 30 }
    if (Test-Path $makeappxStderr) { Get-Content $makeappxStderr -Tail 30 }
    Write-Error "MakeAppx timed out"
    exit 1
}
if (-not ((Test-Path $appx) -and (($makeappxProc.ExitCode -eq 0) -or ((Get-Content $makeappxStdout -Raw -ErrorAction SilentlyContinue) -match 'Package creation succeeded')))) {
    if (Test-Path $makeappxStdout) { Get-Content $makeappxStdout -Tail 30 }
    if (Test-Path $makeappxStderr) { Get-Content $makeappxStderr -Tail 30 }
    Write-Error "MakeAppx failed"
    exit 1
}

$cert = Join-Path $certDir $CertificateFileName
$certName = if ($env:APPX_CERT_SUBJECT) { $env:APPX_CERT_SUBJECT } else { $DefaultCertificateSubject }

if (-not (Test-Path $cert)) {
    Write-Host "  Generating self-signed certificate..."
    $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject $certName `
        -KeyUsage DigitalSignature -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3","2.5.29.19={text}")
    Export-PfxCertificate -Cert $c -FilePath $cert `
        -Password (ConvertTo-SecureString $CertificatePassword -AsPlainText -Force) | Out-Null
    Write-Host "  Certificate generated: $certName"
}

$allCerts = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object {
        $_.HasPrivateKey -and
        ($_.EnhancedKeyUsageList | Where-Object { $_.FriendlyName -eq 'Code Signing' })
    }
$projectCerts = $allCerts | Where-Object { $_.Subject -like '*BanditVault*' } | Sort-Object NotBefore -Descending
$otherCerts = $allCerts | Where-Object { $_.Subject -notlike '*BanditVault*' } | Sort-Object NotBefore -Descending
$signingCerts = @($projectCerts) + @($otherCerts)

if (-not $signingCerts) {
    Write-Error "No code signing certificate found in CurrentUser\My store."
    exit 1
}

$signed = $false
foreach ($c in $signingCerts) {
    & $signtool sign /fd SHA256 /sha1 $c.Thumbprint $appx 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  Signed with: $($c.Subject)"
        $signed = $true
        break
    }
}

if (-not $signed) {
    Write-Warning "  Signing failed with existing certs; generating new one..."
    Remove-Item $cert -Force -ErrorAction SilentlyContinue
    $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject $certName `
        -KeyUsage DigitalSignature -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3","2.5.29.19={text}")
    Export-PfxCertificate -Cert $c -FilePath $cert `
        -Password (ConvertTo-SecureString $CertificatePassword -AsPlainText -Force) | Out-Null
    & $signtool sign /fd SHA256 /sha1 $c.Thumbprint $appx 2>$null
    if ($LASTEXITCODE -ne 0) { Write-Error "Appx signing failed"; exit 1 }
    Write-Host "  Signed with new cert: $certName"
}

Remove-Item -Recurse -Force $stagingDir -ErrorAction SilentlyContinue
Write-Host "  Removed staging directory"
Write-Host ""

$duration = (Get-Date) - $StartTime
Write-Host "========================================"
Write-Host "  Build complete in $($duration.TotalSeconds.ToString('F1'))s"
Write-Host "  EXE:   $exeFile"
Write-Host "  APPX:  $appx"
Write-Host "  Cert:  $cert"
Write-Host "========================================"
