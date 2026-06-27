$MC_VERSION = "1.21.11"

$FABRIC_LOADER_VERSION = "0.19.2"

$INTERMEDIARY_VERSION = $MC_VERSION

$JVM_XMX = "4G"
$JVM_XMS = "1G"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$ServerDir = Join-Path $ProjectRoot "MC.Server"
$CompatModDir = Join-Path $ProjectRoot "compat_mod"
$OutputDir = Join-Path $ProjectRoot "out"
$JREDir = Join-Path $OutputDir "jre"
$LibsDir = Join-Path $OutputDir "libraries"
$ServerJarDir = Join-Path $OutputDir "server"

$MSBuildPath = $null
$VCToolsVersion = $null

$FABRIC_LOADER_MAVEN = "net.fabricmc:fabric-loader:$FABRIC_LOADER_VERSION"

$MINECRAFT_SERVER_MAVEN = "net.minecraft:server:$MC_VERSION"

$FABRIC_MAVEN_URL = "https://maven.fabricmc.net"
$MOJANG_MAVEN_URL = "https://libraries.minecraft.net"
$MC_META_URL = "https://piston-meta.mojang.com"

$OutputExe = Join-Path $OutputDir "MC.Server.exe"
$OutputAppx = Join-Path $OutputDir "MC.Server.appx"
$StagingDir = Join-Path $OutputDir "staging\package"

$AppxFileName = "MC.Server.appx"
$CertificateDir = Join-Path $OutputDir "certs"
$CertificateFileName = "MC_Server_DevMode.pfx"
$CertificatePassword = "devmode"
$DefaultCertificateSubject = "CN=BanditVault MinecraftServerUWP Dev"
$OutputAppx = Join-Path $ProjectRoot "MC.Server.appx"
