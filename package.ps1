# Builds everything and produces dist\SCDA_P2P_Mod_v<Version>.zip
#   Install.bat / Install.ps1   - installer (into the game, or a multiplayer-only copy)
#   README.md
#   System\                     - SCDA_P2P_Launcher.exe, dinput8.dll, SCDA_P2P.ini
#   System\Redist\ZeroTierOne.msi - official ZeroTier One installer (from redist\, downloaded if missing)
#   Source\                     - full source + build scripts
param([string]$Version = '0.1')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
& (Join-Path $root 'build.ps1')

$name = "SCDA_P2P_Mod_v$Version"
$stage = Join-Path $env:TEMP $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\System", "$stage\Source" | Out-Null

Copy-Item "$root\dist\SCDA_P2P_Launcher.exe", "$root\dist\dinput8.dll" "$stage\System"
if (-not (Test-Path "$stage\System\dinput8.dll")) { throw 'dinput8.dll missing - build it first.' }
Copy-Item "$root\src\SCDA_P2P.ini" "$stage\System"          # clean defaults, not a dev's settings

# ZeroTier One installer, bundled like a game bundles DirectX; the launcher offers it when ZeroTier is missing
$msi = Join-Path $root 'redist\ZeroTierOne.msi'
if (-not (Test-Path $msi)) {
    New-Item -ItemType Directory -Force (Split-Path $msi) | Out-Null
    Write-Host 'Downloading the ZeroTier One installer...'
    Invoke-WebRequest 'https://download.zerotier.com/dist/ZeroTier%20One.msi' -OutFile $msi -UseBasicParsing
}
if ((Get-AuthenticodeSignature $msi).Status -ne 'Valid') { throw "$msi is not validly signed." }
New-Item -ItemType Directory -Force "$stage\System\Redist" | Out-Null
Copy-Item $msi "$stage\System\Redist\ZeroTierOne.msi"
Copy-Item "$root\src\package\Install.bat", "$root\src\package\Install.ps1", "$root\README.md" $stage
Copy-Item "$root\src" "$stage\Source\src" -Recurse
Copy-Item "$root\build.ps1", "$root\package.ps1" "$stage\Source"

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Remove-Item $stage -Recurse -Force
Write-Host ("Package: $zip ({0:N0} KB)" -f ((Get-Item $zip).Length / 1KB))
