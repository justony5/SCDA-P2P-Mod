# Builds SCDA P2P into .\dist
#   Launcher: .NET Framework 4 csc.exe (ships with Windows)
#   dinput8.dll: 32-bit MinGW gcc (i686-w64-mingw32-gcc / gcc) or clang from llvm-mingw
param([switch]$Install)   # -Install copies dist\* into ..\SCDA-Online\System
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null

# ---- launcher
$csc = "$env:WINDIR\Microsoft.NET\Framework\v4.0.30319\csc.exe"
$icon = Join-Path $root '..\SCDA.ico'
$iconArg = if (Test-Path $icon) { "/win32icon:$icon" } else { '' }
& $csc /nologo /target:winexe /platform:anycpu /optimize+ "/out:$dist\SCDA_P2P_Launcher.exe" `
    "/win32manifest:$root\src\launcher\app.manifest" $iconArg `
    /r:System.Web.Extensions.dll /r:System.ServiceProcess.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll `
    "$root\src\launcher\Launcher.cs"
if ($LASTEXITCODE) { throw 'launcher build failed' }
Write-Host 'Built SCDA_P2P_Launcher.exe'

# ---- dinput8.dll (must be 32-bit)
$cc = $null; $ccArgs = @()
foreach ($c in 'i686-w64-mingw32-gcc', 'i686-w64-mingw32-clang') { if (Get-Command $c -ErrorAction SilentlyContinue) { $cc = $c; break } }
if (-not $cc -and (Get-Command gcc -ErrorAction SilentlyContinue)) {
    if ((& gcc -dumpmachine) -match '^i686') { $cc = 'gcc' }
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { $null }
if ($vs) {
    # MSVC, 32-bit target. Static CRT (/MT) so players need no VC++ redistributable.
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
    $env:PATH += ";$(Split-Path $vswhere)"   # vcvarsall calls vswhere.exe from PATH
    $obj = Join-Path $env:TEMP 'scda_p2p_build'; New-Item -ItemType Directory -Force $obj | Out-Null
    $cmd = "`"$vcvars`" x86 >nul && cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS /Fo`"$obj\\`" /LD `"$root\src\dll\dinput8_proxy.c`" " +
           "/link /DEF:`"$root\src\dll\dinput8_msvc.def`" /OUT:`"$dist\dinput8.dll`" /IMPLIB:`"$obj\dinput8.lib`" iphlpapi.lib ws2_32.lib user32.lib dxguid.lib"
    cmd /c $cmd
    if ($LASTEXITCODE) { throw 'dinput8.dll build failed (MSVC)' }
    Write-Host 'Built dinput8.dll with MSVC (x86)'
}
elseif (-not $cc) { Write-Warning 'No 32-bit compiler found (Visual Studio C++ or MinGW); dinput8.dll not built.'; }
else {
    & $cc -O2 -shared -s -static-libgcc -o "$dist\dinput8.dll" "$root\src\dll\dinput8_proxy.c" "$root\src\dll\dinput8.def" -liphlpapi -lws2_32 -luser32 -ldxguid
    if ($LASTEXITCODE) { throw 'dinput8.dll build failed' }
    Write-Host "Built dinput8.dll with $cc"
}

# ---- default config (never overwrite a user's ini)
$ini = Join-Path $dist 'SCDA_P2P.ini'
if (-not (Test-Path $ini)) { Copy-Item "$root\src\SCDA_P2P.ini" $ini }

if ($Install) {
    $sys = Join-Path $root '..\SCDA-Online\System'
    Copy-Item "$dist\SCDA_P2P_Launcher.exe" $sys -Force
    if (Test-Path "$dist\dinput8.dll") { Copy-Item "$dist\dinput8.dll" $sys -Force }
    if (-not (Test-Path "$sys\SCDA_P2P.ini")) { Copy-Item $ini $sys }
    if (Test-Path "$root\redist\ZeroTierOne.msi") {
        New-Item -ItemType Directory -Force "$sys\Redist" | Out-Null
        Copy-Item "$root\redist\ZeroTierOne.msi" "$sys\Redist" -Force
    }
    Write-Host "Installed into $sys"
}
