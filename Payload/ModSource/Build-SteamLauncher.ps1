[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$dist = Join-Path $here 'dist'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Visual Studio C++ x86 tools were not found.' }
$devcmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$source = Join-Path $here 'SteamLauncher.c'
$binary = Join-Path $dist 'launcher.exe'
$object = Join-Path $dist 'SteamLauncher.obj'
$command = 'call "{0}" -no_logo -arch=x86 -host_arch=x64 >nul && cl.exe /nologo /c /O2 /GS /DUNICODE /D_UNICODE /TC "{1}" /Fo"{2}" && link.exe /nologo /MACHINE:X86 /SUBSYSTEM:CONSOLE /OUT:"{3}" "{2}" kernel32.lib' -f $devcmd,$source,$object,$binary
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Steam launcher build failed: $LASTEXITCODE" }
Write-Host "Built: $binary"
