# Copies the built .pkg to the PS4 over Wi-Fi so it can be installed without a USB stick.
#
# It uploads to /data/pkg/ on the console, where GoldHEN's Package Installer lists it
# (Settings > Debug Settings > Game > Package Installer).
#
# NOTE: an earlier version of this script replaced the installed app.pkg in place and patched
# app.json / app.pbm. The PS4 rejects that ("The data is corrupted"), so it was removed.
# Never swap /user/app/<ID>/app.pkg by hand - install packages through the Package Installer.
#
# Older OrbisKit packages in /data/pkg are deleted, so only the one just uploaded is left.
#
# Usage: powershell -ExecutionPolicy Bypass -File deploy.ps1 -Ip <ps4-ip> [-File <pkg>] [-Port 2121]
#        -Ip can be left out if the PS4_IP environment variable is set.
#        Without -File the newest OrbisKit .pkg in this folder is uploaded.
#        Port 2121 = GoldHEN's FTP server.
param(
    [string]$Ip = $env:PS4_IP,
    [int]$Port = 2121,
    [string]$TitleId = "ORBK00001",
    [string]$File = ""
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if (-not $Ip) { throw "Give the PS4's address: -Ip <ps4-ip> (or set the PS4_IP environment variable)" }

$pkgFile = if ($File) { Get-Item $File } else { Get-ChildItem "IV0000-$TitleId*.pkg" | Sort-Object LastWriteTime -Descending | Select-Object -First 1 }
if (-not $pkgFile) { throw "No .pkg found. Run build.ps1 first." }
$pkg = [IO.File]::ReadAllBytes($pkgFile.FullName)

$ctrl = New-Object System.Net.Sockets.TcpClient
try { $ctrl.Connect($Ip, $Port) } catch { throw "Can't reach FTP at ${Ip}:$Port (is GoldHEN loaded?)" }
$ctrl.ReceiveTimeout = 30000
$cs = $ctrl.GetStream()
$rd = New-Object IO.StreamReader($cs)
$wr = New-Object IO.StreamWriter($cs); $wr.NewLine = "`r`n"; $wr.AutoFlush = $true
function Read-Reply { $l = $rd.ReadLine(); if ($l -match '^(\d{3})-') { $code = $matches[1]; do { $l = $rd.ReadLine() } until ($l -match "^$code ") }; $l }
function Send-Cmd($c) { $wr.WriteLine($c); Read-Reply }

$null = Read-Reply
$null = Send-Cmd "USER deploy"; $null = Send-Cmd "PASS deploy"; $null = Send-Cmd "TYPE I"
$null = Send-Cmd "MKD /data/pkg"
function Open-Data {
    $r = Send-Cmd "PASV"
    if ($r -notmatch '\((\d+),(\d+),(\d+),(\d+),(\d+),(\d+)\)') { throw "PASV failed: $r" }
    $c = New-Object System.Net.Sockets.TcpClient; $c.Connect($Ip, [int]$matches[5] * 256 + [int]$matches[6]); $c
}

# delete older OrbisKit packages so only the new one is left
$d = Open-Data
$r = Send-Cmd "NLST /data/pkg"
$names = @()
if ($r -match '^1[25]0') {
    $names = (New-Object IO.StreamReader($d.GetStream())).ReadToEnd() -split "`r?`n" | Where-Object { $_ } |
             ForEach-Object { ($_ -split '/')[-1] }
    $d.Close(); $null = Read-Reply
} else { $d.Close() }
foreach ($n in $names) {
    if ($n -like "IV0000-$TitleId*.pkg" -and $n -ne $pkgFile.Name) {
        $null = Send-Cmd "DELE /data/pkg/$n"; Write-Host "Deleted old /data/pkg/$n"
    }
}

$d = Open-Data
$r = Send-Cmd "STOR /data/pkg/$($pkgFile.Name)"
if ($r -notmatch '^1[25]0') { throw "Upload refused: $r" }
Write-Host ("Uploading {0} ({1:N1} MB)..." -f $pkgFile.Name, ($pkg.Length / 1MB))
$s = $d.GetStream(); $s.Write($pkg, 0, $pkg.Length); $s.Flush(); $d.Close()
$r = Read-Reply
if ($r -notmatch '^226') { throw "Upload failed: $r" }
$null = Send-Cmd "QUIT"; $ctrl.Close()
Write-Host "Copied to /data/pkg/$($pkgFile.Name)." -ForegroundColor Green
Write-Host "Install it from Settings > Debug Settings > Game > Package Installer."
