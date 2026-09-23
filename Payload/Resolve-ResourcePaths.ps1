[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$GameRoot)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($GameRoot).TrimEnd('\')
if (!(Test-Path -LiteralPath $root -PathType Container)) {
    throw "Game directory is missing: $root"
}
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class BadMojoShortPath {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint GetShortPathName(string longPath, StringBuilder shortPath, uint length);
}
"@
function Get-ExistingShortPath([string]$path) {
    $buffer = New-Object Text.StringBuilder 32768
    $length = [BadMojoShortPath]::GetShortPathName($path, $buffer, [uint32]$buffer.Capacity)
    if ($length -eq 0 -or $length -ge $buffer.Capacity) {
        throw "Could not resolve game resource path: $path"
    }
    return $buffer.ToString()
}
$shortRoot = Get-ExistingShortPath $root
$paths = [ordered]@{
    BACKGROUND='OVER;BACKGND'; TOPO='TOPO'; BTC='BTC'; PALETTE='PALETTE'
    SCRIBBLE='OVER;SCRIBBLE'; CEL='OVER;CEL'; SCRIPT='SCRIPT'; ROACH='ROACHD'
    MOVIE='OVER;MOVIE'; MASH='OVER;MASH'; SOUND='SOUND'; SAVE='SAVE'
}
foreach ($entry in $paths.GetEnumerator()) {
    $values = foreach ($name in $entry.Value.Split(';')) {
        $directory = Join-Path $root $name
        if ($name -ne 'SAVE' -and !(Test-Path -LiteralPath $directory -PathType Container)) {
            throw "English Redux resource directory is missing: $directory"
        }
        $path = if (Test-Path -LiteralPath $directory -PathType Container) {
            Get-ExistingShortPath $directory
        } else {
            # SAVE may be absent on a fresh installation. Its parent already exists.
            Join-Path $shortRoot 'SAVE'
        }
        $bytes = [Text.Encoding]::Default.GetByteCount($path)
        if ($bytes -gt 50) {
            throw "Game install path is too long for English Redux ($bytes bytes; limit 50): $path. Install the game in a shorter directory."
        }
        $path
    }
    [pscustomobject]@{ Key=$entry.Key; Path=($values -join ';') }
}
