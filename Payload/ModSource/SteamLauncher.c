#include <windows.h>
#include <stdio.h>
#include <wchar.h>

int wmain(void)
{
    wchar_t root[32768], cmd_path[32768], batch_path[32768];
    wchar_t command[] = L"cmd.exe /d /c call .\\Play-BadMojoMod.cmd";
    STARTUPINFOW startup;
    PROCESS_INFORMATION child;
    wchar_t *slash;
    DWORD length, exit_code = 1;

    length = GetModuleFileNameW(NULL, root, 32768);
    if (!length || length >= 32768) {
        fputws(L"Could not find the Bad Mojo game directory.\n", stderr);
        return 1;
    }
    slash = wcsrchr(root, L'\\');
    if (!slash) return 1;
    *slash = 0;
    if (swprintf_s(batch_path, 32768, L"%ls\\Play-BadMojoMod.cmd", root) < 0 ||
        GetFileAttributesW(batch_path) == INVALID_FILE_ATTRIBUTES) {
        fputws(L"Play-BadMojoMod.cmd is missing.\n", stderr);
        return 1;
    }
    length = GetSystemDirectoryW(cmd_path, 32768);
    if (!length || length >= 32768 ||
        wcscat_s(cmd_path, 32768, L"\\cmd.exe")) {
        fputws(L"Could not locate cmd.exe.\n", stderr);
        return 1;
    }

    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&child, sizeof(child));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(cmd_path, command, NULL, NULL, FALSE, 0, NULL,
                        root, &startup, &child)) {
        fwprintf(stderr, L"Could not start cmd.exe (Windows error %lu).\n", GetLastError());
        return 1;
    }
    CloseHandle(child.hThread);
    WaitForSingleObject(child.hProcess, INFINITE);
    GetExitCodeProcess(child.hProcess, &exit_code);
    CloseHandle(child.hProcess);
    return (int)exit_code;
}
