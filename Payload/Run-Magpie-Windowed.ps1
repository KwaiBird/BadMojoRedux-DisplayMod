[CmdletBinding()]
param([ValidateSet('ThirtyProbes', 'TitleDrawn')][string]$ReadinessMode = 'ThirtyProbes')

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($PSScriptRoot)
$magpieDir = Join-Path $root 'Magpie'
$magpie = Join-Path $magpieDir 'Magpie.exe'
$loader = Join-Path $root 'BadMojoEnhancementLoader.exe'
$gamePath = Join-Path $root 'BADMOJO.EXE'
$configSource = Join-Path $magpieDir 'config.badmojo-windowed.json'
$configDir = Join-Path $magpieDir 'config'
$log = Join-Path $root 'magpie-windowed.log'

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class BadMojoMagpieWindow {
 public delegate bool EnumProc(IntPtr hwnd, IntPtr param);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
 [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hwnd, int command);
 [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hwnd);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint msg, UIntPtr wp, IntPtr lp, uint flags, uint timeout, out UIntPtr result);
 [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string name);
 [DllImport("user32.dll")] public static extern bool PostThreadMessage(uint thread, uint msg, UIntPtr wp, IntPtr lp);
 [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
 [DllImport("kernel32.dll", SetLastError=true)] public static extern bool ReadProcessMemory(IntPtr process, IntPtr address, [Out] byte[] buffer, UIntPtr size, out UIntPtr count);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
}
'@

function Write-Log([string]$message) { "$([DateTime]::Now.ToString('o')) $message" | Add-Content -LiteralPath $log }
function Get-GameWindow {
    $hwnd = [BadMojoMagpieWindow]::FindWindow('LittleCRT', 'Bad Mojo')
    if (!$hwnd) { return $null }
    $windowPid = [uint32]0
    [BadMojoMagpieWindow]::GetWindowThreadProcessId($hwnd, [ref]$windowPid) | Out-Null
    $game = Get-Process -Id $windowPid -ErrorAction SilentlyContinue
    if (!$game -or $game.ProcessName -ne 'BADMOJO') { return $null }
    $rect = [BadMojoMagpieWindow+RECT]::new()
    if (![BadMojoMagpieWindow]::GetClientRect($hwnd, [ref]$rect)) { return $null }
    [pscustomobject]@{ Handle=$hwnd; Pid=$windowPid; Process=$game; Width=$rect.right; Height=$rect.bottom }
}
function Wait-OwnedMagpieReady([Diagnostics.Process]$process) {
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $hwnd = [BadMojoMagpieWindow]::FindWindow('Magpie_Main', 'Magpie')
        if ($hwnd) {
            $windowPid = [uint32]0
            [BadMojoMagpieWindow]::GetWindowThreadProcessId($hwnd, [ref]$windowPid) | Out-Null
            if ($windowPid -eq $process.Id) {
                [BadMojoMagpieWindow]::ShowWindowAsync($hwnd, 0) | Out-Null
                return $true
            }
        }
        Start-Sleep -Milliseconds 25
    } while (!$process.HasExited -and [DateTime]::UtcNow -lt $deadline)
    return $false
}
function Focus-GameWindow([IntPtr]$hwnd) {
    [BadMojoMagpieWindow]::ShowWindowAsync($hwnd, 5) | Out-Null
    if ($ReadinessMode -eq 'ThirtyProbes') {
        [BadMojoMagpieWindow]::BringWindowToTop($hwnd) | Out-Null
        [BadMojoMagpieWindow]::SetForegroundWindow($hwnd) | Out-Null
        Start-Sleep -Milliseconds 150
        return
    }
    for ($attempt = 0; $attempt -lt 5; ++$attempt) {
        [BadMojoMagpieWindow]::BringWindowToTop($hwnd) | Out-Null
        [BadMojoMagpieWindow]::SetForegroundWindow($hwnd) | Out-Null
        Start-Sleep -Milliseconds 100
        if ([BadMojoMagpieWindow]::GetForegroundWindow() -eq $hwnd) {
            Write-Log 'Game window is foreground before Magpie startup.'
            return
        }
    }
    $foreground = [BadMojoMagpieWindow]::GetForegroundWindow()
    Write-Log "Game foreground not confirmed (target=0x$('{0:X}' -f $hwnd.ToInt64()) actual=0x$('{0:X}' -f $foreground.ToInt64()))."
}
function Wait-GameReady([IntPtr]$hwnd, [int]$required = 30) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $stable = 0
    do {
        $reply = [UIntPtr]::Zero
        $ok = [BadMojoMagpieWindow]::SendMessageTimeout($hwnd, 0, [UIntPtr]::Zero, [IntPtr]::Zero, 3, 250, [ref]$reply)
        if ($ok -ne [IntPtr]::Zero) { $stable++ } else { $stable = 0 }
        if ($stable -ge $required) { Write-Log "Game message loop responsive for $required consecutive probes."; return }
        Start-Sleep -Milliseconds 100
    } while (!$loaderProcess.HasExited -and [DateTime]::UtcNow -lt $deadline)
    throw 'Game message loop did not become ready; scaling was not requested.'
}
function Wait-TitleDrawn($game) {
    # Redux-BADMOJO.EXE.c: WinMain assigns FUN_0042b560 to DAT_004f285c
    # immediately before GetMessage. The first timer callback draws the
    # current scene through FUN_00437d10, releases the window DC, and only
    # then replaces that callback with FUN_0042b540. Scene 0 is chosen
    # before the window exists, so the scene number alone is insufficient.
    $base = $game.Process.MainModule.BaseAddress.ToInt64()
    $handle = [BadMojoMagpieWindow]::OpenProcess(0x1010, $false, [uint32]$game.Pid)
    if ($handle -eq [IntPtr]::Zero) { throw 'Could not read the game title-draw state.' }
    try {
        $buffer = New-Object byte[] 4
        $count = [UIntPtr]::Zero
        $readSize = [UIntPtr]::new([uint32]4)
        $deadline = [DateTime]::UtcNow.AddSeconds(3)
        do {
            $callbackRead = [BadMojoMagpieWindow]::ReadProcessMemory($handle,
                [IntPtr]($base + 0x0f285c), $buffer, $readSize, [ref]$count)
            $callback = if ($callbackRead -and $count.ToUInt64() -eq 4) {
                [BitConverter]::ToUInt32($buffer, 0)
            } else { 0 }
            $sceneRead = [BadMojoMagpieWindow]::ReadProcessMemory($handle,
                [IntPtr]($base + 0x0fe054), $buffer, $readSize, [ref]$count)
            $scene = if ($sceneRead -and $count.ToUInt64() -eq 4) {
                [BitConverter]::ToInt32($buffer, 0)
            } else { -1 }
            if ($callback -eq ($base + 0x2b540) -and $scene -eq 0) {
                $reply = [UIntPtr]::Zero
                $ok = [BadMojoMagpieWindow]::SendMessageTimeout($game.Handle, 0,
                    [UIntPtr]::Zero, [IntPtr]::Zero, 3, 250, [ref]$reply)
                if ($ok -ne [IntPtr]::Zero) {
                    Write-Log 'Title scene 0 draw completed; game loop responded.'
                    return $true
                }
            }
            Start-Sleep -Milliseconds 50
        } while (!$loaderProcess.HasExited -and [DateTime]::UtcNow -lt $deadline)
        Write-Log "Title-draw signal unavailable (scene=$scene callback=0x$('{0:X8}' -f $callback)); using thirty-probe fallback."
        return $false
    } finally {
        [BadMojoMagpieWindow]::CloseHandle($handle) | Out-Null
    }
}
function Wait-MagpieScalingWindow([Diagnostics.Process]$process) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $hwnd = [BadMojoMagpieWindow]::FindWindow(
            'Window_Magpie_967EB565-6F73-4E94-AE53-00CC42592A22', $null)
        if ($hwnd) {
            $windowPid = [uint32]0
            [BadMojoMagpieWindow]::GetWindowThreadProcessId($hwnd,
                [ref]$windowPid) | Out-Null
            if ($windowPid -eq $process.Id) { return $true }
        }
        Start-Sleep -Milliseconds 100
    } while (!$process.HasExited -and !$loaderProcess.HasExited -and [DateTime]::UtcNow -lt $deadline)
    return $false
}
function Start-MagpieScale {
    # Magpie's configured Scale shortcut is 0x0C41: Win+Shift+A.
    # keybd_event is used only after this launcher has foregrounded LittleCRT.
    [BadMojoMagpieWindow]::keybd_event(0x5B, 0, 0, [UIntPtr]::Zero)
    [BadMojoMagpieWindow]::keybd_event(0x10, 0, 0, [UIntPtr]::Zero)
    [BadMojoMagpieWindow]::keybd_event(0x41, 0, 0, [UIntPtr]::Zero)
    [BadMojoMagpieWindow]::keybd_event(0x41, 0, 2, [UIntPtr]::Zero)
    [BadMojoMagpieWindow]::keybd_event(0x10, 0, 2, [UIntPtr]::Zero)
    [BadMojoMagpieWindow]::keybd_event(0x5B, 0, 2, [UIntPtr]::Zero)
}
function Wait-MagpieShortcutReady([Diagnostics.Process]$process, [Int64]$logOffset) {
    $magpieLog = Join-Path $magpieDir 'logs\magpie.log'
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        if (Test-Path -LiteralPath $magpieLog) {
            $stream = [IO.File]::Open($magpieLog, [IO.FileMode]::Open,
                [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
            try {
                if ($stream.Length -gt $logOffset) {
                    $stream.Position = $logOffset
                    $reader = New-Object IO.StreamReader($stream)
                    try {
                        if ($reader.ReadToEnd() -match 'AppSettings\.cpp:\d+\|Initialize\|') {
                            return $true
                        }
                    } finally { $reader.Dispose() }
                }
            } finally { $stream.Dispose() }
        }
        Start-Sleep -Milliseconds 100
    } while (!$process.HasExited -and [DateTime]::UtcNow -lt $deadline)
    return $false
}
function Stop-OwnedMagpie([Diagnostics.Process]$process) {
    if (!$process -or $process.HasExited) { return }
    $quit = [BadMojoMagpieWindow]::RegisterWindowMessage('WM_MAGPIE_QUIT')
    foreach ($thread in $process.Threads) {
        [BadMojoMagpieWindow]::PostThreadMessage([uint32]$thread.Id, $quit, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    }
    if (!$process.WaitForExit(5000)) { $process.Kill(); Write-Log 'Owned Magpie required forced termination.' }
    else { Write-Log 'Owned Magpie exited normally.' }
}

$guard = New-Object Threading.Mutex($false, 'Local\BadMojoWindowedMagpieLauncher')
if (!$guard.WaitOne(0)) { $guard.Dispose(); exit }
$owned = $null
$loaderProcess = $null
try {
    if (Get-Process BADMOJO -ErrorAction SilentlyContinue) { throw 'Bad Mojo is already running.' }
    if (!(Test-Path -LiteralPath $loader) -or !(Test-Path -LiteralPath $magpie)) { throw 'Required windowed or Magpie file is missing.' }

    New-Item -ItemType Directory -Path $configDir -Force | Out-Null
    Copy-Item -LiteralPath $configSource -Destination (Join-Path $configDir 'config.json') -Force
    $configPath = Join-Path $configDir 'config.json'
    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    $badMojoProfile = $config.profiles | Where-Object name -eq 'Bad Mojo' | Select-Object -First 1
    $badMojoProfile.pathRule = $gamePath
    foreach ($profile in $config.profiles) {
        $profile | Add-Member -NotePropertyName autoScale -NotePropertyValue 0 -Force
    }
    $badMojoProfile.autoScale = 1
    [IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json -Depth 20), (New-Object Text.UTF8Encoding($false)))
    $loaderProcess = Start-Process -FilePath $loader -ArgumentList '-windowed-launcher2' -WorkingDirectory $root -WindowStyle Hidden -PassThru
    Write-Log "Started windowed loader PID $($loaderProcess.Id)."

    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $game = $null
    do {
        $game = Get-GameWindow
        if ($game -and $game.Width -eq 640 -and $game.Height -eq 480) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline -and !$loaderProcess.HasExited)
    if (!$game) { throw 'The windowed Bad Mojo client was not found.' }
    Write-Log "Windowed game client detected: $($game.Width)x$($game.Height)."
    if ($ReadinessMode -eq 'TitleDrawn') {
        if (Wait-TitleDrawn $game) { Wait-GameReady $game.Handle 8 }
        else { Wait-GameReady $game.Handle }
    } else {
        Wait-GameReady $game.Handle
    }

    # Start Magpie only after LittleCRT has remained responsive. Its profile
    # performs one fullscreen auto-scale without a global shortcut race.
    Focus-GameWindow $game.Handle
    $owned = Start-Process -FilePath $magpie -ArgumentList '-t' -WorkingDirectory $magpieDir -WindowStyle Hidden -PassThru
    if (!$owned.WaitForInputIdle(5000)) { Write-Log 'Magpie did not report input-idle before timeout.' }
    if (!(Wait-MagpieScalingWindow $owned)) {
        throw 'Magpie did not create its fullscreen scaling window.'
    }
    Write-Log 'Fullscreen scaling window confirmed.'

    while (!$loaderProcess.WaitForExit(100)) {
        if ($owned.HasExited) { Write-Log 'Owned Magpie exited; game remains running without scaler.'; break }
    }
} catch {
    Write-Log $_.Exception.Message
} finally {
    Stop-OwnedMagpie $owned
    if ($owned) { $owned.Dispose() }
    if ($loaderProcess) { $loaderProcess.Dispose() }
    $guard.ReleaseMutex(); $guard.Dispose()
}
