[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$gameRoot = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
$payloadRoot = Join-Path $gameRoot 'Payload'

function Ensure-Magpie([string]$root) {
    $magpieDir = Join-Path $root 'Magpie'
    $magpieExe = Join-Path $magpieDir 'Magpie.exe'
    $ownershipPath = Join-Path $root 'BadMojoMod-Magpie-owned.txt'
    if (Test-Path -LiteralPath $magpieExe -PathType Leaf) {
        Write-Host "Using existing Magpie: $magpieExe"
        return
    }
    if (Test-Path -LiteralPath $magpieDir -PathType Container) {
        $existing = @(Get-ChildItem -LiteralPath $magpieDir -Force)
        if ($existing.Count -ne 0) {
            throw "Magpie is incomplete. Move or remove the existing contents of $magpieDir, then rerun Install.cmd."
        }
    }
    if (Test-Path -LiteralPath $ownershipPath) {
        throw "A previous Magpie installation marker remains: $ownershipPath"
    }

    $headers = @{ 'User-Agent' = 'BadMojoReduxMod-Installer'; Accept = 'application/vnd.github+json' }
    [Net.ServicePointManager]::SecurityProtocol =
        [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    try {
        $release = Invoke-RestMethod -Uri 'https://api.github.com/repos/Blinue/Magpie/releases/latest' -Headers $headers
        $assets = @($release.assets | Where-Object { $_.name -match '^Magpie-.*-x64\.zip$' })
        if ($assets.Count -ne 1) { throw 'The latest Magpie release does not have exactly one x64 ZIP.' }
        $asset = $assets[0]
        $uri = [Uri]$asset.browser_download_url
        if ($uri.Scheme -ne 'https' -or $uri.Host -ne 'github.com' -or
                !$uri.AbsolutePath.StartsWith('/Blinue/Magpie/releases/download/', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'The Magpie download URL is not an official GitHub release asset.'
        }

        $tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
        $tempRoot = Join-Path $tempBase ('BadMojoMagpie-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $tempRoot | Out-Null
        try {
            $zipPath = Join-Path $tempRoot 'Magpie.zip'
            $extractRoot = Join-Path $tempRoot 'extracted'
            Write-Host "Downloading Magpie $($release.tag_name) x64 from the official release."
            Invoke-WebRequest -Uri $uri.AbsoluteUri -OutFile $zipPath -UseBasicParsing -Headers @{ 'User-Agent' = 'BadMojoReduxMod-Installer' }
            if ($asset.digest -match '^sha256:([0-9a-f]{64})$' -and
                    (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash -ne $Matches[1]) {
                throw 'The downloaded Magpie ZIP does not match the release checksum.'
            }

            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
            try {
                foreach ($entry in $archive.Entries) {
                    $name = $entry.FullName.Replace('/', '\')
                    if ([IO.Path]::IsPathRooted($name) -or $name.Contains(':') -or
                            $name -match '(^|\\)\.\.(\\|$)') {
                        throw 'The Magpie ZIP contains an unsafe path.'
                    }
                }
            } finally {
                $archive.Dispose()
            }

            Expand-Archive -LiteralPath $zipPath -DestinationPath $extractRoot
            $executables = @(Get-ChildItem -LiteralPath $extractRoot -Recurse -File -Filter 'Magpie.exe')
            if ($executables.Count -ne 1) { throw 'The Magpie ZIP does not contain exactly one Magpie.exe.' }
            $sourceDir = Split-Path -Parent $executables[0].FullName
            New-Item -ItemType Directory -Path $magpieDir -Force | Out-Null
            (Get-FileHash -LiteralPath $executables[0].FullName -Algorithm SHA256).Hash |
                Set-Content -LiteralPath $ownershipPath -Encoding ASCII
            foreach ($file in (Get-ChildItem -LiteralPath $sourceDir -Recurse -File)) {
                if ($file.FullName -eq $executables[0].FullName) { continue }
                $relative = $file.FullName.Substring($sourceDir.Length + 1)
                $destination = [IO.Path]::GetFullPath((Join-Path $magpieDir $relative))
                if (!$destination.StartsWith($magpieDir.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
                    throw 'The Magpie ZIP contains a path outside its installation folder.'
                }
                $parent = Split-Path -Parent $destination
                if (!(Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
                Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
            }
            Copy-Item -LiteralPath $executables[0].FullName -Destination $magpieExe -Force
            Write-Host "Installed Magpie $($release.tag_name): $magpieExe"
        } finally {
            if ($tempRoot.StartsWith($tempBase + '\', [StringComparison]::OrdinalIgnoreCase) -and
                    (Test-Path -LiteralPath $tempRoot)) {
                try { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
                catch { Write-Warning "Could not remove temporary Magpie download: $tempRoot" }
            }
        }
    } catch {
        throw "Automatic Magpie installation failed: $($_.Exception.Message) You can download the x64 ZIP from https://github.com/Blinue/Magpie/releases/latest and extract it into $magpieDir."
    }
}

if (!(Test-Path -LiteralPath $gameRoot -PathType Container)) { throw "Game directory is missing: $gameRoot" }
if (!(Test-Path -LiteralPath $payloadRoot -PathType Container)) { throw "Mod payload is missing: $payloadRoot" }
if (Get-Process BADMOJO -ErrorAction SilentlyContinue) { throw 'Close Bad Mojo before installing the mod.' }
$records = @(Import-Csv -LiteralPath (Join-Path $payloadRoot 'mod-manifest.sha256.csv'))
$recordByPath = @{}
foreach ($record in $records) {
    if (!$record.Path -or !$record.SHA256 -or $recordByPath.ContainsKey($record.Path)) {
        throw "Invalid or duplicate mod manifest entry: $($record.Path)"
    }
    $recordByPath[$record.Path] = $record.SHA256
    $source = Join-Path $gameRoot ($record.Path.Replace('/', '\'))
    if (!(Test-Path -LiteralPath $source -PathType Leaf) -or
            (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $record.SHA256) {
        throw "Mod package file is missing or changed: $($record.Path)"
    }
}
$runtimeFiles = @(
    'launcher.exe', 'BadMojoEnhancementLoader.exe', 'BadMojoEnhancements.dll',
    'Play-BadMojoMod.cmd', 'Start-BadMojoMod.ps1', 'Resolve-ResourcePaths.ps1',
    'Run-Magpie-Windowed.ps1', 'Magpie/config.badmojo-windowed.json'
)
foreach ($path in $runtimeFiles) {
    if (!$recordByPath.ContainsKey('Payload/' + $path)) { throw "Required mod file is not listed in the manifest: $path" }
}
$original = @{
    'BADMOJO.EXE' = 'B534E8401E1C1E44D2DC4FE8630880534D633380F53E795F292E3BE795BD7C60'
    'launcher.exe' = '4D26599F82A710AA622CE3B768BA8C90095252998CD020235C2C33C0156DA04F'
    'qthook.dll' = 'DB0A58407746F14ECDAB3EA3B5D3477B45355412F1572635CB9A3EC7D230E11C'
}
$backup = Join-Path $gameRoot 'launcher-original.exe'
$backupExists = Test-Path -LiteralPath $backup -PathType Leaf
if ($backupExists -and
    (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ne
        '4D26599F82A710AA622CE3B768BA8C90095252998CD020235C2C33C0156DA04F') {
    throw 'Existing launcher-original.exe does not match the English Redux original.'
}
foreach ($entry in $original.GetEnumerator()) {
    $path = Join-Path $gameRoot $entry.Key
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Original English Redux file is missing: $($entry.Key)"
    }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($entry.Key -eq 'launcher.exe' -and $backupExists) {
        # Reinstallation is allowed only over the original or this package's Mod launcher.
        if ($hash -notin @($entry.Value, $recordByPath['Payload/launcher.exe'])) {
            throw 'Installed launcher.exe is neither the original nor this Mod wrapper.'
        }
    } elseif ($hash -ne $entry.Value) {
        throw "Original English Redux file is changed: $($entry.Key)"
    }
}
# Validate every resource path before changing any original game file.
& (Join-Path $payloadRoot 'Resolve-ResourcePaths.ps1') -GameRoot $gameRoot | Out-Null
Ensure-Magpie $gameRoot
if (!$backupExists) {
    Copy-Item -LiteralPath (Join-Path $gameRoot 'launcher.exe') -Destination $backup
    if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ne $original['launcher.exe']) {
        Remove-Item -LiteralPath $backup -Force
        throw 'Could not verify the original launcher backup.'
    }
}
try {
    foreach ($path in $runtimeFiles) {
        $target = Join-Path $gameRoot ($path.Replace('/', '\'))
        $parent = Split-Path -Parent $target
        if (!(Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
        Copy-Item -LiteralPath (Join-Path $payloadRoot ($path.Replace('/', '\'))) -Destination $target -Force
    }
} catch {
    Copy-Item -LiteralPath $backup -Destination (Join-Path $gameRoot 'launcher.exe') -Force
    throw
}
Write-Host "Bad Mojo Redux mod installed. Original launcher backup: $backup"
