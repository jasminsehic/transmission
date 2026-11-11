param(
    [Parameter(Mandatory=$true)] [string] $BuildDir,
    [Parameter(Mandatory=$true)] [string] $DepsPrefix,
    [Parameter(Mandatory=$true)] [string] $OutputDir
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

& cmake --install $BuildDir --prefix $OutputDir
if ($LASTEXITCODE -ne 0) { throw "CMake install failed ($LASTEXITCODE)" }
$bin = Join-Path $OutputDir 'bin'
foreach ($exe in @('transmission-qt.exe', 'transmission-daemon.exe', 'transmission-remote.exe')) {
    if (-not (Test-Path (Join-Path $bin $exe))) { throw "Missing installed executable $exe" }
}
# Bundle Qt, OpenSSL, libcurl and other DLLs needed on a clean Windows host.
$depsBin = Join-Path $DepsPrefix 'bin'
Get-ChildItem -Path $depsBin -Filter '*.dll' -File | ForEach-Object {
    Copy-Item -Path $_.FullName -Destination $bin -Force
}
$plugDest = Join-Path $OutputDir 'plugins'
$plugSource = Join-Path $DepsPrefix 'plugins'
if (Test-Path $plugSource) {
    Copy-Item -Path $plugSource -Destination $plugDest -Recurse -Force
}
if (-not (Test-Path (Join-Path $plugDest 'platforms/qwindows.dll'))) {
    throw 'Qt Windows platform plugin is missing'
}
# Executables live in bin; Qt plugins live one folder up.
[System.IO.File]::WriteAllText(
    (Join-Path $bin 'qt.conf'),
    ("[Paths]" + [Environment]::NewLine + "Plugins=../plugins" + [Environment]::NewLine),
    [System.Text.UTF8Encoding]::new($false))
$launcher = Join-Path $OutputDir 'Run-WebTorrent.ps1'
$launcherContents = @'
# Run Transmission Qt in an isolated WebTorrent-only testing profile.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSCommandPath
$configDir = Join-Path $root 'test-profile'
$bin = Join-Path $root 'bin'
New-Item -ItemType Directory -Path $configDir -Force | Out-Null
$settingsPath = Join-Path $configDir 'settings.json'
if (Test-Path $settingsPath) {
    $settings = Get-Content -Raw -Path $settingsPath | ConvertFrom-Json -AsHashtable
} else {
    $settings = @{}
}
$settings['webtorrent_enabled'] = $true
$settings['webtorrent_ice_servers'] = @('stun:stun.l.google.com:19302')
# Disable ordinary BitTorrent discovery/transports to prove RTC is used.
$settings['tcp-enabled'] = $false
$settings['utp-enabled'] = $false
$settings['dht-enabled'] = $false
$settings['pex-enabled'] = $false
$settings['lpd-enabled'] = $false
$settings['port-forwarding-enabled'] = $false
$settings | ConvertTo-Json -Depth 10 | Set-Content -Path $settingsPath -Encoding utf8
Write-Host "Launching isolated WebTorrent-only test session: $configDir"
Write-Host 'Only WebSocket trackers and WebRTC peers can transfer torrent data in this profile.'
Start-Process -FilePath (Join-Path $bin 'transmission-qt.exe') -ArgumentList @('--config-dir', ('"' + $configDir + '"')) -WorkingDirectory $bin
'@
[System.IO.File]::WriteAllText($launcher, $launcherContents, [System.Text.UTF8Encoding]::new($false))
$readme = @'
Transmission WebTorrent — Windows 11 x64 test build

1. Extract this entire GitHub Actions artifact ZIP into a folder.
2. Right-click Run-WebTorrent.ps1 and select Run with PowerShell,
   or execute: powershell -File .\Run-WebTorrent.ps1
3. Add a torrent containing a ws:// or wss:// WebTorrent tracker.
4. This test profile has TCP/uTP, DHT, PEX and LPD disabled, to prevent
   ordinary BitTorrent peers from being mistaken for WebRTC peers.
5. A separate test-profile folder is created beside this README;
   your existing Transmission settings and torrents are not modified.
6. For an ordinary session or when testing the GUI alone, start
   bin\transmission-qt.exe directly.

Also included:
  bin\transmission-daemon.exe  — command-line background daemon
  bin\transmission-remote.exe  — RPC command-line control utility
  share\transmission\web       — daemon Web interface assets

This is an unsigned, experimental developer build from a fork.
Windows SmartScreen may show a warning. Check provenance before running.
On a clean machine, the Microsoft Visual C++ 2015–2022 x64
Redistributable may be required.
'@
[System.IO.File]::WriteAllText((Join-Path $OutputDir 'README-Windows.txt'), $readme, [System.Text.UTF8Encoding]::new($false))
Write-Host "Packaged Transmission Qt + daemon + remote + DLLs in $OutputDir"
