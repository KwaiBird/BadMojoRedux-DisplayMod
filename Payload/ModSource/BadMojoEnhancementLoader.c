#include <windows.h>
#include <tlhelp32.h>

static int fail(const WCHAR *stage, DWORD error)
{
    WCHAR message[512];
    wsprintfW(message, L"%s failed (0x%08lX).", stage, error);
    MessageBoxW(NULL, message, L"Bad Mojo Enhancements test", MB_OK | MB_ICONERROR);
    return 1;
}

static BOOL directory_from_path(WCHAR *path)
{
    WCHAR *cursor = path + lstrlenW(path);
    while (cursor > path && cursor[-1] != L'\\') --cursor;
    if (cursor == path) return FALSE;
    cursor[-1] = 0;
    return TRUE;
}

static BOOL command_equals(PWSTR command, const WCHAR *expected)
{
    SIZE_T length;
    SIZE_T index;
    while (*command == L' ' || *command == L'\t') ++command;
    length = lstrlenW(expected);
    for (index = 0; index < length; ++index)
        if (command[index] != expected[index]) return FALSE;
    command += length;
    while (*command == L' ' || *command == L'\t') ++command;
    return *command == 0;
}

static BOOL same_path(DWORD pid, const WCHAR *expected)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    WCHAR actual[MAX_PATH];
    DWORD length = MAX_PATH;
    BOOL match = FALSE;
    HANDLE expected_file = INVALID_HANDLE_VALUE;
    HANDLE actual_file = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION expected_info;
    BY_HANDLE_FILE_INFORMATION actual_info;
    if (!process) return FALSE;
    if (QueryFullProcessImageNameW(process, 0, actual, &length))
        match = lstrcmpiW(actual, expected) == 0;
    if (!match && actual[0]) {
        expected_file = CreateFileW(expected, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        actual_file = CreateFileW(actual, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (expected_file != INVALID_HANDLE_VALUE && actual_file != INVALID_HANDLE_VALUE
                && GetFileInformationByHandle(expected_file, &expected_info)
                && GetFileInformationByHandle(actual_file, &actual_info))
            match = expected_info.dwVolumeSerialNumber == actual_info.dwVolumeSerialNumber
                && expected_info.nFileIndexHigh == actual_info.nFileIndexHigh
                && expected_info.nFileIndexLow == actual_info.nFileIndexLow;
    }
    if (expected_file != INVALID_HANDLE_VALUE) CloseHandle(expected_file);
    if (actual_file != INVALID_HANDLE_VALUE) CloseHandle(actual_file);
    CloseHandle(process);
    return match;
}

static DWORD find_game(const WCHAR *expected)
{
    HANDLE snapshot;
    PROCESSENTRY32W entry;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (!lstrcmpiW(entry.szExeFile, L"BADMOJO.EXE")
                    && same_path(entry.th32ProcessID, expected)) {
                CloseHandle(snapshot);
                return entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return 0;
}

static void suspend_game_threads(DWORD pid, HANDLE *threads, unsigned int *count)
{
    HANDLE snapshot;
    THREADENTRY32 entry;
    *count = 0;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == pid && *count < 64) {
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE,
                        entry.th32ThreadID);
                if (thread) {
                    SuspendThread(thread);
                    threads[(*count)++] = thread;
                }
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

static void resume_game_threads(HANDLE *threads, unsigned int count)
{
    unsigned int i;
    for (i = 0; i < count; ++i) {
        ResumeThread(threads[i]);
        CloseHandle(threads[i]);
    }
}

static BOOL inject(HANDLE process, const WCHAR *dll_path)
{
    SIZE_T bytes;
    LPVOID remote_path = NULL;
    HANDLE remote_thread = NULL;
    HMODULE local_dll = NULL;
    HMODULE kernel32;
    FARPROC load_library;
    FARPROC local_initialize;
    LPTHREAD_START_ROUTINE remote_initialize;
    DWORD remote_module = 0;
    DWORD result = 0;
    BOOL success = FALSE;

    bytes = ((SIZE_T)lstrlenW(dll_path) + 1) * sizeof(WCHAR);
    remote_path = VirtualAllocEx(process, NULL, bytes,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) goto cleanup;
    if (!WriteProcessMemory(process, remote_path, dll_path, bytes, NULL)) goto cleanup;
    kernel32 = GetModuleHandleW(L"kernel32.dll");
    load_library = kernel32 ? GetProcAddress(kernel32, "LoadLibraryW") : NULL;
    if (!load_library) goto cleanup;
    remote_thread = CreateRemoteThread(process, NULL, 0,
            (LPTHREAD_START_ROUTINE)load_library, remote_path, 0, NULL);
    if (!remote_thread) goto cleanup;
    if (WaitForSingleObject(remote_thread, 10000) != WAIT_OBJECT_0) goto cleanup;
    if (!GetExitCodeThread(remote_thread, &remote_module) || !remote_module) goto cleanup;
    CloseHandle(remote_thread); remote_thread = NULL;
    local_dll = LoadLibraryExW(dll_path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!local_dll) goto cleanup;
    local_initialize = GetProcAddress(local_dll, "InitializeEnhancements");
    if (!local_initialize) goto cleanup;
    remote_initialize = (LPTHREAD_START_ROUTINE)(ULONG_PTR)(remote_module
            + ((BYTE *)local_initialize - (BYTE *)local_dll));
    remote_thread = CreateRemoteThread(process, NULL, 0,
            remote_initialize, NULL, 0, NULL);
    if (!remote_thread) goto cleanup;
    if (WaitForSingleObject(remote_thread, 10000) != WAIT_OBJECT_0) goto cleanup;
    if (!GetExitCodeThread(remote_thread, &result) || result != 1) goto cleanup;
    success = TRUE;
cleanup:
    if (remote_thread) CloseHandle(remote_thread);
    if (remote_path) VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    if (local_dll) FreeLibrary(local_dll);
    return success;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show)
{
    WCHAR loader_path[MAX_PATH], directory[MAX_PATH];
    WCHAR launcher_path[MAX_PATH], game_path[MAX_PATH], dll_path[MAX_PATH];
    WCHAR command_line[2 * MAX_PATH];
    STARTUPINFOW startup;
    PROCESS_INFORMATION launcher;
    HANDLE process, threads[64];
    unsigned int thread_count;
    DWORD pid = 0, tick, child_exit = 1;
    BOOL injected = FALSE;
    BOOL use_original_launcher = FALSE;
    BOOL use_direct_launcher2 = FALSE;
    BOOL use_windowed_launcher2 = FALSE;
    BOOL suspended_game = FALSE;
    (void)instance; (void)previous; (void)show;

    if (command && command[0]) {
        if (command_equals(command, L"-original-launcher"))
            use_original_launcher = TRUE;
        else if (command_equals(command, L"-direct-launcher2"))
            use_direct_launcher2 = TRUE;
        else if (command_equals(command, L"-windowed-launcher2"))
            use_windowed_launcher2 = TRUE;
        else
            return fail(L"loader command line", ERROR_INVALID_PARAMETER);
    }

    if (!GetModuleFileNameW(NULL, loader_path, MAX_PATH))
        return fail(L"GetModuleFileName", GetLastError());
    lstrcpyW(directory, loader_path);
    if (!directory_from_path(directory)) return fail(L"loader directory", ERROR_BAD_PATHNAME);
    wsprintfW(game_path, L"%s\\BADMOJO.EXE", directory);
    if (use_windowed_launcher2) {
        lstrcpyW(launcher_path, game_path);
        wsprintfW(command_line, L"\"%s\"", game_path);
        suspended_game = TRUE;
    } else if (use_direct_launcher2) {
        WCHAR system_directory[MAX_PATH];
        if (!GetSystemDirectoryW(system_directory, MAX_PATH))
            return fail(L"GetSystemDirectory", GetLastError());
        wsprintfW(launcher_path, L"%s\\cmd.exe", system_directory);
        wsprintfW(command_line, L"\"%s\" /d /c call \"%s\\launcher2.bat\"",
                launcher_path, directory);
    } else if (use_original_launcher) {
        wsprintfW(launcher_path, L"%s\\BadMojoEnhancementsOriginalLauncher.exe", directory);
        wsprintfW(command_line, L"\"%s\"", launcher_path);
    } else {
        wsprintfW(launcher_path, L"%s\\launcher.exe", directory);
        wsprintfW(command_line, L"\"%s\"", launcher_path);
    }
    wsprintfW(dll_path, L"%s\\BadMojoEnhancements.dll", directory);
    if (GetFileAttributesW(launcher_path) == INVALID_FILE_ATTRIBUTES)
        return fail(L"launcher.exe lookup", ERROR_FILE_NOT_FOUND);
    if (GetFileAttributesW(dll_path) == INVALID_FILE_ATTRIBUTES)
        return fail(L"enhancement DLL lookup", ERROR_FILE_NOT_FOUND);

    SetEnvironmentVariableW(L"__COMPAT_LAYER", L"");
    ZeroMemory(&startup, sizeof(startup)); startup.cb = sizeof(startup);
    ZeroMemory(&launcher, sizeof(launcher));
    if (!CreateProcessW(launcher_path, command_line, NULL, NULL, FALSE,
            suspended_game ? CREATE_SUSPENDED : 0,
            NULL, directory, &startup, &launcher))
        return fail(L"launcher CreateProcess", GetLastError());

    if (suspended_game) {
        if (!SetProcessAffinityMask(launcher.hProcess, 1)) {
            TerminateProcess(launcher.hProcess, ERROR_FUNCTION_FAILED);
            CloseHandle(launcher.hThread); CloseHandle(launcher.hProcess);
            return fail(L"SetProcessAffinityMask", GetLastError());
        }
        process = launcher.hProcess;
        injected = inject(process, dll_path);
        if (!injected) {
            TerminateProcess(launcher.hProcess, ERROR_DLL_INIT_FAILED);
            CloseHandle(launcher.hThread); CloseHandle(launcher.hProcess);
            return fail(L"suspended BADMOJO.EXE injection", ERROR_DLL_INIT_FAILED);
        }
        if (ResumeThread(launcher.hThread) == (DWORD)-1) {
            TerminateProcess(launcher.hProcess, ERROR_FUNCTION_FAILED);
            CloseHandle(launcher.hThread); CloseHandle(launcher.hProcess);
            return fail(L"ResumeThread", GetLastError());
        }
    }

    tick = GetTickCount();
    while (!suspended_game && !injected && GetTickCount() - tick < 15000) {
        pid = find_game(game_path);
        if (pid) {
            process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION
                    | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                    FALSE, pid);
            if (process) {
                suspend_game_threads(pid, threads, &thread_count);
                injected = inject(process, dll_path);
                resume_game_threads(threads, thread_count);
                CloseHandle(process);
            }
        }
        if (!injected) Sleep(50);
    }
    if (!injected) {
        TerminateProcess(launcher.hProcess, ERROR_TIMEOUT);
        CloseHandle(launcher.hThread); CloseHandle(launcher.hProcess);
        return fail(L"BADMOJO.EXE post-launch injection", ERROR_TIMEOUT);
    }
    WaitForSingleObject(launcher.hProcess, INFINITE);
    GetExitCodeProcess(launcher.hProcess, &child_exit);
    CloseHandle(launcher.hThread);
    CloseHandle(launcher.hProcess);
    return (int)child_exit;
}
