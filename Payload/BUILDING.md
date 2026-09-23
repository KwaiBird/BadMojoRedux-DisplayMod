# Build project binaries

Use Visual Studio C++ x86 tools on Windows. Build the enhancement loader and
DLL with `ModSource\Build-Phase1.ps1` in a private English Redux installation,
as documented in that script. Build the Mod-owned Steam launcher from
`ModSource\SteamLauncher.c` with:

`powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\ModSource\Build-SteamLauncher.ps1`

The launcher build writes `ModSource\dist\launcher.exe`; copy that output
into a local candidate only after checking the original launcher backup.
Compiler timestamps can change binary hashes.
