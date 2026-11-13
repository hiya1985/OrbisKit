# Builds OrbisKit into a .pkg with the OpenOrbis PS4 toolchain (Windows).
#
# Needs:
#   - OpenOrbis PS4 Toolchain, with env var OO_PS4_TOOLCHAIN pointing at it
#   - LLVM (clang + ld.lld) on PATH
#   - Optional: GoldHEN Plugins SDK with env var GOLDHEN_SDK (enables sandbox escape)
#
# Usage:  powershell -ExecutionPolicy Bypass -File build.ps1 [-Patch] [-Deploy] [-Clean]
#   (no -Patch)  full app package  <CONTENT_ID>.pkg              - first install
#   -Patch       update package    <CONTENT_ID>-update-X.YY.pkg  - meant to install over an installed
#                                  OrbisKit, but the Package Installer refuses it for now ("unable")
param([switch]$Clean, [switch]$Deploy, [switch]$Patch, [string]$Ver = "", [string]$Ip = $env:PS4_IP)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$TITLE      = "OrbisKit"
$TITLE_ID   = "ORBK00001"
$CONTENT_ID = "IV0000-ORBK00001_00-ORBISKIT00000000"
$VERSION    = "01.01"   # the ONLY place the version is set: also shown in the app (01.01 -> "v1.01")
$MASTER_VER = "01.00"   # base (master) version: must stay the same in the app and all its updates
if ($Ver) {             # one-off override, e.g. -Ver 01.01 (PowerShell names ignore case: not -Version)
    if ($Ver -notmatch '^\d\d\.\d\d$') { throw "-Ver must look like 01.01" }
    $VERSION = $Ver
}

function Run($exe, [string[]]$argv) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "Failed ($LASTEXITCODE): $exe $($argv -join ' ')" }
}

if ($Clean) {
    Remove-Item -Recurse -Force build, eboot.bin, pkg.gp4, sce_sys\param.sfo, *.pkg -ErrorAction SilentlyContinue
    Write-Host "Cleaned."; return
}

$TC = $env:OO_PS4_TOOLCHAIN
if (-not $TC -or -not (Test-Path "$TC\link.x")) {
    throw "Set OO_PS4_TOOLCHAIN to your OpenOrbis toolchain folder (https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases)"
}
foreach ($t in "clang", "ld.lld") {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "$t not found. Install LLVM and add it to PATH." }
}
$BIN = "$TC\bin\windows"
# PkgTool targets .NET Core 3.0; let it run on any newer installed runtime
$env:DOTNET_ROLL_FORWARD = "Major"

if (-not (Test-Path src\font_data.h) -or -not (Test-Path sce_sys\icon0.png)) {
    & powershell -ExecutionPolicy Bypass -File tools\gen_assets.ps1
}

New-Item -ItemType Directory -Force build | Out-Null

$cflags = @("--target=x86_64-pc-freebsd12-elf", "-fPIC", "-funwind-tables", "-O2", "-Wall",
            "-isysroot", $TC, "-isystem", "$TC\include", "-Isrc",
            ('-DAPP_VERSION=\"{0}\"' -f $VERSION.TrimStart('0')))   # \" survives PowerShell 5.1's argument passing
$libs = @("-lc", "-lm", "-lkernel", "-lSceVideoOut", "-lSceUserService", "-lScePad",
          "-lSceNet", "-lSceNetCtl", "-lSceSystemService")
$libdirs = @("-L$TC\lib")

if ($env:GOLDHEN_SDK) {
    Write-Host "GoldHEN SDK: $env:GOLDHEN_SDK (sandbox escape enabled)"
    $cflags += @("-DUSE_GOLDHEN", "-I$env:GOLDHEN_SDK\include")
    $libdirs += "-L$env:GOLDHEN_SDK"
    $libs += "-lGoldHEN_Hook"
} else {
    Write-Host "GOLDHEN_SDK not set: building without sandbox escape (limited filesystem access)" -ForegroundColor Yellow
}

$objs = @()
foreach ($src in Get-ChildItem src\*.c) {
    $obj = "build\$($src.BaseName).o"
    Write-Host "CC  $($src.Name)"
    Run clang ($cflags + @("-c", $src.FullName, "-o", $obj))
    $objs += $obj
}

Write-Host "LD  eboot.elf"
Run ld.lld (@("-m", "elf_x86_64", "-pie", "--script", "$TC\link.x", "--eh-frame-hdr",
              "-o", "build\eboot.elf") + $libdirs + $libs + @("$TC\lib\crt1.o") + $objs)

Write-Host "SELF eboot.bin"
Run "$BIN\create-fself.exe" @("-in=build\eboot.elf", "-out=build\eboot.oelf", "--eboot", "eboot.bin", "--paid", "0x3800000000000011")

Write-Host "SFO param.sfo"
$sfo = "sce_sys\param.sfo"
$pkgtool = "$BIN\PkgTool.Core.exe"
Run $pkgtool @("sfo_new", $sfo)
$entries = @(
    @("APP_TYPE", "Integer", "4", "1"),
    @("APP_VER", "Utf8", "8", $VERSION),
    @("ATTRIBUTE", "Integer", "4", "0"),
    @("CATEGORY", "Utf8", "4", $(if ($Patch) { "gp" } else { "gd" })),   # gp = game patch (update)
    @("CONTENT_ID", "Utf8", "48", $CONTENT_ID),
    @("DOWNLOAD_DATA_SIZE", "Integer", "4", "0"),
    @("SYSTEM_VER", "Integer", "4", "0"),
    @("TITLE", "Utf8", "128", $TITLE),
    @("TITLE_ID", "Utf8", "12", $TITLE_ID),
    @("VERSION", "Utf8", "8", $MASTER_VER)
)
foreach ($e in $entries) {
    Run $pkgtool @("sfo_setentry", $sfo, $e[0], "--type", $e[1], "--maxsize", $e[2], "--value", $e[3])
}

New-Item -ItemType Directory -Force sce_sys\about | Out-Null
if (-not (Test-Path sce_sys\about\right.sprx)) {
    $sprx = Get-ChildItem -Recurse "$TC\samples" -Filter right.sprx -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $sprx) { throw "Couldn't find right.sprx in the toolchain samples folder" }
    Copy-Item $sprx.FullName sce_sys\about\right.sprx
}

# OpenOrbis libc needs these runtime modules shipped inside the package
New-Item -ItemType Directory -Force sce_module | Out-Null
foreach ($m in "libc.prx", "libSceFios2.prx") {
    if (-not (Test-Path "sce_module\$m")) {
        $f = Get-ChildItem -Recurse "$TC\samples" -Filter $m -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $f) { throw "Couldn't find $m in the toolchain samples folder" }
        Copy-Item $f.FullName "sce_module\$m"
    }
}

$outName = if ($Patch) { "$CONTENT_ID-update-$($VERSION.TrimStart('0')).pkg" } else { "$CONTENT_ID.pkg" }
Write-Host "PKG $outName"
Run "$BIN\create-gp4.exe" @("-out", "pkg.gp4", "--content-id=$CONTENT_ID",
    "--files", "eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx")
if ($Patch) {
    $gp4 = (Get-Content pkg.gp4 -Raw).Replace("<volume_type>pkg_ps4_app</volume_type>", "<volume_type>pkg_ps4_patch</volume_type>")
    [IO.File]::WriteAllText("$PSScriptRoot\pkg.gp4", $gp4)
}
# pkg_build always names its output <CONTENT_ID>.pkg: build in a temp folder, then rename
New-Item -ItemType Directory -Force build\pkg | Out-Null
Remove-Item build\pkg\*.pkg -ErrorAction SilentlyContinue
Run $pkgtool @("pkg_build", "pkg.gp4", "build\pkg")
if ($Patch) { Remove-Item "$CONTENT_ID-update-*.pkg" -ErrorAction SilentlyContinue }   # keep only the newest update
Move-Item -Force "build\pkg\$CONTENT_ID.pkg" $outName

Write-Host ""
Write-Host "Built $PSScriptRoot\$outName" -ForegroundColor Green

if ($Deploy) {
    Write-Host ""
    & "$PSScriptRoot\deploy.ps1" -Ip $Ip -Port 2121 -TitleId $TITLE_ID -File $outName
}
