[CmdletBinding()]
param(
    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$gameRoot = Split-Path -Parent (Split-Path -Parent $here)
$targetRoot = if ($OutputRoot) { [IO.Path]::GetFullPath($OutputRoot) } else { $gameRoot }
if (-not (Test-Path -LiteralPath $targetRoot -PathType Container)) {
    throw "Output directory does not exist: $targetRoot"
}
$dist = Join-Path $here 'dist'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ x86 tools were not found.' }
$devcmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
New-Item -ItemType Directory -Path $dist -Force | Out-Null

$dllSource = Join-Path $here 'BadMojoEnhancements.c'
$dllDef = Join-Path $here 'BadMojoEnhancements.def'
$dllObject = Join-Path $dist 'BadMojoEnhancements.obj'
$dllOutput = Join-Path $dist 'BadMojoEnhancements.dll'
$loaderSource = Join-Path $here 'BadMojoEnhancementLoader.c'
$loaderObject = Join-Path $dist 'BadMojoEnhancementLoader.obj'
$loaderBuildOutput = Join-Path $dist 'BadMojoEnhancementLoader.exe'

$compileDll = 'call "{0}" -no_logo -arch=x86 -host_arch=x64 >nul && cl.exe /nologo /c /O2 /GS /TC "{1}" /Fo"{2}" && link.exe /nologo /DLL /MACHINE:X86 /SUBSYSTEM:WINDOWS /DEF:"{3}" /OUT:"{4}" "{2}" kernel32.lib user32.lib gdi32.lib comdlg32.lib' -f $devcmd,$dllSource,$dllObject,$dllDef,$dllOutput
& $env:ComSpec /d /s /c $compileDll
if ($LASTEXITCODE -ne 0) { throw "Enhancement DLL build failed: $LASTEXITCODE" }

$compileLoader = 'call "{0}" -no_logo -arch=x86 -host_arch=x64 >nul && cl.exe /nologo /c /O2 /GS /DUNICODE /D_UNICODE /TC "{1}" /Fo"{2}" && link.exe /nologo /MACHINE:X86 /SUBSYSTEM:WINDOWS /OUT:"{3}" "{2}" kernel32.lib user32.lib' -f $devcmd,$loaderSource,$loaderObject,$loaderBuildOutput
& $env:ComSpec /d /s /c $compileLoader
if ($LASTEXITCODE -ne 0) { throw "Enhancement loader build failed: $LASTEXITCODE" }

Copy-Item -LiteralPath $dllOutput -Destination (Join-Path $targetRoot 'BadMojoEnhancements.dll') -Force
Copy-Item -LiteralPath $loaderBuildOutput -Destination (Join-Path $targetRoot 'BadMojoEnhancementLoader.exe') -Force

$expectedOriginalQTHook = 'DB0A58407746F14ECDAB3EA3B5D3477B45355412F1572635CB9A3EC7D230E11C'
$qthook = Join-Path $targetRoot 'qthook.dll'
$actualQTHook = (Get-FileHash -LiteralPath $qthook -Algorithm SHA256).Hash

[pscustomobject]@{
    Loader = (Join-Path $targetRoot 'BadMojoEnhancementLoader.exe')
    EnhancementDll = (Join-Path $targetRoot 'BadMojoEnhancements.dll')
    LoaderSHA256 = (Get-FileHash -LiteralPath (Join-Path $targetRoot 'BadMojoEnhancementLoader.exe') -Algorithm SHA256).Hash
    EnhancementDllSHA256 = (Get-FileHash -LiteralPath (Join-Path $targetRoot 'BadMojoEnhancements.dll') -Algorithm SHA256).Hash
    QTHookSHA256 = $actualQTHook
    QTHookIsOriginal = $actualQTHook -eq $expectedOriginalQTHook
} | Format-List
