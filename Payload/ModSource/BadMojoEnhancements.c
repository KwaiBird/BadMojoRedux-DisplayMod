#include <windows.h>
#include <commdlg.h>
#include <initguid.h>
#include <ddraw.h>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

static BOOL (WINAPI *original_GetSaveFileNameA)(LPOPENFILENAMEA);
static BOOL (WINAPI *original_GetOpenFileNameA)(LPOPENFILENAMEA);
static INT_PTR (WINAPI *original_DialogBoxParamA)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
static DLGPROC original_preferences_proc;
static SIZE preferences_base_client;
static SIZE preferences_min_window;
static LOGFONTA preferences_base_font;
static HFONT preferences_scaled_font;
static int preferences_scaled_height;
static BOOL preferences_layout_ready;

#define MAX_PREFERENCES_CHILDREN 32
typedef struct preferences_child_layout {
    HWND window;
    RECT rectangle;
} preferences_child_layout;
static preferences_child_layout preferences_children[MAX_PREFERENCES_CHILDREN];
static unsigned int preferences_child_count;

static BOOL feature_save_dialog_defaults = TRUE;
static BOOL feature_skip_startup_logos = TRUE;
static BOOL feature_windowed_main_window = FALSE;
static BOOL feature_lives_overlay = TRUE;
static BOOL feature_directdraw_route_probe;
static BOOL feature_directdraw_proc_audit;
static BOOL feature_directdraw_qthook_object_pin;
static BOOL feature_directdraw_vtable_persistence_probe;
static BOOL feature_directdraw_quicktime_surface_patch;
static BOOL feature_qthook_overlay_compat;
static BOOL feature_directdraw_warmup = TRUE;
static BOOL feature_qthook_early_initialize = TRUE;
static BOOL feature_directdraw_display_mode_override;
static char ini_path[MAX_PATH];
static char log_path[MAX_PATH];

static HWND windowed_main_window;
static BOOL directdraw_warmup_complete;
static BOOL qthook_early_initialize_attempted;
/* The game surface is 640x420 inside its 640x480 logical canvas. */
static int windowed_width = 640;
static int windowed_height = 480;
static HWND (WINAPI *original_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD,
        int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static int (WINAPI *original_GetDeviceCaps)(HDC, int);
static BOOL (WINAPI *original_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);

/* The original game shows lives only in the 30-pixel strip hidden by the
 * scaler. Keep a copy of that authentic icon and composite it into the
 * 640x420 playfield without changing either BADMOJO.EXE or LIVES.CEL. */
#define BADMOJO_DRAW_LIVES_THUNK_RVA 0x000010c3
#define BADMOJO_DRAW_LIVES_RVA       0x00013f10
#define BADMOJO_SCENE_SWITCH_THUNK_RVA 0x00001767
#define BADMOJO_SCENE_SWITCH_RVA       0x00007a50
#define BADMOJO_PAUSE_MENU_SCENE       916 /* SLADY.LUT: GAME */
#define BADMOJO_VIEWPORT_X_RVA       0x000f22b8
#define BADMOJO_VIEWPORT_Y_RVA       0x000f22bc
#define BADMOJO_DEATH_COUNT_RVA      0x00103dcc
#define LIVES_DEFAULT_TOGGLE_KEY     VK_F9
#define LIFE_ICON_SIZE                24
#define LIFE_ICON_STEP                30
#define LIVES_OVERLAY_WIDTH           (LIFE_ICON_SIZE + 3 * LIFE_ICON_STEP)
#define LIVES_TIMER_ID                0x424d4c56
#define LIVES_DEFAULT_DISPLAY_MS     2000
#define LIVES_OVERLAP_LOG_LIMIT        32

typedef void (__cdecl *draw_lives_fn)(HDC, int, int);
typedef void (__cdecl *scene_switch_fn)(int);
typedef BOOL (WINAPI *transparent_blt_fn)(HDC, int, int, int, int,
        HDC, int, int, int, int, UINT);
typedef HRESULT (WINAPI *direct_draw_get_display_mode_fn)(IDirectDraw *,
        DDSURFACEDESC *);
typedef HRESULT (WINAPI *direct_draw_create_surface_fn)(IDirectDraw *,
        DDSURFACEDESC *, IDirectDrawSurface **, IUnknown *);
typedef HRESULT (WINAPI *surface_lock_fn)(IDirectDrawSurface *, RECT *,
        DDSURFACEDESC *, DWORD, HANDLE);
typedef HRESULT (WINAPI *surface_unlock_fn)(IDirectDrawSurface *, void *);
typedef int (WINAPI *stretch_dibits_fn)(HDC, int, int, int, int, int, int,
        int, int, const void *, const BITMAPINFO *, UINT, DWORD);
typedef BOOL (WINAPI *bitblt_fn)(HDC, int, int, int, int, HDC, int, int, DWORD);

static BYTE *badmojo_image;
static draw_lives_fn original_draw_lives;
static scene_switch_fn original_scene_switch;
static transparent_blt_fn overlay_TransparentBlt;
static HDC life_icon_dc;
static HBITMAP life_icon_bitmap;
static HGDIOBJ life_icon_previous;
static HDC lives_backing_dc;
static HBITMAP lives_backing_bitmap;
static HGDIOBJ lives_backing_previous;
static COLORREF life_icon_transparent_color = RGB(0, 0, 0);
static HWND lives_main_window;
static BOOL lives_available;
static BOOL lives_visible = TRUE;
static BOOL lives_key_down;
static BOOL lives_timer_started;
static BOOL lives_hook_thread_started;
static BOOL directdraw_route_probe_started;
static FARPROC (WINAPI *original_GetProcAddress)(HMODULE, LPCSTR);
typedef HRESULT (WINAPI *direct_draw_create_fn)(GUID *, IDirectDraw **, IUnknown *);
static direct_draw_create_fn original_audited_DirectDrawCreate;
static IDirectDraw *retained_qthook_ddraw;
static BOOL directdraw_proc_audit_hooked;
static LONG directdraw_proc_audit_events;
static int lives_toggle_key = LIVES_DEFAULT_TOGGLE_KEY;
static DWORD lives_change_display_ms = LIVES_DEFAULT_DISPLAY_MS;
static BOOL lives_drawn;
static int lives_remaining;
static int lives_drawn_count = -1;
static int lives_known_count = -1;
static int lives_drawn_x;
static int lives_drawn_y;
static DWORD lives_change_visible_until;
static BOOL lives_change_pending;
static LONG lives_pause_menu_active;
static LONG lives_overlap_stretch_logs;
static LONG lives_overlap_bitblt_logs;
static LONG lives_overlap_frontbuffer_logs;
static LONG lives_overlay_composition_depth;
static HDC lives_frame_dc;
static HBITMAP lives_frame_bitmap;
static HGDIOBJ lives_frame_original;
static int lives_frame_width, lives_frame_height;
static LONG lives_frame_events;
static DWORD movie_last_lock_tick;
static int movie_lock_streak;
static BOOL movie_active;
static int lives_last_callback_first = -1;
static int lives_last_callback_last = -1;
static direct_draw_get_display_mode_fn original_DirectDraw_GetDisplayMode;
static BOOL directdraw_display_mode_hooked;
static BOOL directdraw_display_mode_logged;
static BOOL directdraw_display_mode_thread_started;
static DWORD directdraw_qthook_create_surface_rva = 0x2960;

#define VANILLA_QTHOOK_SURFACE_LOCK_RVA       0x1260
#define VANILLA_QTHOOK_SURFACE_UNLOCK_RVA     0x1480
#define VANILLA_QTHOOK_STRETCHDIBITS_RVA      0x15f0
#define VANILLA_QTHOOK_BITBLT_RVA             0x1990
#define VANILLA_QTHOOK_UPDATE_FRONTBUFFER_RVA 0x3000
#define VANILLA_QTHOOK_INTERNAL_BITBLT_RVA    0x3008

static HMODULE qthook_compat_module;
static BOOL *qthook_update_frontbuffer;
static BOOL *qthook_internal_bitblt;
static direct_draw_create_surface_fn original_qthook_CreateSurface;
static surface_lock_fn original_qthook_surface_lock;
static surface_unlock_fn original_qthook_surface_unlock;
static stretch_dibits_fn original_qthook_StretchDIBits;
static bitblt_fn original_qthook_BitBlt;
static BOOL qthook_gdi_compat_hooked;
static BOOL qthook_surface_compat_hooked;

static VOID CALLBACK lives_timer_proc(HWND, UINT, UINT_PTR, DWORD);
static void start_lives_timer(HWND);
static DWORD WINAPI install_lives_hook_after_startup(LPVOID);
static void start_directdraw_route_probe(void);
static BOOL install_directdraw_proc_audit(void);
static HRESULT WINAPI replacement_audited_DirectDrawCreate(GUID *, IDirectDraw **,
        IUnknown *);
static HRESULT WINAPI compat_qthook_CreateSurface(IDirectDraw *, DDSURFACEDESC *,
        IDirectDrawSurface **, IUnknown *);
static HRESULT WINAPI compat_qthook_surface_lock(IDirectDrawSurface *, RECT *,
        DDSURFACEDESC *, DWORD, HANDLE);
static HRESULT WINAPI compat_qthook_surface_unlock(IDirectDrawSurface *, void *);
static int WINAPI compat_qthook_StretchDIBits(HDC, int, int, int, int, int,
        int, int, int, const void *, const BITMAPINFO *, UINT, DWORD);
static BOOL WINAPI compat_qthook_BitBlt(HDC, int, int, int, int, HDC, int,
        int, DWORD);
static BOOL install_qthook_gdi_compat(void);
static BOOL install_lives_scene_switch_hook(void);
static void erase_lives_overlay(HDC);
static void draw_lives_overlay(HDC);
static void redraw_lives_now(void);
static BOOL lives_overlay_intersects(int, int, int, int);
static BOOL lives_should_be_visible(void);
static void log_lives_overlap(const char *, LONG *, int, int, int, int, BOOL);

static void append_log(const char *message, DWORD value)
{
    HANDLE file;
    char line[256];
    DWORD written;
    if (!log_path[0]) return;
    wsprintfA(line, "%s: 0x%08lX\r\n", message, value);
    file = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, line, lstrlenA(line), &written, NULL);
    CloseHandle(file);
}

static void append_log_text(const char *message, const char *value)
{
    HANDLE file;
    char line[MAX_PATH + 96];
    DWORD written;
    if (!log_path[0]) return;
    file = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    wsprintfA(line, "%s: %s\r\n", message, value ? value : "(null)");
    WriteFile(file, line, lstrlenA(line), &written, NULL);
    CloseHandle(file);
}

static void log_address_owner(const char *message, const void *address)
{
    HMODULE module = NULL;
    char path[MAX_PATH];
    MEMORY_BASIC_INFORMATION information;
    if (!address) {
        append_log(message, 0);
        return;
    }
    append_log(message, (DWORD)(ULONG_PTR)address);
    if (VirtualQuery(address, &information, sizeof(information)))
        append_log("DirectDraw route allocation base",
                (DWORD)(ULONG_PTR)information.AllocationBase);
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)address, &module) &&
            GetModuleFileNameA(module, path, sizeof(path)))
        append_log_text("DirectDraw route owner", path);
    else
        append_log("DirectDraw route owner error", GetLastError());
}

/* This only reads the shared DirectDraw vtable.  It is deliberately separate
 * from the route audit: no function pointer or COM reference count changes
 * are made while collecting this persistence evidence. */
static void log_directdraw_vtable(const char *prefix, IDirectDraw *ddraw)
{
    if (!ddraw || !ddraw->lpVtbl) {
        append_log(prefix, 0);
        return;
    }
    append_log(prefix, (DWORD)(ULONG_PTR)ddraw);
    append_log("DirectDraw persistence vtable", (DWORD)(ULONG_PTR)ddraw->lpVtbl);
    log_address_owner("DirectDraw persistence CreateSurface entry",
            ddraw->lpVtbl->CreateSurface);
}

static DWORD WINAPI directdraw_route_probe_thread(LPVOID unused)
{
    HMODULE ddraw;
    FARPROC create;
    (void)unused;
    /* qthook and QuickTime are loaded after the game window.  Waiting here
     * records the same export resolution after their startup work has run,
     * without creating a DirectDraw object or changing a vtable. */
    Sleep(2000);
    ddraw = GetModuleHandleA("ddraw.dll");
    append_log("DirectDraw route ddraw module", (DWORD)(ULONG_PTR)ddraw);
    create = ddraw ? GetProcAddress(ddraw, "DirectDrawCreate") : NULL;
    log_address_owner("DirectDraw route DirectDrawCreate", create);
    append_log("DirectDraw route qthook module",
            (DWORD)(ULONG_PTR)GetModuleHandleA("qthook.dll"));
    if (feature_directdraw_vtable_persistence_probe)
        log_directdraw_vtable("DirectDraw persistence retained qthook object",
                retained_qthook_ddraw);
    return 0;
}

static void start_directdraw_route_probe(void)
{
    HANDLE thread;
    if (!(feature_directdraw_route_probe || feature_directdraw_vtable_persistence_probe)
            || directdraw_route_probe_started) return;
    thread = CreateThread(NULL, 0, directdraw_route_probe_thread, NULL, 0, NULL);
    if (!thread) {
        append_log("DirectDraw route probe thread", GetLastError());
        return;
    }
    CloseHandle(thread);
    directdraw_route_probe_started = TRUE;
}

__declspec(naked) static FARPROC WINAPI call_original_GetProcAddress(
        HMODULE module, LPCSTR name)
{
    __asm {
        push ebp;
        mov ebp, esp;
        mov eax, dword ptr [original_GetProcAddress];
        jmp eax;
    }
}

static HRESULT WINAPI replacement_audited_DirectDrawCreate(GUID *guid,
        IDirectDraw **ddraw, IUnknown *outer_unknown)
{
    HRESULT result;
    const void *caller = _ReturnAddress();
    HMODULE caller_module = NULL;
    char path[MAX_PATH];
    if (!original_audited_DirectDrawCreate) return E_FAIL;
    result = original_audited_DirectDrawCreate(guid, ddraw, outer_unknown);
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)caller, &caller_module);
    if (feature_directdraw_proc_audit) {
        append_log("DirectDraw audit create result", (DWORD)result);
        append_log("DirectDraw audit create caller", (DWORD)(ULONG_PTR)caller);
        if (caller_module && GetModuleFileNameA(caller_module, path, sizeof(path)))
            append_log_text("DirectDraw audit create caller owner", path);
        if (SUCCEEDED(result) && ddraw && *ddraw) {
            append_log("DirectDraw audit object", (DWORD)(ULONG_PTR)*ddraw);
            append_log("DirectDraw audit vtable", (DWORD)(ULONG_PTR)(*ddraw)->lpVtbl);
            log_address_owner("DirectDraw audit CreateSurface entry",
                    (*ddraw)->lpVtbl->CreateSurface);
        }
    }
    if (feature_directdraw_vtable_persistence_probe && SUCCEEDED(result) && ddraw && *ddraw)
        log_directdraw_vtable("DirectDraw persistence returned object", *ddraw);
    /* qthook's own DllMain installs this pointer first, but the current
     * apphelp DirectDraw shim restores the shared vtable as it creates
     * QuickTime's object.  Reapply the already-loaded qthook handler only
     * after that object has been returned.  The handler's RVA is verified by
     * the Phase-27 build script against the fixed B26 qthook binary. */
    if (feature_directdraw_quicktime_surface_patch &&
            caller_module == GetModuleHandleA("QuickTime.qts") &&
            SUCCEEDED(result) && ddraw && *ddraw) {
        HMODULE qthook = GetModuleHandleA("qthook.dll");
        DWORD old_protect, ignored;
        FARPROC handler = qthook ? (FARPROC)((BYTE *)qthook
                + directdraw_qthook_create_surface_rva) : NULL;
        if (!handler || !VirtualProtect(&(*ddraw)->lpVtbl->CreateSurface,
                sizeof((*ddraw)->lpVtbl->CreateSurface), PAGE_EXECUTE_READWRITE,
                &old_protect)) {
            append_log("DirectDraw QuickTime CreateSurface patch", GetLastError());
        } else {
            qthook_compat_module = qthook;
            qthook_update_frontbuffer = qthook ? (BOOL *)((BYTE *)qthook
                    + VANILLA_QTHOOK_UPDATE_FRONTBUFFER_RVA) : NULL;
            qthook_internal_bitblt = qthook ? (BOOL *)((BYTE *)qthook
                    + VANILLA_QTHOOK_INTERNAL_BITBLT_RVA) : NULL;
            original_qthook_CreateSurface =
                    (direct_draw_create_surface_fn)handler;
            (*ddraw)->lpVtbl->CreateSurface = feature_qthook_overlay_compat
                    ? compat_qthook_CreateSurface
                    : (direct_draw_create_surface_fn)handler;
            VirtualProtect(&(*ddraw)->lpVtbl->CreateSurface,
                    sizeof((*ddraw)->lpVtbl->CreateSurface), old_protect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), &(*ddraw)->lpVtbl->CreateSurface,
                    sizeof((*ddraw)->lpVtbl->CreateSurface));
            append_log("DirectDraw QuickTime CreateSurface patched",
                    (DWORD)(ULONG_PTR)handler);
            if (feature_qthook_overlay_compat)
                append_log("qthook overlay CreateSurface wrapper",
                        (DWORD)(ULONG_PTR)compat_qthook_CreateSurface);
        }
        if (feature_qthook_overlay_compat && !install_qthook_gdi_compat())
            append_log("qthook overlay GDI compatibility", GetLastError());
    }
    if (feature_directdraw_qthook_object_pin && caller_module == GetModuleHandleA("qthook.dll") &&
            SUCCEEDED(result) && ddraw && *ddraw && !retained_qthook_ddraw) {
        (*ddraw)->lpVtbl->AddRef(*ddraw);
        retained_qthook_ddraw = *ddraw;
        append_log("DirectDraw qthook object retained",
                (DWORD)(ULONG_PTR)retained_qthook_ddraw);
    }
    return result;
}

static FARPROC WINAPI replacement_GetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC result = call_original_GetProcAddress(module, name);
    const void *caller = _ReturnAddress();
    HMODULE caller_module = NULL;
    char path[MAX_PATH];
    if (!(feature_directdraw_proc_audit || feature_directdraw_qthook_object_pin) || !result ||
            module != GetModuleHandleA("ddraw.dll") ||
            (ULONG_PTR)name <= 0xffff || lstrcmpiA(name, "DirectDrawCreate"))
        return result;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)caller, &caller_module))
        return result;
    if (feature_directdraw_proc_audit &&
            InterlockedIncrement(&directdraw_proc_audit_events) <= 16) {
        append_log("DirectDraw audit caller", (DWORD)(ULONG_PTR)caller);
        if (GetModuleFileNameA(caller_module, path, sizeof(path)))
            append_log_text("DirectDraw audit caller owner", path);
        else
            append_log("DirectDraw audit caller owner error", GetLastError());
        log_address_owner("DirectDraw audit result", result);
    }
    if (caller_module == GetModuleHandleA("qthook.dll") ||
            caller_module == GetModuleHandleA("QuickTime.qts")) {
        original_audited_DirectDrawCreate = (direct_draw_create_fn)result;
        return (FARPROC)replacement_audited_DirectDrawCreate;
    }
    return result;
}

static BOOL install_directdraw_proc_audit(void)
{
    BYTE *target = (BYTE *)GetProcAddress;
    DWORD old_protect, ignored;
    DWORD displacement;
    static const BYTE expected[] = { 0x8b, 0xff, 0x55, 0x8b, 0xec };
    if (!(feature_directdraw_proc_audit || feature_directdraw_qthook_object_pin) ||
            directdraw_proc_audit_hooked) return TRUE;
    if (target[0] != expected[0] || target[1] != expected[1] ||
            target[2] != expected[2] || target[3] != expected[3] ||
            target[4] != expected[4]) {
        append_log("DirectDraw audit GetProcAddress signature", *(DWORD *)target);
        return FALSE;
    }
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
        append_log("DirectDraw audit GetProcAddress protect", GetLastError());
        return FALSE;
    }
    original_GetProcAddress = (void *)(target + 5);
    displacement = (DWORD)(ULONG_PTR)replacement_GetProcAddress - (DWORD)(ULONG_PTR)target - 5;
    target[0] = 0xe9;
    *(DWORD *)(target + 1) = displacement;
    if (!VirtualProtect(target, 5, old_protect, &ignored)) {
        append_log("DirectDraw audit GetProcAddress restore", GetLastError());
        return FALSE;
    }
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    directdraw_proc_audit_hooked = TRUE;
    append_log("DirectDraw audit GetProcAddress hook", ERROR_SUCCESS);
    return TRUE;
}

static void warm_up_directdraw(void)
{
    typedef HRESULT (WINAPI *direct_draw_create_fn)(GUID *, IDirectDraw **, IUnknown *);
    HMODULE ddraw_module;
    direct_draw_create_fn direct_draw_create;
    IDirectDraw *ddraw = NULL;
    IDirectDrawSurface *surface = NULL;
    DDSURFACEDESC description;
    HRESULT result;
    if (directdraw_warmup_complete) return;
    directdraw_warmup_complete = TRUE;
    ddraw_module = LoadLibraryA("ddraw.dll");
    if (!ddraw_module) {
        append_log("DirectDraw warmup LoadLibrary", GetLastError());
        return;
    }
    direct_draw_create = (direct_draw_create_fn)GetProcAddress(ddraw_module,
            "DirectDrawCreate");
    if (!direct_draw_create) {
        append_log("DirectDraw warmup GetProcAddress", GetLastError());
        return;
    }
    result = direct_draw_create(NULL, &ddraw, NULL);
    append_log("DirectDraw warmup DirectDrawCreate", (DWORD)result);
    if (FAILED(result) || !ddraw) return;
    result = IDirectDraw_SetCooperativeLevel(ddraw, NULL, DDSCL_NORMAL);
    append_log("DirectDraw warmup SetCooperativeLevel", (DWORD)result);
    if (FAILED(result)) goto cleanup;
    ZeroMemory(&description, sizeof(description));
    description.dwSize = sizeof(description);
    description.dwFlags = DDSD_CAPS;
    description.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    result = IDirectDraw_CreateSurface(ddraw, &description, &surface, NULL);
    append_log("DirectDraw warmup CreateSurface primary", (DWORD)result);
cleanup:
    if (surface) IDirectDrawSurface_Release(surface);
    if (ddraw) IDirectDraw_Release(ddraw);
}

/* qthook is normally loaded by QuickTime.qts only after Bad Mojo has begun
 * initializing its renderer.  In a native-desktop, windowed launch that can
 * be too late for qthook's own temporary primary-surface probe.  Loading the
 * shipped QuickTime bridge after LittleCRT exists, but before the game resumes
 * renderer setup, makes the *unmodified* qthook perform that same setup in
 * its intended order.  QuickTime.qts deliberately returns FALSE from its
 * DllMain, so the bridge load itself is expected to fail; qthook.dll is the
 * successful nested load and is what we verify. */
static void initialize_qthook_early(void)
{
    HMODULE bridge;
    DWORD result;
    if (qthook_early_initialize_attempted) return;
    qthook_early_initialize_attempted = TRUE;
    if (GetModuleHandleA("qthook.dll")) {
        append_log("qthook early initialization already loaded", ERROR_SUCCESS);
        return;
    }
    SetLastError(ERROR_SUCCESS);
    bridge = LoadLibraryA("quicktime.qts");
    result = GetModuleHandleA("qthook.dll") ? ERROR_SUCCESS : GetLastError();
    append_log("qthook early initialization", result);
    if (bridge) FreeLibrary(bridge);
}

/* qthook creates its QuickTime presentation surface at the dimensions
 * returned by IDirectDraw::GetDisplayMode.  That was 640x480 while the
 * original launcher owned the display mode.  In the windowed route it is the
 * native desktop instead, which leaves QuickTime drawing outside the 640x480
 * game client.  Keep the correction in the enhancement DLL: qthook.dll and
 * its on-disk hash remain untouched. */
static HRESULT WINAPI replacement_DirectDraw_GetDisplayMode(IDirectDraw *ddraw,
        DDSURFACEDESC *description)
{
    HRESULT result = original_DirectDraw_GetDisplayMode(ddraw, description);
    if (SUCCEEDED(result) && feature_windowed_main_window && description) {
        description->dwFlags |= DDSD_WIDTH | DDSD_HEIGHT;
        description->dwWidth = windowed_width;
        description->dwHeight = windowed_height;
        if (!directdraw_display_mode_logged) {
            append_log("DirectDraw display mode redirected",
                    ((DWORD)windowed_width << 16) | (DWORD)windowed_height);
            directdraw_display_mode_logged = TRUE;
        }
    }
    return result;
}

static void install_directdraw_display_mode_hook(void)
{
    typedef HRESULT (WINAPI *direct_draw_create_fn)(GUID *, IDirectDraw **,
            IUnknown *);
    HMODULE module;
    direct_draw_create_fn create;
    IDirectDraw *ddraw = NULL;
    HRESULT result;
    DWORD old_protect, ignored;
    if (directdraw_display_mode_hooked || !feature_windowed_main_window) return;
    module = GetModuleHandleA("ddraw.dll");
    create = module ? (direct_draw_create_fn)GetProcAddress(module,
            "DirectDrawCreate") : NULL;
    if (!create) {
        append_log("DirectDraw display mode hook lookup", GetLastError());
        return;
    }
    result = create(NULL, &ddraw, NULL);
    if (FAILED(result) || !ddraw) {
        append_log("DirectDraw display mode hook create", (DWORD)result);
        return;
    }
    original_DirectDraw_GetDisplayMode = ddraw->lpVtbl->GetDisplayMode;
    if (!VirtualProtect(&ddraw->lpVtbl->GetDisplayMode,
            sizeof(ddraw->lpVtbl->GetDisplayMode), PAGE_EXECUTE_READWRITE,
            &old_protect)) {
        append_log("DirectDraw display mode hook protect", GetLastError());
        IDirectDraw_Release(ddraw);
        return;
    }
    ddraw->lpVtbl->GetDisplayMode = replacement_DirectDraw_GetDisplayMode;
    VirtualProtect(&ddraw->lpVtbl->GetDisplayMode,
            sizeof(ddraw->lpVtbl->GetDisplayMode), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &ddraw->lpVtbl->GetDisplayMode,
            sizeof(ddraw->lpVtbl->GetDisplayMode));
    directdraw_display_mode_hooked = TRUE;
    append_log("DirectDraw display mode hook", ERROR_SUCCESS);
    IDirectDraw_Release(ddraw);
}

/* qthook is loaded by QuickTime after the game window is created.  Install
 * the display-mode correction only after qthook has installed its DirectDraw
 * vtable hooks, so its later QuickTime surface creation sees 640x480. */
static DWORD WINAPI install_directdraw_display_mode_after_qthook(LPVOID unused)
{
    DWORD deadline = GetTickCount() + 10000;
    (void)unused;
    while ((!GetModuleHandleA("qthook.dll") || !GetModuleHandleA("ddraw.dll"))
            && (LONG)(deadline - GetTickCount()) > 0)
        Sleep(25);
    if (GetModuleHandleA("qthook.dll") && GetModuleHandleA("ddraw.dll"))
        install_directdraw_display_mode_hook();
    else
        append_log("DirectDraw display mode hook timeout", ERROR_TIMEOUT);
    return 0;
}

static void start_directdraw_display_mode_hook(void)
{
    HANDLE thread;
    if (directdraw_display_mode_thread_started || !feature_directdraw_display_mode_override)
        return;
    thread = CreateThread(NULL, 0, install_directdraw_display_mode_after_qthook,
            NULL, 0, NULL);
    if (!thread) {
        append_log("DirectDraw display mode hook thread", GetLastError());
        return;
    }
    CloseHandle(thread);
    directdraw_display_mode_thread_started = TRUE;
}

static void initialize_paths(void)
{
    DWORD length = GetCurrentDirectoryA(MAX_PATH, ini_path);
    if (!length || length >= MAX_PATH - 48) {
        ini_path[0] = 0;
        log_path[0] = 0;
        return;
    }
    lstrcpyA(log_path, ini_path);
    if (length && ini_path[length - 1] != '\\') {
        lstrcatA(ini_path, "\\");
        lstrcatA(log_path, "\\");
    }
    lstrcatA(ini_path, "badmojo.INI");
    lstrcatA(log_path, "BadMojoEnhancements-Phase1.log");
    DeleteFileA(log_path);
}

static void load_settings(void)
{
    if (!ini_path[0]) return;
    feature_save_dialog_defaults = GetPrivateProfileIntA(
            "BadMojoEnhancements", "SaveDialogDefaults", 1, ini_path) != 0;
    feature_skip_startup_logos = GetPrivateProfileIntA(
            "BadMojoEnhancements", "SkipStartupLogos", 1, ini_path) != 0;
    feature_windowed_main_window = GetPrivateProfileIntA(
            "BadMojoEnhancements", "WindowedMainWindow", 0, ini_path) != 0;
    feature_lives_overlay = GetPrivateProfileIntA(
            "BadMojoEnhancements", "LivesOverlayEnabled", 1, ini_path) != 0;
    feature_directdraw_route_probe = GetPrivateProfileIntA(
            "BadMojoEnhancements", "DirectDrawRouteProbeEnabled", 0, ini_path) != 0;
    feature_directdraw_proc_audit = GetPrivateProfileIntA(
            "BadMojoEnhancements", "DirectDrawProcAuditEnabled", 0, ini_path) != 0;
    feature_directdraw_qthook_object_pin = GetPrivateProfileIntA(
            "BadMojoEnhancements", "DirectDrawQTHookObjectPinEnabled", 0, ini_path) != 0;
    feature_directdraw_vtable_persistence_probe = GetPrivateProfileIntA(
            "BadMojoEnhancements", "DirectDrawVtablePersistenceProbeEnabled", 0,
            ini_path) != 0;
    feature_directdraw_quicktime_surface_patch = GetPrivateProfileIntA(
            "BadMojoEnhancements", "DirectDrawQuickTimeSurfacePatchEnabled", 0,
            ini_path) != 0;
    feature_qthook_overlay_compat = GetPrivateProfileIntA(
            "BadMojoEnhancements", "QTHookOverlayCompatEnabled", 0,
            ini_path) != 0;
    {
        DWORD rva = GetPrivateProfileIntA("BadMojoEnhancements",
                "QTHookCreateSurfaceRva", 0x2960, ini_path);
        if (rva >= 0x1000 && rva < 0x100000)
            directdraw_qthook_create_surface_rva = rva;
    }
    feature_directdraw_warmup = GetPrivateProfileIntA(
            "BadMojoEnhancements", "DirectDrawWarmup", 1, ini_path) != 0;
    feature_qthook_early_initialize = GetPrivateProfileIntA(
            "BadMojoEnhancements", "QTHookEarlyInitialize", 1, ini_path) != 0;
    feature_directdraw_display_mode_override = GetPrivateProfileIntA(
            "BadMojoEnhancements", "DirectDrawDisplayModeOverride", 0, ini_path) != 0;
    lives_visible = GetPrivateProfileIntA(
            "BadMojoEnhancements", "LivesOverlay", 1, ini_path) != 0;
    {
        int key = GetPrivateProfileIntA("BadMojoEnhancements", "LivesToggleKey",
                LIVES_DEFAULT_TOGGLE_KEY, ini_path);
        int duration = GetPrivateProfileIntA("BadMojoEnhancements", "LivesNotifyMs",
                LIVES_DEFAULT_DISPLAY_MS, ini_path);
        if (key >= 1 && key <= 0xff) lives_toggle_key = key;
        if (duration >= 250 && duration <= 10000)
            lives_change_display_ms = (DWORD)duration;
    }
}

static void windowed_position(int *x, int *y)
{
    RECT work;
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
        *x = work.left + ((work.right - work.left) - windowed_width) / 2;
        *y = work.top + ((work.bottom - work.top) - windowed_height) / 2;
    } else {
        *x = (GetSystemMetrics(SM_CXSCREEN) - windowed_width) / 2;
        *y = (GetSystemMetrics(SM_CYSCREEN) - windowed_height) / 2;
    }
}

static BOOL is_main_window_request(LPCSTR class_name, LPCSTR window_name)
{
    return (ULONG_PTR)class_name > 0xffff && (ULONG_PTR)window_name > 0xffff
            && !lstrcmpiA(class_name, "LittleCRT")
            && !lstrcmpiA(window_name, "Bad Mojo");
}

static HWND WINAPI replacement_CreateWindowExA(DWORD exstyle, LPCSTR class_name,
        LPCSTR window_name, DWORD style, int x, int y, int width, int height,
        HWND parent, HMENU menu, HINSTANCE instance, LPVOID param)
{
    HWND window;
    if (is_main_window_request(class_name, window_name)) {
        windowed_position(&x, &y);
        style = WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        width = windowed_width;
        height = windowed_height;
        append_log("windowed CreateWindowExA", (DWORD)style);
    }
    window = original_CreateWindowExA(exstyle, class_name, window_name, style,
            x, y, width, height, parent, menu, instance, param);
    if (is_main_window_request(class_name, window_name)) {
        windowed_main_window = window;
        if (feature_directdraw_warmup) warm_up_directdraw();
        if (feature_qthook_early_initialize) initialize_qthook_early();
        start_directdraw_display_mode_hook();
        start_directdraw_route_probe();
        if (feature_lives_overlay) start_lives_timer(window);
    }
    return window;
}

static int WINAPI replacement_GetDeviceCaps(HDC dc, int index)
{
    if (windowed_main_window && WindowFromDC(dc) == windowed_main_window) {
        if (index == HORZRES) return windowed_width;
        if (index == VERTRES) return windowed_height;
    }
    return original_GetDeviceCaps(dc, index);
}

static BOOL WINAPI replacement_SetWindowPos(HWND window, HWND insert_after,
        int x, int y, int width, int height, UINT flags)
{
    if (windowed_main_window && window == windowed_main_window) {
        windowed_position(&x, &y);
        width = windowed_width;
        height = windowed_height;
        if (insert_after == HWND_TOPMOST)
            insert_after = HWND_NOTOPMOST;
        append_log("windowed SetWindowPos", (DWORD)width << 16 | (DWORD)height);
    }
    return original_SetWindowPos(window, insert_after, x, y, width, height, flags);
}

static int *badmojo_value(DWORD rva)
{
    return (int *)(badmojo_image + rva);
}

/* Overlay housekeeping must not make the shipped qthook believe the game
 * changed the window. Otherwise the next frontbuffer lock copies the overlay
 * into DirectDraw and makes its saved background stale. */
static BOOL overlay_bitblt(HDC destination, int x, int y, int width,
        int height, HDC source, int source_x, int source_y, DWORD rop)
{
    BOOL previous_internal = FALSE;
    BOOL result;
    if (feature_qthook_overlay_compat && qthook_internal_bitblt) {
        previous_internal = *qthook_internal_bitblt;
        *qthook_internal_bitblt = TRUE;
    }
    if (feature_qthook_overlay_compat && original_qthook_BitBlt)
        result = original_qthook_BitBlt(destination, x, y, width, height,
                source, source_x, source_y, rop);
    else
        result = BitBlt(destination, x, y, width, height, source,
                source_x, source_y, rop);
    if (feature_qthook_overlay_compat && qthook_internal_bitblt)
        *qthook_internal_bitblt = previous_internal;
    return result;
}

static void capture_life_icon(HDC source)
{
    int source_x, source_y;
    HBITMAP bitmap;
    if (!source || !badmojo_image) return;
    if (!life_icon_dc) life_icon_dc = CreateCompatibleDC(source);
    if (!life_icon_dc) return;
    if (!life_icon_bitmap) {
        bitmap = CreateCompatibleBitmap(source, LIFE_ICON_SIZE, LIFE_ICON_SIZE);
        if (!bitmap) return;
        life_icon_previous = SelectObject(life_icon_dc, bitmap);
        if (!life_icon_previous || life_icon_previous == HGDI_ERROR) {
            DeleteObject(bitmap);
            life_icon_previous = NULL;
            return;
        }
        life_icon_bitmap = bitmap;
    }
    source_x = *badmojo_value(BADMOJO_VIEWPORT_X_RVA);
    source_y = *badmojo_value(BADMOJO_VIEWPORT_Y_RVA) - 30;
    overlay_bitblt(life_icon_dc, 0, 0, LIFE_ICON_SIZE, LIFE_ICON_SIZE,
            source, source_x, source_y, SRCCOPY);
    life_icon_transparent_color = GetPixel(life_icon_dc, 0, 0);
}

static void erase_lives_overlay(HDC destination)
{
    if (!lives_drawn || !destination || !lives_backing_dc) return;
    overlay_bitblt(destination, lives_drawn_x, lives_drawn_y,
            LIVES_OVERLAY_WIDTH, LIFE_ICON_SIZE, lives_backing_dc, 0, 0,
            SRCCOPY);
    lives_drawn = FALSE;
    lives_drawn_count = -1;
}

static BOOL lives_should_be_visible(void)
{
    return !lives_pause_menu_active && (lives_visible
            || (LONG)(lives_change_visible_until - GetTickCount()) > 0);
}

static BOOL lives_overlay_intersects(int x, int y, int width, int height)
{
    int left = x;
    int top = y;
    int right = x + width;
    int bottom = y + height;
    int swap;
    if (!lives_drawn) return TRUE;
    if (right < left) { swap = left; left = right; right = swap; }
    if (bottom < top) { swap = top; top = bottom; bottom = swap; }
    return left < lives_drawn_x + LIVES_OVERLAY_WIDTH
            && right > lives_drawn_x
            && top < lives_drawn_y + LIFE_ICON_SIZE
            && bottom > lives_drawn_y;
}

/* Bounded evidence only: reaching the limit prevents later events from
 * appearing, so absence of an entry does not prove a bypass. */
static void log_lives_overlap(const char *route, LONG *counter,
        int x, int y, int width, int height, BOOL internal)
{
    LONG event = InterlockedIncrement(counter);
    char details[128];
    if (event > LIVES_OVERLAP_LOG_LIMIT) return;
    wsprintfA(details, "event=%ld x=%d y=%d width=%d height=%d internal=%d",
            event, x, y, width, height, internal);
    append_log_text(route, details);
}

static void draw_lives_overlay(HDC destination)
{
    int i, x, y, death_count;
    if (!destination || !badmojo_image) return;
    erase_lives_overlay(destination);
    if (!lives_available || !lives_should_be_visible()
            || (feature_qthook_overlay_compat && movie_active) || !life_icon_bitmap
            || !overlay_TransparentBlt)
        return;
    death_count = *badmojo_value(BADMOJO_DEATH_COUNT_RVA);
    if (death_count >= 0 && death_count <= 3)
        lives_remaining = 4 - death_count;
    if (lives_remaining < 1 || lives_remaining > 4) return;
    x = *badmojo_value(BADMOJO_VIEWPORT_X_RVA) + 8;
    y = *badmojo_value(BADMOJO_VIEWPORT_Y_RVA) + 8;
    if (!lives_backing_dc)
        lives_backing_dc = CreateCompatibleDC(destination);
    if (!lives_backing_dc) return;
    if (!lives_backing_bitmap) {
        lives_backing_bitmap = CreateCompatibleBitmap(destination,
                LIVES_OVERLAY_WIDTH, LIFE_ICON_SIZE);
        if (!lives_backing_bitmap) return;
        lives_backing_previous = SelectObject(lives_backing_dc,
                lives_backing_bitmap);
        if (!lives_backing_previous || lives_backing_previous == HGDI_ERROR) {
            DeleteObject(lives_backing_bitmap);
            lives_backing_bitmap = NULL;
            lives_backing_previous = NULL;
            return;
        }
    }
    overlay_bitblt(lives_backing_dc, 0, 0, LIVES_OVERLAY_WIDTH,
            LIFE_ICON_SIZE, destination, x, y, SRCCOPY);
    /* Defensive re-entry guard. Phase 39 did not establish that
     * TransparentBlt actually re-enters these hooks. */
    InterlockedIncrement(&lives_overlay_composition_depth);
    for (i = 0; i < lives_remaining; ++i)
        overlay_TransparentBlt(destination, x + i * LIFE_ICON_STEP, y,
                LIFE_ICON_SIZE, LIFE_ICON_SIZE, life_icon_dc, 0, 0,
                LIFE_ICON_SIZE, LIFE_ICON_SIZE, life_icon_transparent_color);
    InterlockedDecrement(&lives_overlay_composition_depth);
    lives_drawn = TRUE;
    lives_drawn_count = lives_remaining;
    lives_drawn_x = x;
    lives_drawn_y = y;
}

/* Complete the clean background, game draw and icons offscreen. The visible
 * window receives only the finished image. Restrict to the game's ordinary
 * identity-mapped client DC; other DCs retain the previous path. */
static HDC begin_lives_frame(HDC destination)
{
    RECT client;
    POINT origin;
    int width, height;
    if (!lives_main_window || movie_active || !lives_available ||
            GetMapMode(destination) != MM_TEXT ||
            !GetViewportOrgEx(destination, &origin) || origin.x || origin.y ||
            !GetWindowOrgEx(destination, &origin) || origin.x || origin.y ||
            !GetClientRect(lives_main_window, &client)) return NULL;
    width = client.right; height = client.bottom;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return NULL;
    if (!lives_frame_dc) lives_frame_dc = CreateCompatibleDC(destination);
    if (!lives_frame_dc) return NULL;
    if (!lives_frame_bitmap || width != lives_frame_width || height != lives_frame_height) {
        HBITMAP bitmap = CreateCompatibleBitmap(destination, width, height);
        HGDIOBJ old;
        if (!bitmap) return NULL;
        old = SelectObject(lives_frame_dc, bitmap);
        if (!old || old == HGDI_ERROR) { DeleteObject(bitmap); return NULL; }
        if (lives_frame_bitmap) DeleteObject(lives_frame_bitmap);
        else lives_frame_original = old;
        lives_frame_bitmap = bitmap;
        lives_frame_width = width; lives_frame_height = height;
    }
    if (!overlay_bitblt(lives_frame_dc, 0, 0, width, height,
            destination, 0, 0, SRCCOPY)) return NULL;
    return lives_frame_dc;
}

static void present_lives_frame(HDC destination)
{
    overlay_bitblt(destination, 0, 0, lives_frame_width, lives_frame_height,
            lives_frame_dc, 0, 0, SRCCOPY);
    if (InterlockedIncrement(&lives_frame_events) <= 4)
        append_log("lives completed frame presented", lives_frame_events);
}

static void redraw_lives_now(void)
{
    HDC destination;
    HDC frame;
    if (!lives_main_window) return;
    destination = GetDC(lives_main_window);
    if (!destination) return;
    frame = begin_lives_frame(destination);
    draw_lives_overlay(frame ? frame : destination);
    if (frame) present_lives_frame(destination);
    ReleaseDC(lives_main_window, destination);
}

static BOOL compat_surface_is_frontbuffer(IDirectDrawSurface *surface)
{
    IDirectDrawSurface4 *surface4 = NULL;
    DWORD data;
    DWORD size = sizeof(data);
    static const GUID frontbuffer_guid = {
        0x345826e0, 0xc729, 0x46bf,
        { 0x9d, 0x63, 0x9d, 0x10, 0xb7, 0x42, 0x21, 0x47 }
    };
    BOOL result = FALSE;
    if (!surface || FAILED(IDirectDrawSurface_QueryInterface(surface,
            &IID_IDirectDrawSurface4, (void **)&surface4)) || !surface4)
        return FALSE;
    result = SUCCEEDED(IDirectDrawSurface4_GetPrivateData(surface4,
            &frontbuffer_guid, &data, &size));
    IDirectDrawSurface4_Release(surface4);
    return result;
}

static BOOL install_qthook_surface_compat(IDirectDrawSurface *surface)
{
    IDirectDrawSurfaceVtbl *vtable;
    surface_lock_fn expected_lock;
    surface_unlock_fn expected_unlock;
    DWORD old_protect, ignored;
    if (!feature_qthook_overlay_compat || !qthook_compat_module || !surface)
        return FALSE;
    vtable = surface->lpVtbl;
    expected_lock = (surface_lock_fn)((BYTE *)qthook_compat_module
            + VANILLA_QTHOOK_SURFACE_LOCK_RVA);
    expected_unlock = (surface_unlock_fn)((BYTE *)qthook_compat_module
            + VANILLA_QTHOOK_SURFACE_UNLOCK_RVA);
    if (vtable->Lock == compat_qthook_surface_lock
            && vtable->Unlock == compat_qthook_surface_unlock)
        return TRUE;
    if (vtable->Lock != expected_lock || vtable->Unlock != expected_unlock) {
        append_log("qthook overlay unexpected surface Lock",
                (DWORD)(ULONG_PTR)vtable->Lock);
        append_log("qthook overlay unexpected surface Unlock",
                (DWORD)(ULONG_PTR)vtable->Unlock);
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    if (!VirtualProtect(vtable, sizeof(*vtable), PAGE_EXECUTE_READWRITE,
            &old_protect)) return FALSE;
    original_qthook_surface_lock = expected_lock;
    original_qthook_surface_unlock = expected_unlock;
    vtable->Lock = compat_qthook_surface_lock;
    vtable->Unlock = compat_qthook_surface_unlock;
    if (!VirtualProtect(vtable, sizeof(*vtable), old_protect, &ignored))
        return FALSE;
    FlushInstructionCache(GetCurrentProcess(), vtable, sizeof(*vtable));
    qthook_surface_compat_hooked = TRUE;
    append_log("qthook overlay surface wrappers", (DWORD)(ULONG_PTR)vtable);
    return TRUE;
}

static HRESULT WINAPI compat_qthook_CreateSurface(IDirectDraw *ddraw,
        DDSURFACEDESC *description, IDirectDrawSurface **surface,
        IUnknown *outer_unknown)
{
    HRESULT result;
    if (!original_qthook_CreateSurface) return E_FAIL;
    result = original_qthook_CreateSurface(ddraw, description, surface,
            outer_unknown);
    if (SUCCEEDED(result) && surface && *surface
            && !install_qthook_surface_compat(*surface))
        append_log("qthook overlay surface compatibility", GetLastError());
    return result;
}

static HRESULT WINAPI compat_qthook_surface_lock(IDirectDrawSurface *surface,
        RECT *rect, DDSURFACEDESC *description, DWORD flags, HANDLE event)
{
    BOOL frontbuffer = compat_surface_is_frontbuffer(surface);
    BOOL update = frontbuffer && qthook_update_frontbuffer
            && *qthook_update_frontbuffer;
    HDC destination = NULL;
    DWORD now;
    HRESULT result;
    if (frontbuffer) {
        now = GetTickCount();
        if (now - movie_last_lock_tick <= 100)
            ++movie_lock_streak;
        else
            movie_lock_streak = 1;
        movie_last_lock_tick = now;
        if (movie_lock_streak >= 3 && !movie_active) {
            movie_active = TRUE;
        }
        if (lives_drawn && (update || movie_active)) {
            log_lives_overlap("lives overlap frontbuffer lock",
                    &lives_overlap_frontbuffer_logs, 0, 0, 0, 0, update);
            destination = GetDC(lives_main_window);
            if (destination) {
                erase_lives_overlay(destination);
                ReleaseDC(lives_main_window, destination);
            }
        }
    }
    result = original_qthook_surface_lock(surface, rect, description, flags,
            event);
    return result;
}

static HRESULT WINAPI compat_qthook_surface_unlock(IDirectDrawSurface *surface,
        void *data)
{
    return original_qthook_surface_unlock(surface, data);
}

static int WINAPI compat_qthook_StretchDIBits(HDC destination, int x_dst,
        int y_dst, int width_dst, int height_dst, int x_src, int y_src,
        int width_src, int height_src, const void *bits,
        const BITMAPINFO *bitmapinfo, UINT usage, DWORD rop)
{
    BOOL is_main = lives_main_window
            && WindowFromDC(destination) == lives_main_window;
    BOOL affects = lives_overlay_intersects(x_dst, y_dst, width_dst,
            height_dst);
    BOOL composing = InterlockedCompareExchange(&lives_overlay_composition_depth,
            0, 0) != 0;
    int result;
    HDC frame = NULL;
    if (is_main && affects && !composing && !lives_pause_menu_active)
        frame = begin_lives_frame(destination);
    if (frame) {
        erase_lives_overlay(frame);
        result = original_qthook_StretchDIBits(frame, x_dst, y_dst,
                width_dst, height_dst, x_src, y_src, width_src, height_src,
                bits, bitmapinfo, usage, rop);
        draw_lives_overlay(frame);
        present_lives_frame(destination);
        return result;
    }
    if (is_main && affects && !composing)
        log_lives_overlap("lives overlap StretchDIBits", &lives_overlap_stretch_logs,
                x_dst, y_dst, width_dst, height_dst, FALSE);
    if (is_main && affects && !composing) erase_lives_overlay(destination);
    result = original_qthook_StretchDIBits(destination, x_dst, y_dst,
            width_dst, height_dst, x_src, y_src, width_src, height_src,
            bits, bitmapinfo, usage, rop);
    if (is_main && affects && !composing) draw_lives_overlay(destination);
    return result;
}

static BOOL WINAPI compat_qthook_BitBlt(HDC destination, int x, int y,
        int width, int height, HDC source, int source_x, int source_y,
        DWORD rop)
{
    BOOL internal = qthook_internal_bitblt && *qthook_internal_bitblt;
    BOOL is_main = lives_main_window
            && WindowFromDC(destination) == lives_main_window;
    BOOL affects = lives_overlay_intersects(x, y, width, height);
    BOOL composing = InterlockedCompareExchange(&lives_overlay_composition_depth,
            0, 0) != 0;
    BOOL result;
    if (is_main && affects && !composing)
        log_lives_overlap("lives overlap BitBlt", &lives_overlap_bitblt_logs,
                x, y, width, height, internal);
    if (!composing && !internal && is_main && affects) erase_lives_overlay(destination);
    result = original_qthook_BitBlt(destination, x, y, width, height,
            source, source_x, source_y, rop);
    if (is_main && affects && !composing) {
        if (internal) {
            /* qthook has just completed its frontbuffer-to-window transfer.
             * The old overlay area was replaced, so do not restore the old
             * backing over these fresh pixels.  Recompose before qthook
             * returns from Unlock to avoid exposing a blank frame. */
            lives_drawn = FALSE;
            lives_drawn_count = -1;
            if (!movie_active) draw_lives_overlay(destination);
        } else {
            draw_lives_overlay(destination);
        }
    }
    return result;
}

static BOOL install_qthook_gdi_compat(void)
{
    BYTE *stretch = (BYTE *)StretchDIBits;
    BYTE *bitblt = (BYTE *)BitBlt;
    void *stretch_target;
    void *bitblt_target;
    LONG displacement;
    DWORD old_protect, ignored;
    if (qthook_gdi_compat_hooked) return TRUE;
    if (!qthook_compat_module || stretch[0] != 0xe9 || bitblt[0] != 0xe9) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    stretch_target = stretch + 5 + *(LONG *)(stretch + 1);
    bitblt_target = bitblt + 5 + *(LONG *)(bitblt + 1);
    if (stretch_target != (BYTE *)qthook_compat_module
                    + VANILLA_QTHOOK_STRETCHDIBITS_RVA
            || bitblt_target != (BYTE *)qthook_compat_module
                    + VANILLA_QTHOOK_BITBLT_RVA) {
        append_log("qthook overlay unexpected StretchDIBits",
                (DWORD)(ULONG_PTR)stretch_target);
        append_log("qthook overlay unexpected BitBlt",
                (DWORD)(ULONG_PTR)bitblt_target);
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    original_qthook_StretchDIBits = (stretch_dibits_fn)stretch_target;
    original_qthook_BitBlt = (bitblt_fn)bitblt_target;
    if (!VirtualProtect(stretch, 5, PAGE_EXECUTE_READWRITE, &old_protect))
        return FALSE;
    displacement = (LONG)((BYTE *)compat_qthook_StretchDIBits - (stretch + 5));
    *(LONG *)(stretch + 1) = displacement;
    if (!VirtualProtect(stretch, 5, old_protect, &ignored)) return FALSE;
    if (!VirtualProtect(bitblt, 5, PAGE_EXECUTE_READWRITE, &old_protect))
        return FALSE;
    displacement = (LONG)((BYTE *)compat_qthook_BitBlt - (bitblt + 5));
    *(LONG *)(bitblt + 1) = displacement;
    if (!VirtualProtect(bitblt, 5, old_protect, &ignored)) return FALSE;
    FlushInstructionCache(GetCurrentProcess(), stretch, 5);
    FlushInstructionCache(GetCurrentProcess(), bitblt, 5);
    qthook_gdi_compat_hooked = TRUE;
    append_log("qthook overlay GDI wrappers", ERROR_SUCCESS);
    return TRUE;
}

static VOID CALLBACK lives_timer_proc(HWND window, UINT message,
        UINT_PTR timer_id, DWORD time)
{
    BOOL down;
    DWORD now;
    (void)window; (void)message; (void)timer_id; (void)time;
    now = GetTickCount();
    down = (GetAsyncKeyState(lives_toggle_key) & 0x8000) != 0;
    if (down && !lives_key_down) {
        lives_visible = !lives_visible;
        WritePrivateProfileStringA("BadMojoEnhancements", "LivesOverlay",
                lives_visible ? "1" : "0", ini_path);
        redraw_lives_now();
    }
    lives_key_down = down;
    if (feature_qthook_overlay_compat && movie_active
            && now - movie_last_lock_tick > 150) {
        movie_active = FALSE;
        movie_lock_streak = 0;
        if (lives_change_pending) {
            lives_change_pending = FALSE;
            if (!lives_visible)
                lives_change_visible_until = now + lives_change_display_ms;
            append_log("lives callback notification released",
                    lives_known_count);
        }
        redraw_lives_now();
    }
    if (!lives_available || !badmojo_image) return;
    if (!lives_visible && lives_drawn
            && (LONG)(lives_change_visible_until - now) <= 0) {
        redraw_lives_now();
    }
}

static void __cdecl overlay_draw_lives(HDC destination, int first, int last)
{
    BOOL game_display_event;
    DWORD now;
    original_draw_lives(destination, first, last);
    lives_last_callback_first = first;
    lives_last_callback_last = last;
    append_log("lives callback", ((DWORD)(first & 0xffff) << 16)
            | (DWORD)(last & 0xffff));
    if (last == 3 && first >= 0 && first <= 3) {
        capture_life_icon(destination);
        lives_remaining = 4 - first;
        lives_available = life_icon_bitmap != NULL;
        /* draw_lives is the game's own display event, including the first
         * post-load display.  Do not suppress that initial callback: with F9
         * off it is the only notification emitted by a freshly loaded save. */
        game_display_event = TRUE;
        lives_known_count = lives_remaining;
        if (feature_qthook_overlay_compat && movie_active) {
            lives_change_pending = game_display_event;
            append_log("lives callback deferred for movie", lives_remaining);
            return;
        }
        if (!lives_visible && game_display_event) {
            now = GetTickCount();
            lives_change_visible_until = now + lives_change_display_ms;
            append_log("lives callback notification started", lives_remaining);
        }
        draw_lives_overlay(destination);
    }
}

static BOOL install_lives_overlay_hook(void)
{
    BYTE *thunk;
    BYTE *target;
    DWORD old_protect, ignored;
    LONG displacement;
    badmojo_image = (BYTE *)GetModuleHandleA(NULL);
    if (!badmojo_image) return FALSE;
    thunk = badmojo_image + BADMOJO_DRAW_LIVES_THUNK_RVA;
    if (thunk[0] != 0xe9) return FALSE;
    target = thunk + 5 + *(LONG *)(thunk + 1);
    if (target != badmojo_image + BADMOJO_DRAW_LIVES_RVA) return FALSE;
    original_draw_lives = (draw_lives_fn)target;
    displacement = (LONG)((BYTE *)overlay_draw_lives - (thunk + 5));
    if (!VirtualProtect(thunk, 5, PAGE_EXECUTE_READWRITE, &old_protect)) return FALSE;
    *(LONG *)(thunk + 1) = displacement;
    if (!VirtualProtect(thunk, 5, old_protect, &ignored)) return FALSE;
    FlushInstructionCache(GetCurrentProcess(), thunk, 5);
    return TRUE;
}

/* The pause menu is the game's GAME script, not a USER32 modal dialog.
 * Input handling saves _saveit.bmx, resolves GAME through SLADY.LUT (916),
 * then invokes this scene setter. Resume Game restores the saved scene through
 * the same setter.  This observes those game-owned transitions only. */
static void __cdecl overlay_scene_switch(int scene)
{
    BOOL pause_menu = scene == BADMOJO_PAUSE_MENU_SCENE;
    original_scene_switch(scene);
    if ((BOOL)InterlockedExchange(&lives_pause_menu_active, pause_menu) != pause_menu) {
        append_log(pause_menu ? "lives pause menu entered" : "lives pause menu exited",
                (DWORD)scene);
        if (pause_menu)
            redraw_lives_now();
    }
}

static BOOL install_lives_scene_switch_hook(void)
{
    BYTE *thunk;
    BYTE *target;
    DWORD old_protect, ignored;
    LONG displacement;
    if (!badmojo_image) return FALSE;
    thunk = badmojo_image + BADMOJO_SCENE_SWITCH_THUNK_RVA;
    if (thunk[0] != 0xe9) return FALSE;
    target = thunk + 5 + *(LONG *)(thunk + 1);
    if (target != badmojo_image + BADMOJO_SCENE_SWITCH_RVA) return FALSE;
    original_scene_switch = (scene_switch_fn)target;
    displacement = (LONG)((BYTE *)overlay_scene_switch - (thunk + 5));
    if (!VirtualProtect(thunk, 5, PAGE_EXECUTE_READWRITE, &old_protect)) return FALSE;
    *(LONG *)(thunk + 1) = displacement;
    if (!VirtualProtect(thunk, 5, old_protect, &ignored)) return FALSE;
    FlushInstructionCache(GetCurrentProcess(), thunk, 5);
    return TRUE;
}

static DWORD WINAPI install_lives_hook_after_startup(LPVOID unused)
{
    DWORD deadline = GetTickCount() + 10000;
    HMODULE msimg32;
    (void)unused;
    /* qthook initializes its DirectDraw vtables during process startup. Do
     * not modify a game code thunk until qthook is present and has had a
     * bounded settling interval; the E9 displacement itself is then a single
     * aligned write between two valid destinations. */
    while (!GetModuleHandleA("qthook.dll")
            && (LONG)(deadline - GetTickCount()) > 0)
        Sleep(25);
    Sleep(750);
    msimg32 = LoadLibraryA("msimg32.dll");
    overlay_TransparentBlt = msimg32 ? (transparent_blt_fn)GetProcAddress(
            msimg32, "TransparentBlt") : NULL;
    if (!overlay_TransparentBlt) {
        append_log("lives overlay dependencies", ERROR_PROC_NOT_FOUND);
        return 0;
    }
    if (install_lives_overlay_hook()) {
        append_log("lives draw hook", ERROR_SUCCESS);
    } else {
        append_log("lives draw hook", ERROR_BAD_EXE_FORMAT);
    }
    if (install_lives_scene_switch_hook()) {
        append_log("lives scene switch hook", ERROR_SUCCESS);
    } else {
        append_log("lives scene switch hook", ERROR_BAD_EXE_FORMAT);
    }
    return 0;
}

static void start_lives_timer(HWND window)
{
    HANDLE thread;
    if (lives_timer_started || !window) return;
    lives_main_window = window;
    if (SetTimer(window, LIVES_TIMER_ID, 16, lives_timer_proc))
        lives_timer_started = TRUE;
    if (!lives_hook_thread_started) {
        thread = CreateThread(NULL, 0, install_lives_hook_after_startup,
                NULL, 0, NULL);
        if (thread) {
            CloseHandle(thread);
            lives_hook_thread_started = TRUE;
        } else {
            append_log("lives hook thread", GetLastError());
        }
    }
}

#define BADMOJO_INTRO_PATCH_RVA 0x00058c48
static DWORD hash_intro_bytes(const BYTE *bytes)
{
    DWORD hash = 2166136261u;
    SIZE_T index;
    for (index = 0; index < 20; ++index) {
        hash ^= bytes[index];
        hash *= 16777619u;
    }
    return hash;
}

static BOOL configure_startup_logo_skip(void)
{
    BYTE *image = (BYTE *)GetModuleHandleA(NULL);
    BYTE *target;
    DWORD old_protect, ignored;
    DWORD signature;
    if (!feature_skip_startup_logos) return TRUE;
    if (!image) return FALSE;
    target = image + BADMOJO_INTRO_PATCH_RVA;
    signature = hash_intro_bytes(target);
    if (signature == 0xb294d54e) return TRUE;
    if (signature != 0xbe65f48e) {
        append_log("intro signature mismatch", ERROR_BAD_EXE_FORMAT);
        return FALSE;
    }
    if (!VirtualProtect(target, 20,
            PAGE_EXECUTE_READWRITE, &old_protect)) return FALSE;
    /* Change the installed game's state value, then branch to its existing
       skip path. No original instruction bytes are embedded in this DLL. */
    target[6] = 4;
    target[10] = 0xe8;
    *(DWORD *)(target + 11) = (DWORD)-455;
    target[15] = 0xe9;
    *(DWORD *)(target + 16) = 0x90;
    if (!VirtualProtect(target, 20,
            old_protect, &ignored)) return FALSE;
    FlushInstructionCache(GetCurrentProcess(), target, 20);
    return TRUE;
}

static BOOL is_bad_mojo_dialog(const OPENFILENAMEA *ofn)
{
    return ofn && ofn->lpstrFile && ofn->nMaxFile && ofn->lpstrDefExt
            && !lstrcmpiA(ofn->lpstrDefExt, "bmj");
}

static void copy_dialog_filename(OPENFILENAMEA *ofn, const char *name)
{
    DWORD limit = ofn->nMaxFile;
    if (limit > 0x7fffffff) limit = 0x7fffffff;
    lstrcpynA(ofn->lpstrFile, name, (int)limit);
}

static BOOL save_name_exists(const OPENFILENAMEA *ofn, const char *name)
{
    char directory[MAX_PATH];
    char path[MAX_PATH];
    DWORD length;
    if (ofn->lpstrInitialDir && ofn->lpstrInitialDir[0]) {
        length = lstrlenA(ofn->lpstrInitialDir);
        if (length >= MAX_PATH) return FALSE;
        lstrcpyA(directory, ofn->lpstrInitialDir);
    } else {
        length = GetCurrentDirectoryA(MAX_PATH, directory);
        if (!length || length >= MAX_PATH) return FALSE;
    }
    length = lstrlenA(directory);
    if (length && directory[length - 1] != '\\') {
        if (length + 1 >= MAX_PATH) return FALSE;
        directory[length++] = '\\';
        directory[length] = 0;
    }
    if (length + lstrlenA(name) + sizeof(".bmj") > MAX_PATH) return FALSE;
    lstrcpyA(path, directory);
    lstrcatA(path, name);
    lstrcatA(path, ".bmj");
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void set_save_default(OPENFILENAMEA *ofn)
{
    SYSTEMTIME time;
    char base[32];
    char candidate[40];
    unsigned int suffix;
    GetLocalTime(&time);
    wsprintfA(base, "save-%04u%02u%02u-%02u%02u%02u",
            time.wYear, time.wMonth, time.wDay,
            time.wHour, time.wMinute, time.wSecond);
    lstrcpyA(candidate, base);
    for (suffix = 0; suffix <= 999; ++suffix) {
        if (suffix) wsprintfA(candidate, "%s-%02u", base, suffix);
        if (!save_name_exists(ofn, candidate)) break;
    }
    copy_dialog_filename(ofn, candidate);
}

static BOOL WINAPI replacement_GetSaveFileNameA(LPOPENFILENAMEA ofn)
{
    if (is_bad_mojo_dialog(ofn)) set_save_default(ofn);
    return original_GetSaveFileNameA(ofn);
}

static BOOL WINAPI replacement_GetOpenFileNameA(LPOPENFILENAMEA ofn)
{
    if (is_bad_mojo_dialog(ofn)) ofn->lpstrFile[0] = 0;
    return original_GetOpenFileNameA(ofn);
}

static void capture_preferences_layout(HWND dialog)
{
    HWND child;
    RECT rectangle;
    HFONT font;
    preferences_child_count = 0;
    GetClientRect(dialog, &rectangle);
    preferences_base_client.cx = rectangle.right;
    preferences_base_client.cy = rectangle.bottom;
    GetWindowRect(dialog, &rectangle);
    preferences_min_window.cx = rectangle.right - rectangle.left;
    preferences_min_window.cy = rectangle.bottom - rectangle.top;
    font = (HFONT)SendMessageA(dialog, WM_GETFONT, 0, 0);
    ZeroMemory(&preferences_base_font, sizeof(preferences_base_font));
    if (font) GetObjectA(font, sizeof(preferences_base_font), &preferences_base_font);
    child = GetWindow(dialog, GW_CHILD);
    while (child && preferences_child_count < MAX_PREFERENCES_CHILDREN) {
        preferences_children[preferences_child_count].window = child;
        GetWindowRect(child, &preferences_children[preferences_child_count].rectangle);
        MapWindowPoints(NULL, dialog,
                (POINT *)&preferences_children[preferences_child_count].rectangle, 2);
        ++preferences_child_count;
        child = GetWindow(child, GW_HWNDNEXT);
    }
    preferences_layout_ready = TRUE;
}

static void resize_preferences_layout(HWND dialog)
{
    RECT client;
    unsigned int index;
    int width, height, font_height;
    LONG x, y, cx, cy;
    HFONT font;
    LOGFONTA scaled_font;
    if (!preferences_layout_ready) return;
    GetClientRect(dialog, &client);
    width = client.right;
    height = client.bottom;
    if (!preferences_base_client.cx || !preferences_base_client.cy) return;
    for (index = 0; index < preferences_child_count; ++index) {
        RECT *base = &preferences_children[index].rectangle;
        x = MulDiv(base->left, width, preferences_base_client.cx);
        y = MulDiv(base->top, height, preferences_base_client.cy);
        cx = MulDiv(base->right - base->left, width, preferences_base_client.cx);
        cy = MulDiv(base->bottom - base->top, height, preferences_base_client.cy);
        SetWindowPos(preferences_children[index].window, NULL, x, y, cx, cy,
                SWP_NOACTIVATE | SWP_NOZORDER);
    }
    font_height = MulDiv(preferences_base_font.lfHeight,
            width * preferences_base_client.cy < height * preferences_base_client.cx
                ? width : height,
            width * preferences_base_client.cy < height * preferences_base_client.cx
                ? preferences_base_client.cx : preferences_base_client.cy);
    if (font_height == preferences_scaled_height) return;
    preferences_scaled_height = font_height;
    scaled_font = preferences_base_font;
    scaled_font.lfHeight = font_height;
    font = CreateFontIndirectA(&scaled_font);
    if (!font) return;
    SendMessageA(dialog, WM_SETFONT, (WPARAM)font, TRUE);
    for (index = 0; index < preferences_child_count; ++index)
        SendMessageA(preferences_children[index].window, WM_SETFONT, (WPARAM)font, TRUE);
    if (preferences_scaled_font) DeleteObject(preferences_scaled_font);
    preferences_scaled_font = font;
}

/* The original launcher temporarily switches to a low desktop resolution.
 * A dialog made resizable after creation can then place its caption outside
 * the visible work area.  Fit once on creation instead, preserving the
 * game's normal modal-dialog behavior while scaling every child consistently. */
static void fit_preferences_to_screen(HWND dialog)
{
    RECT outer, client, work;
    int frame_width, frame_height, maximum_width, maximum_height;
    int target_width, target_height, target_x, target_y;
    if (!preferences_layout_ready) return;
    if (!GetWindowRect(dialog, &outer) || !GetClientRect(dialog, &client)) return;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) return;
    frame_width = (outer.right - outer.left) - client.right;
    frame_height = (outer.bottom - outer.top) - client.bottom;
    maximum_width = (work.right - work.left) - 32;
    maximum_height = (work.bottom - work.top) - 48;
    target_width = outer.right - outer.left;
    target_height = outer.bottom - outer.top;
    if (target_width > maximum_width) target_width = maximum_width;
    if (target_height > maximum_height) target_height = maximum_height;
    if (target_width <= frame_width || target_height <= frame_height) return;
    target_x = work.left + ((work.right - work.left) - target_width) / 2;
    target_y = work.top + ((work.bottom - work.top) - target_height) / 2;
    SetWindowPos(dialog, NULL, target_x, target_y, target_width, target_height,
            SWP_NOACTIVATE | SWP_NOZORDER);
}

static LRESULT preferences_resize_hit(HWND dialog, LPARAM lparam)
{
    RECT rectangle;
    int x = (short)LOWORD(lparam);
    int y = (short)HIWORD(lparam);
    const int edge = 10;
    BOOL left, right, top, bottom;
    GetWindowRect(dialog, &rectangle);
    left = x < rectangle.left + edge;
    right = x >= rectangle.right - edge;
    top = y < rectangle.top + edge;
    bottom = y >= rectangle.bottom - edge;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return 0;
}

static INT_PTR CALLBACK preferences_proc(HWND dialog, UINT message,
        WPARAM wparam, LPARAM lparam)
{
    INT_PTR result;
    if (!original_preferences_proc) return FALSE;
    if (message == WM_INITDIALOG) {
        LONG style, exstyle;
        append_log("preferences WM_INITDIALOG", (DWORD)(ULONG_PTR)dialog);
        result = CallWindowProcA((WNDPROC)original_preferences_proc,
                dialog, message, wparam, lparam);
        capture_preferences_layout(dialog);
        style = GetWindowLongA(dialog, GWL_STYLE);
        style = (style & ~WS_DLGFRAME) | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX;
        SetWindowLongA(dialog, GWL_STYLE, style);
        exstyle = GetWindowLongA(dialog, GWL_EXSTYLE);
        SetWindowLongA(dialog, GWL_EXSTYLE, exstyle & ~WS_EX_DLGMODALFRAME);
        SetWindowPos(dialog, NULL, 0, 0, 0, 0,
                SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        fit_preferences_to_screen(dialog);
        return result;
    }
    if (message == WM_NCHITTEST) {
        LRESULT hit = preferences_resize_hit(dialog, lparam);
        if (hit) return hit;
    }
    if (message == WM_GETMINMAXINFO) {
        MINMAXINFO *limits = (MINMAXINFO *)lparam;
        limits->ptMinTrackSize.x = 360;
        limits->ptMinTrackSize.y = 260;
        return TRUE;
    }
    if (message == WM_SIZE && wparam != SIZE_MINIMIZED) {
        append_log("preferences WM_SIZE", (DWORD)lparam);
        resize_preferences_layout(dialog);
        InvalidateRect(dialog, NULL, TRUE);
    }
    result = CallWindowProcA((WNDPROC)original_preferences_proc,
            dialog, message, wparam, lparam);
    if (message == WM_DESTROY) {
        if (preferences_scaled_font) DeleteObject(preferences_scaled_font);
        preferences_scaled_font = NULL;
        preferences_scaled_height = 0;
        preferences_layout_ready = FALSE;
        preferences_child_count = 0;
    }
    return result;
}

static INT_PTR WINAPI replacement_DialogBoxParamA(HINSTANCE instance,
        LPCSTR template_name, HWND parent, DLGPROC dialog_proc, LPARAM init_param)
{
    INT_PTR result;
    DLGPROC previous;
    append_log("preferences DialogBoxParamA", (DWORD)(ULONG_PTR)template_name);
    /* DialogBoxParamA receives IDD_DIALOG5 as MAKEINTRESOURCE(5) in the
     * Redux executable, not as the literal string "IDD_DIALOG5". */
    if ((ULONG_PTR)template_name <= 0xffff) {
        if ((ULONG_PTR)template_name != 5)
            return original_DialogBoxParamA(instance, template_name, parent,
                    dialog_proc, init_param);
    } else if (lstrcmpiA(template_name, "IDD_DIALOG5")) {
        return original_DialogBoxParamA(instance, template_name, parent,
                dialog_proc, init_param);
    }
    previous = original_preferences_proc;
    original_preferences_proc = dialog_proc;
    result = original_DialogBoxParamA(instance, template_name, parent,
            preferences_proc, init_param);
    original_preferences_proc = previous;
    return result;
}

static BOOL patch_main_import(const char *dll_name, const char *function_name,
        void *replacement, void **original)
{
    BYTE *image = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)image;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *descriptor;
    if (!image || dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    nt = (IMAGE_NT_HEADERS *)(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;
    if (!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress)
        return FALSE;
    descriptor = (IMAGE_IMPORT_DESCRIPTOR *)(image
            + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        PIMAGE_THUNK_DATA names;
        PIMAGE_THUNK_DATA addresses;
        if (lstrcmpiA((char *)image + descriptor->Name, dll_name)) continue;
        names = (PIMAGE_THUNK_DATA)(image + (descriptor->OriginalFirstThunk
                ? descriptor->OriginalFirstThunk : descriptor->FirstThunk));
        addresses = (PIMAGE_THUNK_DATA)(image + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            IMAGE_IMPORT_BY_NAME *import_name;
            DWORD old_protect, ignored;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            import_name = (IMAGE_IMPORT_BY_NAME *)(image + names->u1.AddressOfData);
            if (lstrcmpA((char *)import_name->Name, function_name)) continue;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                    PAGE_READWRITE, &old_protect)) return FALSE;
            *original = (void *)(ULONG_PTR)addresses->u1.Function;
            addresses->u1.Function = (ULONG_PTR)replacement;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                    old_protect, &ignored)) return FALSE;
            FlushInstructionCache(GetCurrentProcess(), &addresses->u1.Function,
                    sizeof(addresses->u1.Function));
            return TRUE;
        }
    }
    return FALSE;
}

__declspec(dllexport) DWORD WINAPI InitializeEnhancements(LPVOID unused)
{
    BOOL result;
    (void)unused;
    initialize_paths();
    load_settings();
    append_log("initialization begin", GetCurrentProcessId());
    result = configure_startup_logo_skip();
    append_log("startup logo configuration", result ? ERROR_SUCCESS : GetLastError());
    if (feature_save_dialog_defaults) {
        result = patch_main_import("COMDLG32.dll", "GetSaveFileNameA",
                replacement_GetSaveFileNameA, (void **)&original_GetSaveFileNameA);
        append_log("save dialog hook", result ? ERROR_SUCCESS : GetLastError());
        if (!result) return 0;
        result = patch_main_import("COMDLG32.dll", "GetOpenFileNameA",
                replacement_GetOpenFileNameA, (void **)&original_GetOpenFileNameA);
        append_log("load dialog hook", result ? ERROR_SUCCESS : GetLastError());
        if (!result) return 0;
    }
    if (!install_directdraw_proc_audit()) return 0;
    if (feature_windowed_main_window || feature_lives_overlay) {
        result = patch_main_import("USER32.dll", "CreateWindowExA",
                replacement_CreateWindowExA, (void **)&original_CreateWindowExA);
        append_log("windowed CreateWindowExA hook", result ? ERROR_SUCCESS : GetLastError());
        if (!result) return 0;
    }
    if (feature_windowed_main_window) {
        result = patch_main_import("GDI32.dll", "GetDeviceCaps",
                replacement_GetDeviceCaps, (void **)&original_GetDeviceCaps);
        append_log("windowed GetDeviceCaps hook", result ? ERROR_SUCCESS : GetLastError());
        if (!result) return 0;
        result = patch_main_import("USER32.dll", "SetWindowPos",
                replacement_SetWindowPos, (void **)&original_SetWindowPos);
        append_log("windowed SetWindowPos hook", result ? ERROR_SUCCESS : GetLastError());
        if (!result) return 0;
    }
    append_log("initialization complete", ERROR_SUCCESS);
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
