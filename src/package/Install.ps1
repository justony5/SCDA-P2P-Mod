# SCDA P2P installer
#  - Mode "Mod":        copies the mod into an existing Double Agent install (SCDA-Online\System)
#  - Mode "OnlineOnly": makes a standalone copy of just the multiplayer part (~2.1 GB) with the mod,
#                       so the single-player files (~8 GB) aren't needed.
# Run with no arguments for the interactive version (Install.bat does this).
param(
    [string]$GameDir,                  # folder containing SCDA-Online (the game install)
    [ValidateSet('Mod', 'OnlineOnly')][string]$Mode,
    [string]$TargetDir,                # OnlineOnly: where to create the copy
    [switch]$NoShortcut
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
$here = $PSScriptRoot
$modFiles = @('SCDA_P2P_Launcher.exe', 'dinput8.dll', 'SCDA_P2P.ini')

function Pick-Folder([string]$prompt) {
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = $prompt
    $dlg.ShowNewFolderButton = $true
    if ($dlg.ShowDialog() -ne 'OK') { throw 'Cancelled.' }
    $dlg.SelectedPath
}

function Find-OnlineDir([string]$dir) {
    foreach ($c in @($dir, (Join-Path $dir 'SCDA-Online'))) {
        if (Test-Path (Join-Path $c 'System\SCDA_online.exe')) { return (Resolve-Path $c).Path }
    }
    return $null
}

function Find-GameDir {
    $guesses = @(
        (Join-Path $here '..'),
        "${env:ProgramFiles(x86)}\Ubisoft\Tom Clancy's Splinter Cell Double Agent",
        "$env:ProgramFiles\Ubisoft\Tom Clancy's Splinter Cell Double Agent",
        "${env:ProgramFiles(x86)}\Steam\steamapps\common\Tom Clancy's Splinter Cell Double Agent"
    )
    foreach ($g in $guesses) { if ($g -and (Find-OnlineDir $g)) { return Find-OnlineDir $g } }
    return $null
}

function New-Shortcut([string]$exe, [string]$name) {
    $lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) "$name.lnk"
    $ws = New-Object -ComObject WScript.Shell
    $s = $ws.CreateShortcut($lnk)
    $s.TargetPath = $exe
    $s.WorkingDirectory = Split-Path $exe
    $s.IconLocation = "$exe,0"
    $s.Save()
    Write-Host "Desktop shortcut: $lnk"
}

function Copy-ModFiles([string]$sysDir) {
    foreach ($f in $modFiles) {
        $dst = Join-Path $sysDir $f
        if ($f -eq 'SCDA_P2P.ini' -and (Test-Path $dst)) { continue }   # keep the player's settings
        if ($f -eq 'dinput8.dll' -and (Test-Path $dst)) {
            $isOurs = (Select-String -Path $dst -Pattern 'SCDA P2P loaded' -SimpleMatch -Quiet)
            if (-not $isOurs) {
                Copy-Item $dst "$dst.backup" -Force
                Write-Host "Existing dinput8.dll backed up to dinput8.dll.backup"
            }
        }
        Copy-Item (Join-Path $here "System\$f") $dst -Force
    }
    Write-Host "Mod files installed in $sysDir"
}

# ---------------------------------------------------------------- main
try {
    $online = if ($GameDir) { Find-OnlineDir $GameDir } else { Find-GameDir }
    if (-not $online) {
        [System.Windows.Forms.MessageBox]::Show("Select your Splinter Cell Double Agent folder (the one containing SCDA-Online).", 'SCDA P2P') | Out-Null
        $online = Find-OnlineDir (Pick-Folder 'Select the Splinter Cell Double Agent folder (contains SCDA-Online)')
        if (-not $online) { throw 'SCDA-Online\System\SCDA_online.exe was not found in that folder.' }
    }
    Write-Host "Found multiplayer files: $online"

    if (-not $Mode) {
        $r = [System.Windows.Forms.MessageBox]::Show(
            "Found: $online`n`nYES  = install the mod into this game folder`nNO   = create a separate multiplayer-only copy (about 2.1 GB)`n         that doesn't need the single-player files",
            'SCDA P2P - choose install type', 'YesNoCancel', 'Question')
        if ($r -eq 'Cancel') { throw 'Cancelled.' }
        $Mode = if ($r -eq 'Yes') { 'Mod' } else { 'OnlineOnly' }
    }

    if ($Mode -eq 'Mod') {
        $sys = Join-Path $online 'System'
        Copy-ModFiles $sys
        if (-not $NoShortcut) { New-Shortcut (Join-Path $sys 'SCDA_P2P_Launcher.exe') 'SCDA Multiplayer (P2P)' }
    }
    else {
        if (-not $TargetDir) { $TargetDir = Join-Path (Pick-Folder 'Choose where to create the multiplayer-only copy') 'SCDA-Multiplayer' }
        if (Test-Path (Join-Path $TargetDir 'System\SCDA_online.exe')) { throw "$TargetDir already contains a copy." }
        $need = (Get-ChildItem $online -Recurse -File | Measure-Object Length -Sum).Sum
        $drive = Get-PSDrive -Name ((Split-Path -Qualifier ([IO.Path]::GetFullPath($TargetDir))).TrimEnd(':'))
        if ($drive.Free -lt $need + 100MB) { throw ("Not enough space: need {0:N1} GB, {1:N1} GB free." -f ($need / 1GB), ($drive.Free / 1GB)) }

        Write-Host ("Copying {0:N1} GB to $TargetDir ..." -f ($need / 1GB))
        New-Item -ItemType Directory -Force $TargetDir | Out-Null
        # robocopy: fast, resumable; exit codes < 8 mean success
        robocopy $online $TargetDir /E /NFL /NDL /NJH /NP /R:1 /W:1 /XF SCDA_P2P.log | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "Copy failed (robocopy exit $LASTEXITCODE)." }
        $sys = Join-Path $TargetDir 'System'
        Copy-ModFiles $sys
        # firewall rule in the launcher is keyed by exe path; clear it so the copy gets its own rule
        $ini = Join-Path $sys 'SCDA_P2P.ini'
        (Get-Content $ini) | Where-Object { $_ -notmatch '^FirewallRule=' } | Set-Content $ini
        if (-not $NoShortcut) { New-Shortcut (Join-Path $sys 'SCDA_P2P_Launcher.exe') 'SCDA Multiplayer (P2P)' }
        Write-Host "Multiplayer-only copy created: $TargetDir"
    }
    if (-not $PSBoundParameters.Count) {
        [System.Windows.Forms.MessageBox]::Show("Done. Start the game with 'SCDA Multiplayer (P2P)' on your desktop.`nZeroTier One must be installed (zerotier.com/download).", 'SCDA P2P') | Out-Null
    }
}
catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    if (-not $PSBoundParameters.Count) { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'SCDA P2P', 'OK', 'Error') | Out-Null }
    exit 1
}
