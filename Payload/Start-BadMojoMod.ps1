[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
$iniPath = Join-Path $root 'BADMOJO.INI'
$backupPath = Join-Path $root 'BADMOJO.INI.badmojo-mod-backup'
$createdPath = Join-Path $root 'BADMOJO.INI.badmojo-mod-created'
$sessionStarted = $false

function Assert-Original([string]$name, [string]$expected) {
    $path = Join-Path $root $name
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $expected) {
        throw "English Redux original file is missing or changed: $name"
    }
}

function Restore-Ini {
    if (Test-Path -LiteralPath $backupPath -PathType Leaf) {
        Copy-Item -LiteralPath $backupPath -Destination $iniPath -Force
        Remove-Item -LiteralPath $backupPath -Force
    } elseif (Test-Path -LiteralPath $createdPath -PathType Leaf) {
        if (Test-Path -LiteralPath $iniPath) { Remove-Item -LiteralPath $iniPath -Force }
        Remove-Item -LiteralPath $createdPath -Force
    }
}

function Get-LocalGameProcess {
    Get-Process BADMOJO -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase) }
}

try {
    if (Get-Process BADMOJO -ErrorAction SilentlyContinue) { throw 'Bad Mojo is already running.' }
    if ((Test-Path -LiteralPath $backupPath) -and (Test-Path -LiteralPath $createdPath)) {
        throw 'Conflicting BADMOJO.INI recovery markers were found.'
    }
    # Recover an INI left by an interrupted previous launcher after the game exited.
    Restore-Ini

    Assert-Original 'BADMOJO.EXE' 'B534E8401E1C1E44D2DC4FE8630880534D633380F53E795F292E3BE795BD7C60'
    Assert-Original 'launcher-original.exe' '4D26599F82A710AA622CE3B768BA8C90095252998CD020235C2C33C0156DA04F'
    Assert-Original 'qthook.dll' 'DB0A58407746F14ECDAB3EA3B5D3477B45355412F1572635CB9A3EC7D230E11C'
    foreach ($name in 'BadMojoEnhancementLoader.exe','BadMojoEnhancements.dll','Run-Magpie-Windowed.ps1','Magpie\config.badmojo-windowed.json') {
        if (!(Test-Path -LiteralPath (Join-Path $root $name) -PathType Leaf)) { throw "Mod file is missing: $name" }
    }
    $magpieExe = Join-Path $root 'Magpie\Magpie.exe'
    if (!(Test-Path -LiteralPath $magpieExe -PathType Leaf)) {
        throw "Magpie is missing. Run Install.cmd again or install Magpie x64 into: $(Join-Path $root 'Magpie')"
    }

    $resourceLines = @(& (Join-Path $root 'Resolve-ResourcePaths.ps1') -GameRoot $root) |
        ForEach-Object { $_.Key + '=' + $_.Path }
    if (!(Test-Path -LiteralPath (Join-Path $root 'SAVE') -PathType Container)) {
        New-Item -ItemType Directory -Path (Join-Path $root 'SAVE') | Out-Null
    }
    if (Test-Path -LiteralPath $iniPath -PathType Leaf) {
        Copy-Item -LiteralPath $iniPath -Destination $backupPath
    } else {
        New-Item -ItemType File -Path $createdPath | Out-Null
    }
    $sessionStarted = $true
    $sessionIni = @('[booger]') + $resourceLines + @(
        'USER=Badmojo Fan', '[SoundMix]', 'SoundOn=1', 'BetweenScreens=1',
        'SmallChunkSize=3', 'BigChunkSize=26', 'BigChunkMethod=0', 'DumbSound=0',
        '[badmojo]', 'Preferences=1111110', '[BadMojoEnhancements]',
        'QTHookCreateSurfaceRva=5856', 'QTHookOverlayCompatEnabled=1',
        'DirectDrawProcAuditEnabled=1', 'DirectDrawQuickTimeSurfacePatchEnabled=1',
        'SaveDialogDefaults=1', 'SkipStartupLogos=1', 'WindowedMainWindow=1',
        'LivesOverlayEnabled=1', 'LivesOverlay=0', 'LivesToggleKey=120',
        'LivesNotifyMs=2000', 'DirectDrawWarmup=0', 'QTHookEarlyInitialize=0',
        'DirectDrawDisplayModeOverride=0'
    )
    [IO.File]::WriteAllText($iniPath, ($sessionIni -join "`r`n") + "`r`n", [Text.Encoding]::ASCII)

    Push-Location -LiteralPath $root
    try {
        & (Join-Path $root 'Run-Magpie-Windowed.ps1') -ReadinessMode TitleDrawn
    } finally {
        Pop-Location
    }
} finally {
    if ($sessionStarted) {
        # The scaler supervisor may return while the game remains open.
        while (Get-LocalGameProcess) { Start-Sleep -Seconds 1 }
        Restore-Ini
    }
}
