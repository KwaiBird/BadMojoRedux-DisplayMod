[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$gameRoot = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
$payloadRoot = Join-Path $gameRoot 'Payload'
$logPath = Join-Path $gameRoot 'BadMojoMod-uninstall.log'
$originalLauncherHash = '4D26599F82A710AA622CE3B768BA8C90095252998CD020235C2C33C0156DA04F'
$backup = Join-Path $gameRoot 'launcher-original.exe'
$ownershipPath = Join-Path $gameRoot 'BadMojoMod-Magpie-owned.txt'
$magpieDir = Join-Path $gameRoot 'Magpie'
$saveDir = Join-Path $gameRoot 'SAVE'

function Write-UninstallLog([string]$message) {
    Add-Content -LiteralPath $logPath -Value "$([DateTime]::Now.ToString('o')) $message" -Encoding UTF8
    Write-Host $message
}

function Get-ModPath([string]$relative) {
    $name = $relative.Replace('/', '\')
    if (!$name -or [IO.Path]::IsPathRooted($name) -or $name.Contains(':') -or
            $name -match '(^|\)\.\.(\|$)') {
        throw "Unsafe uninstall path: $relative"
    }
    # SAVE is an explicit whitelist. Never enumerate or remove it, even if a
    # future package manifest accidentally lists something below it.
    if ($name.Split('\')[0] -ieq 'SAVE') { throw "SAVE is protected: $relative" }
    $full = [IO.Path]::GetFullPath((Join-Path $gameRoot $name))
    if (!$full.StartsWith($gameRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
            $full.StartsWith($saveDir + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the mod area: $relative"
    }
    return $full
}

function Assert-PlainDirectory([string]$path) {
    if (!(Test-Path -LiteralPath $path)) { return }
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($path)
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        $item = Get-Item -LiteralPath $current -Force
        if (!$item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Uninstall target is not an ordinary directory: $current"
        }
        foreach ($child in (Get-ChildItem -LiteralPath $current -Force)) {
            if ($child.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Uninstall target contains a link: $($child.FullName)"
            }
            if ($child.PSIsContainer) { $pending.Push($child.FullName) }
        }
    }
}

try {
    Write-UninstallLog 'Bad Mojo Redux mod uninstall started.'
    if (Get-Process BADMOJO -ErrorAction SilentlyContinue) { throw 'Close Bad Mojo before uninstalling.' }
    foreach ($process in (Get-Process Magpie,BadMojoEnhancementLoader -ErrorAction SilentlyContinue)) {
        if ($process.Path -and $process.Path.StartsWith($gameRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Close the mod process before uninstalling: $($process.ProcessName)"
        }
    }
    if (!(Test-Path -LiteralPath $payloadRoot -PathType Container)) { throw 'Payload is missing; cannot safely identify the package files.' }
    Assert-PlainDirectory $payloadRoot
    $manifestPath = Join-Path $payloadRoot 'mod-manifest.sha256.csv'
    $records = @(Import-Csv -LiteralPath $manifestPath)
    if ($records.Count -lt 1) { throw 'The mod manifest is empty.' }
    $knownPayload = @{}
    $knownPayload[$manifestPath.ToLowerInvariant()] = $true
    $recordByPath = @{}
    foreach ($record in $records) {
        $path = Get-ModPath $record.Path
        if (!$record.SHA256 -or $recordByPath.ContainsKey($record.Path)) { throw "Invalid manifest entry: $($record.Path)" }
        $recordByPath[$record.Path] = $record.SHA256
        if ($record.Path.StartsWith('Payload/', [StringComparison]::OrdinalIgnoreCase)) {
            $knownPayload[$path.ToLowerInvariant()] = $true
        } elseif ($record.Path -notmatch '[/\\]') {
            # Root package files are removed by the fixed allowlist below.
        } else {
            throw "Unexpected package path: $($record.Path)"
        }
    }
    foreach ($file in (Get-ChildItem -LiteralPath $payloadRoot -Recurse -File -Force)) {
        if (!$knownPayload.ContainsKey($file.FullName.ToLowerInvariant())) {
            throw "Payload contains an unlisted file; leaving it untouched: $($file.FullName)"
        }
    }
    foreach ($path in @('Install.cmd','Install-BadMojoMod.ps1','README.md','LICENSE','Uninstall.cmd','Uninstall-BadMojoMod.ps1')) {
        if (!$recordByPath.ContainsKey($path)) { throw "The package manifest does not list $path" }
    }

    $launcher = Join-Path $gameRoot 'launcher.exe'
    $launcherHash = (Get-FileHash -LiteralPath $launcher -Algorithm SHA256).Hash
    $modLauncherHash = $recordByPath['Payload/launcher.exe']
    $backupExists = Test-Path -LiteralPath $backup -PathType Leaf
    if ($backupExists -and (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ne $originalLauncherHash) {
        throw 'launcher-original.exe is not the original English Redux launcher.'
    }
    if ($launcherHash -ne $originalLauncherHash -and $launcherHash -ne $modLauncherHash) {
        throw 'launcher.exe is neither the original nor this Mod launcher.'
    }
    if ($launcherHash -eq $modLauncherHash -and !$backupExists) {
        throw 'The original launcher backup is missing. No files were removed.'
    }
    $iniPath = Join-Path $gameRoot 'BADMOJO.INI'
    $iniBackup = Join-Path $gameRoot 'BADMOJO.INI.badmojo-mod-backup'
    $iniCreated = Join-Path $gameRoot 'BADMOJO.INI.badmojo-mod-created'
    if ((Test-Path -LiteralPath $iniBackup) -and (Test-Path -LiteralPath $iniCreated)) {
        throw 'Conflicting BADMOJO.INI recovery markers were found.'
    }

    $magpieOwned = Test-Path -LiteralPath $ownershipPath -PathType Leaf
    if (Test-Path -LiteralPath $magpieDir) { Assert-PlainDirectory $magpieDir }
    if ($magpieOwned) {
        $expectedMagpieHash = (Get-Content -LiteralPath $ownershipPath -Raw).Trim()
        if ($expectedMagpieHash -notmatch '^[0-9A-Fa-f]{64}$') { throw 'Magpie ownership marker is invalid.' }
        $magpieExe = Join-Path $magpieDir 'Magpie.exe'
        if ((Test-Path -LiteralPath $magpieExe -PathType Leaf) -and
                (Get-FileHash -LiteralPath $magpieExe -Algorithm SHA256).Hash -ne $expectedMagpieHash) {
            $magpieOwned = $false
            Write-UninstallLog 'Magpie.exe changed since automatic installation; preserving that copy.'
        }
    }

    # Restore original state before removing the package needed for recovery.
    if (Test-Path -LiteralPath $iniBackup -PathType Leaf) {
        Copy-Item -LiteralPath $iniBackup -Destination $iniPath -Force
        Remove-Item -LiteralPath $iniBackup -Force
        Write-UninstallLog 'Restored the previous BADMOJO.INI.'
    } elseif (Test-Path -LiteralPath $iniCreated -PathType Leaf) {
        if (Test-Path -LiteralPath $iniPath -PathType Leaf) { Remove-Item -LiteralPath $iniPath -Force }
        Remove-Item -LiteralPath $iniCreated -Force
        Write-UninstallLog 'Removed the temporary BADMOJO.INI.'
    }
    if ($launcherHash -eq $modLauncherHash) {
        Copy-Item -LiteralPath $backup -Destination $launcher -Force
        if ((Get-FileHash -LiteralPath $launcher -Algorithm SHA256).Hash -ne $originalLauncherHash) {
            throw 'Restoring the original launcher failed; the backup was retained.'
        }
        Write-UninstallLog 'Restored original launcher.exe.'
    }
    if ($backupExists) {
        Remove-Item -LiteralPath $backup -Force
        Write-UninstallLog 'Removed launcher-original.exe backup.'
    }

    foreach ($name in @('BadMojoEnhancementLoader.exe','BadMojoEnhancements.dll',
            'Play-BadMojoMod.cmd','Start-BadMojoMod.ps1','Resolve-ResourcePaths.ps1',
            'Run-Magpie-Windowed.ps1','magpie-windowed.log','BadMojoEnhancements-Phase1.log')) {
        $path = Get-ModPath $name
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Remove-Item -LiteralPath $path -Force
            Write-UninstallLog "Removed $name."
        }
    }
    if ($magpieOwned) {
        if (Test-Path -LiteralPath $magpieDir -PathType Container) {
            Remove-Item -LiteralPath $magpieDir -Recurse -Force
            Write-UninstallLog 'Removed Magpie downloaded by the Mod installer.'
        }
        Remove-Item -LiteralPath $ownershipPath -Force
    } else {
        $profile = Get-ModPath 'Magpie/config.badmojo-windowed.json'
        if (Test-Path -LiteralPath $profile -PathType Leaf) {
            Remove-Item -LiteralPath $profile -Force
            Write-UninstallLog 'Removed Mod profile from user-supplied Magpie.'
        }
        if ((Test-Path -LiteralPath $magpieDir -PathType Container) -and
                @(Get-ChildItem -LiteralPath $magpieDir -Force).Count -eq 0) {
            Remove-Item -LiteralPath $magpieDir
            Write-UninstallLog 'Removed the empty Magpie directory.'
        } elseif (Test-Path -LiteralPath $magpieDir -PathType Container) {
            Write-UninstallLog 'Preserved user-supplied Magpie.'
        }
    }
    if (Test-Path -LiteralPath $ownershipPath -PathType Leaf) {
        Remove-Item -LiteralPath $ownershipPath -Force
    }

    Remove-Item -LiteralPath $payloadRoot -Recurse -Force
    Write-UninstallLog 'Removed Payload directory.'
    foreach ($name in @('.gitattributes','README.md','LICENSE','Install.cmd','Install-BadMojoMod.ps1')) {
        $path = Get-ModPath $name
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Remove-Item -LiteralPath $path -Force
            Write-UninstallLog "Removed $name."
        }
    }
    Write-UninstallLog 'SAVE directory and its contents were preserved.'
    Write-UninstallLog 'Package cleanup complete; Uninstall.cmd will remove the two uninstaller files.'
} catch {
    Write-UninstallLog "Uninstall stopped: $($_.Exception.Message)"
    throw
}
