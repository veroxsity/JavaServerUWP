function Resolve-MSVC {

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        Write-Error "vswhere.exe not found. Install Visual Studio with 'Desktop development with C++' workload."
        return $null
    }

    $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installPath) {
        Write-Error "Visual Studio with C++ tools not found."
        return $null
    }

    $msvcRoot = Get-ChildItem (Join-Path $installPath "VC\Tools\MSVC") -Directory |
        Sort-Object Name -Descending |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $msvcRoot) {
        Write-Error "MSVC tools directory not found under $installPath\VC\Tools\MSVC"
        return $null
    }

    $clExe = Join-Path $msvcRoot "bin\Hostx64\x64\cl.exe"
    if (-not (Test-Path $clExe)) {
        Write-Error "cl.exe not found at $clExe"
        return $null
    }

    $linkExe = Join-Path $msvcRoot "bin\Hostx64\x64\link.exe"
    $msbuildExe = Join-Path $installPath "MSBuild\Current\Bin\MSBuild.exe"

    Write-Host "Found MSVC: $msvcRoot"
    return @{
        VSPath      = $installPath
        MsvcRoot    = $msvcRoot
        VCToolsPath = $msvcRoot
        VCToolsVer  = (Split-Path $msvcRoot -Leaf)
        ClExe       = $clExe
        LinkExe     = $linkExe
        MSBuildPath  = $msbuildExe
    }
}

function Resolve-WindowsSdk {

    $sdkRoot = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots").KitsRoot10
    if (-not $sdkRoot) {
        Write-Error "Windows 10/11 SDK not found."
        return $null
    }

    $sdkVer = Get-ChildItem (Join-Path $sdkRoot "Include") -Directory |
        Sort-Object Name |
        Select-Object -Last 1 -ExpandProperty Name
    if (-not $sdkVer) {
        Write-Error "Windows SDK include directory not found under $sdkRoot"
        return $null
    }

    Write-Host "Found Windows SDK $sdkVer"
    return @{
        SDKPath    = $sdkRoot
        SDKVersion = $sdkVer
        IncludeDir = Join-Path (Join-Path $sdkRoot "Include") $sdkVer
        LibDir     = Join-Path (Join-Path $sdkRoot "Lib") $sdkVer
    }
}

function Resolve-Java {

# jdk 25 breaks embedded uwp security init
    $TargetMajor = 21

    function Test-IsJdk21 {
        param([string]$JavaHome)
        $javaExe = Join-Path $JavaHome "bin\java.exe"
        if (-not (Test-Path $javaExe)) { return $false }
        try {
            $prev = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            $verLine = (& $javaExe -version 2>&1 | Select-Object -First 1).ToString()
            $ErrorActionPreference = $prev
        } catch {
            return $false
        }
        if ($verLine -match '"(?<major>\d+)\.') {
            return [int]$Matches.major -eq $TargetMajor
        }
        return $false
    }

    if ($env:JAVA_HOME -and (Test-IsJdk21 $env:JAVA_HOME)) {
        Write-Host "Using JAVA_HOME (JDK 21): $env:JAVA_HOME"
        return @{
            JavaHome = $env:JAVA_HOME
            JavaExe  = Join-Path $env:JAVA_HOME "bin\java.exe"
            JavacExe = Join-Path $env:JAVA_HOME "bin\javac.exe"
            JarExe   = Join-Path $env:JAVA_HOME "bin\jar.exe"
        }
    }
    if ($env:JAVA_HOME) {
        Write-Warning "JAVA_HOME ($env:JAVA_HOME) is not JDK $TargetMajor; searching for a matching install."
    }

    $directRoots = @(
        (Join-Path $env:ProgramFiles "Amazon Corretto"),
        (Join-Path $env:ProgramFiles "Eclipse Adoptium"),
        (Join-Path $env:ProgramFiles "Java"),
        (Join-Path $env:ProgramFiles "Microsoft"),
        (Join-Path $env:SystemDrive "ms-jdk$TargetMajor")
    )

    foreach ($root in $directRoots | Select-Object -Unique) {
        if (-not (Test-Path $root)) { continue }
        $children = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
            Where-Object {
                $_.Name -like "jdk-$TargetMajor*" -or
                $_.Name -like "jdk$TargetMajor*" -or
                $_.Name -like "msopenjdk-$TargetMajor*" -or
                $_.Name -like "microsoft-jdk-$TargetMajor*"
            }
        foreach ($c in $children) {
            if (Test-IsJdk21 $c.FullName) {
                Write-Host "Found JDK $TargetMajor at $($c.FullName)"
                return @{
                    JavaHome = $c.FullName
                    JavaExe  = Join-Path $c.FullName "bin\java.exe"
                    JavacExe = Join-Path $c.FullName "bin\javac.exe"
                    JarExe   = Join-Path $c.FullName "bin\jar.exe"
                }
            }
        }
    }

    Write-Warning "JDK $TargetMajor not found. Install Amazon Corretto 21 or set JAVA_HOME to a JDK 21 install. Compat mod build will be skipped."
    return $null
}

function Ensure-DirectoryTree {

    param([string]$Path)
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Force -Path $Path | Out-Null
    }
}

function Resolve-LocalStateDir {

    $localAppData = $env:LOCALAPPDATA
    if ($localAppData) {
        $pkgPath = Join-Path $localAppData "Packages\BanditVault.MinecraftServerUWP_*\LocalState"
        $found = Get-Item $pkgPath -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) {
            return $found.FullName
        }
    }

    $fallback = Join-Path $ProjectRoot "out\LocalState"
    Ensure-DirectoryTree $fallback
    return $fallback
}

function Collect-Jars {

    param([string]$RootDir)
    $jars = Get-ChildItem -Path $RootDir -Recurse -Filter "*.jar" -ErrorAction SilentlyContinue
    return ($jars.FullName -join ";")
}
