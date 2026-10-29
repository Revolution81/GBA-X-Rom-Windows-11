/*
 * XRomShim - drop-in DlPortIO.dll for LittleWriter 1.0.8.4 that lets the
 * XROM GBA Link Port USB cable work on 64-bit Windows 10/11 through WinUSB.
 *
 * LittlewriterDll.dll statically imports DlPortIO.dll (it was used for old
 * parallel-port writers). Windows loads it from the program folder, so this
 * DLL gets loaded before LittleWriter runs. In DllMain we patch
 * LittlewriterDll.dll's import table so that its calls to
 *
 *   SetupDiGetClassDevs / SetupDiEnumDeviceInterfaces /
 *   SetupDiGetDeviceInterfaceDetail / SetupDiDestroyDeviceInfoList /
 *   CreateFile / ReadFile / WriteFile / DeviceIoControl / CloseHandle
 *
 * for the XROMIO.SYS ("BlockIO") device interface are serviced by WinUSB.
 * Every other call (files, other writers) goes straight to Windows.
 *
 * The DlPortIO port functions are stubs: no parallel port support.
 */
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "xrom_usb.h"

/* BlockIO interface GUID used by XROMIO.SYS: {606377C1-2270-11D4-BFD8-00207812F5D5} */
static const GUID GUID_XROM_BLOCKIO =
    {0x606377C1, 0x2270, 0x11D4, {0xBF, 0xD8, 0x00, 0x20, 0x78, 0x12, 0xF5, 0xD5}};

#define FAKE_PATH_A  "\\\\.\\XRomShim-WinUSB-0"
#define FAKE_PATH_W L"\\\\.\\XRomShim-WinUSB-0"
#define IOCTL_BLOCKIO_GET_DEVICE_DESCRIPTOR 0x222000

/* ---------------- configuration & logging ---------------- */

static HMODULE g_self;
static char g_dir[MAX_PATH];
static int g_log_level = 1;          /* 0 off, 1 normal, 2 dump packets */
static DWORD g_read_timeout = 30000; /* ms, 0 = forever */
static DWORD g_write_timeout = 30000;
static DWORD g_fw_wait = 10000;
static int g_show_errors = 1;
static CRITICAL_SECTION g_lock;

static void shim_log(const char *fmt, ...)
{
    char path[MAX_PATH], line[2048];
    va_list ap;
    FILE *f;
    SYSTEMTIME t;
    if (g_log_level <= 0) return;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, ap);
    line[sizeof(line) - 1] = 0;
    va_end(ap);
    _snprintf(path, MAX_PATH, "%sXRomShim.log", g_dir);
    f = fopen(path, "a");
    if (!f) return;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d [%5lu] %s\n", t.wHour, t.wMinute, t.wSecond,
            t.wMilliseconds, GetCurrentThreadId(), line);
    fclose(f);
}

static void dump(const char *tag, const void *buf, DWORD n)
{
    char s[3 * 32 + 1];
    DWORD i, k = 0;
    if (g_log_level < 2) return;
    for (i = 0; i < n && i < 32; i++) k += sprintf(s + k, "%02X ", ((const UCHAR *)buf)[i]);
    s[k] = 0;
    shim_log("  %s %lu bytes: %s%s", tag, n, s, n > 32 ? "..." : "");
}

static void load_config(void)
{
    char ini[MAX_PATH];
    char *slash;
    GetModuleFileNameA(g_self, g_dir, MAX_PATH);
    slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = 0; else g_dir[0] = 0;
    _snprintf(ini, MAX_PATH, "%sXRomShim.ini", g_dir);
    g_log_level     = GetPrivateProfileIntA("XRomShim", "Log", 1, ini);
    g_read_timeout  = GetPrivateProfileIntA("XRomShim", "ReadTimeoutMs", 30000, ini);
    g_write_timeout = GetPrivateProfileIntA("XRomShim", "WriteTimeoutMs", 30000, ini);
    g_fw_wait       = GetPrivateProfileIntA("XRomShim", "FirmwareWaitMs", 10000, ini);
    g_show_errors   = GetPrivateProfileIntA("XRomShim", "ShowErrors", 1, ini);
}

/* ---------------- the WinUSB session ---------------- */

static xrom_dev g_dev = {INVALID_HANDLE_VALUE, NULL, 0, 0, 64};
static int g_dev_refs = 0;

#define MAX_HANDLES 16
static HANDLE g_handles[MAX_HANDLES];   /* fake handles = real event objects */

static BOOL is_fake(HANDLE h)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    for (i = 0; i < MAX_HANDLES; i++)
        if (g_handles[i] == h) return TRUE;
    return FALSE;
}

static void error_box(const char *msg)
{
    static int shown = 0;
    if (!g_show_errors || shown) return;
    shown = 1;
    MessageBoxA(NULL, msg, "XRomShim (X-ROM USB on WinUSB)", MB_OK | MB_ICONWARNING | MB_TOPMOST);
}

/* Called when LittleWriter enumerates the BlockIO interface. Returns TRUE if
 * a running cable is available (loading firmware if needed).               */
static BOOL g_ready = FALSE;

static BOOL prepare_device(void)
{
    xdev_state st, ls;
    if (g_dev_refs > 0) return TRUE;  /* already open */
    shim_log("LittleWriter is looking for the XROM cable");
    st = xrom_ensure_running(g_fw_wait, &ls);
    if (st == XDEV_OK) return TRUE;
    if (st == XDEV_NO_WINUSB || ls == XDEV_NO_WINUSB) {
        shim_log("Cable found but it is not using the WinUSB driver");
        error_box("The X-ROM cable was found, but Windows is not using the WinUSB driver for it.\n\n"
                  "Run Zadig, choose Options > List All Devices, pick the device with USB ID\n"
                  "4542 4149 (before firmware) or 4542 4144 (after firmware) and install WinUSB.\n"
                  "Both IDs need WinUSB. See README.txt.");
    } else if (st == XDEV_FW_FAILED) {
        shim_log("Firmware download to the cable failed");
        error_box("The X-ROM cable rejected its firmware download.\n\n"
                  "Unplug the cable, plug it back in and try again.\n"
                  "Details are in XRomShim.log.");
    } else if (ls == XDEV_OK) {
        shim_log("Firmware was sent but the cable did not come back as 4542:4144");
        error_box("Firmware was loaded into the X-ROM cable, but the cable did not come back\n"
                  "as a WinUSB device (USB ID 4542 4144).\n\n"
                  "If this is the first time, open Zadig now, choose Options > List All Devices,\n"
                  "select USB ID 4542 4144, install WinUSB, then try again in LittleWriter.");
    } else {
        shim_log("No XROM cable found (4542:4149 / 4542:4144)");
    }
    return FALSE;
}

static HANDLE open_session(void)
{
    HANDLE h;
    int i;
    EnterCriticalSection(&g_lock);
    if (g_dev_refs == 0) {
        if (xrom_open(XROM_PID_RUNNING, &g_dev) != XDEV_OK) {
            LeaveCriticalSection(&g_lock);
            shim_log("CreateFile: could not open the cable through WinUSB");
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        shim_log("Opened cable: bulk OUT 0x%02X, bulk IN 0x%02X (max packet %u)",
                 g_dev.ep_out, g_dev.ep_in, g_dev.in_max_packet);
    }
    for (i = 0; i < MAX_HANDLES && g_handles[i]; i++) {}
    if (i == MAX_HANDLES) {
        LeaveCriticalSection(&g_lock);
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return INVALID_HANDLE_VALUE;
    }
    h = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!h) {
        if (g_dev_refs == 0) xrom_close(&g_dev);
        LeaveCriticalSection(&g_lock);
        return INVALID_HANDLE_VALUE;
    }
    g_handles[i] = h;
    g_dev_refs++;
    LeaveCriticalSection(&g_lock);
    return h;
}

/* ---------------- original function pointers ---------------- */

typedef HDEVINFO (WINAPI *pGetClassDevsA)(const GUID *, PCSTR, HWND, DWORD);
typedef HDEVINFO (WINAPI *pGetClassDevsW)(const GUID *, PCWSTR, HWND, DWORD);
typedef BOOL (WINAPI *pEnumIfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL (WINAPI *pIfaceDetailA)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *pIfaceDetailW)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *pDestroyList)(HDEVINFO);
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *pCreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *pReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *pWriteFile)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *pDeviceIoControl)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *pCloseHandle)(HANDLE);

static pGetClassDevsA   o_GetClassDevsA;
static pGetClassDevsW   o_GetClassDevsW;
static pEnumIfaces      o_EnumIfaces;
static pIfaceDetailA    o_IfaceDetailA;
static pIfaceDetailW    o_IfaceDetailW;
static pDestroyList     o_DestroyList;
static pCreateFileA     o_CreateFileA;
static pCreateFileW     o_CreateFileW;
static pReadFile        o_ReadFile;
static pWriteFile       o_WriteFile;
static pDeviceIoControl o_DeviceIoControl;
static pCloseHandle     o_CloseHandle;

/* A recognisable, never-valid HDEVINFO for our fake device list. */
static int g_fake_list_marker;
#define FAKE_DEVINFO ((HDEVINFO)&g_fake_list_marker)

/* ---------------- hooks ---------------- */

static HDEVINFO WINAPI h_GetClassDevsA(const GUID *g, PCSTR e, HWND w, DWORD f)
{
    if (g && (f & DIGCF_DEVICEINTERFACE) && IsEqualGUID(g, &GUID_XROM_BLOCKIO)) {
        g_ready = prepare_device();
        return FAKE_DEVINFO;
    }
    return o_GetClassDevsA(g, e, w, f);
}

static HDEVINFO WINAPI h_GetClassDevsW(const GUID *g, PCWSTR e, HWND w, DWORD f)
{
    if (g && (f & DIGCF_DEVICEINTERFACE) && IsEqualGUID(g, &GUID_XROM_BLOCKIO)) {
        g_ready = prepare_device();
        return FAKE_DEVINFO;
    }
    return o_GetClassDevsW(g, e, w, f);
}

static BOOL WINAPI h_EnumIfaces(HDEVINFO s, PSP_DEVINFO_DATA d, const GUID *g, DWORD idx,
                                PSP_DEVICE_INTERFACE_DATA out)
{
    if (s != FAKE_DEVINFO) return o_EnumIfaces(s, d, g, idx, out);
    if (!g_ready || idx != 0) { SetLastError(ERROR_NO_MORE_ITEMS); return FALSE; }
    if (!out || out->cbSize != sizeof(SP_DEVICE_INTERFACE_DATA)) {
        SetLastError(ERROR_INVALID_USER_BUFFER);
        return FALSE;
    }
    out->InterfaceClassGuid = GUID_XROM_BLOCKIO;
    out->Flags = SPINT_ACTIVE;
    out->Reserved = 0;
    return TRUE;
}

static BOOL WINAPI h_IfaceDetailA(HDEVINFO s, PSP_DEVICE_INTERFACE_DATA i,
                                  PSP_DEVICE_INTERFACE_DETAIL_DATA_A det, DWORD sz,
                                  PDWORD req, PSP_DEVINFO_DATA dd)
{
    DWORD need = (DWORD)(FIELD_OFFSET(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) + sizeof(FAKE_PATH_A));
    if (s != FAKE_DEVINFO) return o_IfaceDetailA(s, i, det, sz, req, dd);
    if (req) *req = need;
    if (!det || sz < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(det->DevicePath, FAKE_PATH_A, sizeof(FAKE_PATH_A));
    return TRUE;
}

static BOOL WINAPI h_IfaceDetailW(HDEVINFO s, PSP_DEVICE_INTERFACE_DATA i,
                                  PSP_DEVICE_INTERFACE_DETAIL_DATA_W det, DWORD sz,
                                  PDWORD req, PSP_DEVINFO_DATA dd)
{
    DWORD need = (DWORD)(FIELD_OFFSET(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath) + sizeof(FAKE_PATH_W));
    if (s != FAKE_DEVINFO) return o_IfaceDetailW(s, i, det, sz, req, dd);
    if (req) *req = need;
    if (!det || sz < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(det->DevicePath, FAKE_PATH_W, sizeof(FAKE_PATH_W));
    return TRUE;
}

static BOOL WINAPI h_DestroyList(HDEVINFO s)
{
    if (s == FAKE_DEVINFO) return TRUE;
    return o_DestroyList(s);
}

static HANDLE WINAPI h_CreateFileA(LPCSTR n, DWORD a, DWORD sm, LPSECURITY_ATTRIBUTES sa,
                                   DWORD cd, DWORD fl, HANDLE t)
{
    if (n && _stricmp(n, FAKE_PATH_A) == 0) {
        HANDLE h = open_session();
        shim_log("CreateFile(cable) -> %p", h);
        return h;
    }
    return o_CreateFileA(n, a, sm, sa, cd, fl, t);
}

static HANDLE WINAPI h_CreateFileW(LPCWSTR n, DWORD a, DWORD sm, LPSECURITY_ATTRIBUTES sa,
                                   DWORD cd, DWORD fl, HANDLE t)
{
    if (n && _wcsicmp(n, FAKE_PATH_W) == 0) {
        HANDLE h = open_session();
        shim_log("CreateFile(cable) -> %p", h);
        return h;
    }
    return o_CreateFileW(n, a, sm, sa, cd, fl, t);
}

static BOOL WINAPI h_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD done, LPOVERLAPPED ov)
{
    DWORD d = 0;
    BOOL ok;
    if (!is_fake(h)) return o_WriteFile(h, buf, n, done, ov);
    ok = xrom_write(&g_dev, buf, n, &d, g_write_timeout);
    dump(ok ? "OUT" : "OUT(fail)", buf, n);
    if (!ok) shim_log("WriteFile(cable, %lu) failed, err %lu, wrote %lu", n, GetLastError(), d);
    if (done) *done = d;
    if (ov) { ov->Internal = ok ? 0 : (ULONG_PTR)0xC0000001L; ov->InternalHigh = d; if (ov->hEvent) SetEvent(ov->hEvent); }
    return ok;
}

static BOOL WINAPI h_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD done, LPOVERLAPPED ov)
{
    DWORD d = 0;
    BOOL ok;
    if (!is_fake(h)) return o_ReadFile(h, buf, n, done, ov);
    ok = xrom_read(&g_dev, buf, n, &d, g_read_timeout);
    dump(ok ? "IN " : "IN(fail)", buf, d);
    if (!ok) shim_log("ReadFile(cable, %lu) failed, err %lu, read %lu", n, GetLastError(), d);
    if (done) *done = d;
    if (ov) { ov->Internal = ok ? 0 : (ULONG_PTR)0xC0000001L; ov->InternalHigh = d; if (ov->hEvent) SetEvent(ov->hEvent); }
    return ok;
}

static BOOL WINAPI h_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD inn, LPVOID out,
                                     DWORD outn, LPDWORD ret, LPOVERLAPPED ov)
{
    if (!is_fake(h)) return o_DeviceIoControl(h, code, in, inn, out, outn, ret, ov);
    shim_log("DeviceIoControl(cable, 0x%08lX, in %lu, out %lu)", code, inn, outn);
    if (code == IOCTL_BLOCKIO_GET_DEVICE_DESCRIPTOR) {
        if (!out || outn < sizeof(USB_DEVICE_DESCRIPTOR)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        memcpy(out, &g_dev.devdesc, sizeof(USB_DEVICE_DESCRIPTOR));
        if (ret) *ret = sizeof(USB_DEVICE_DESCRIPTOR);
        return TRUE;
    }
    /* XROMIO.SYS passed every other IOCTL down to the USB stack, which
     * rejected it. Report the same thing.                                */
    if (ret) *ret = 0;
    SetLastError(ERROR_INVALID_FUNCTION);
    return FALSE;
}

static BOOL WINAPI h_CloseHandle(HANDLE h)
{
    int i;
    if (h && h != INVALID_HANDLE_VALUE) {
        EnterCriticalSection(&g_lock);
        for (i = 0; i < MAX_HANDLES; i++) {
            if (g_handles[i] == h) {
                g_handles[i] = NULL;
                if (--g_dev_refs == 0) { xrom_close(&g_dev); shim_log("Cable closed"); }
                break;
            }
        }
        LeaveCriticalSection(&g_lock);
    }
    return o_CloseHandle(h);
}

/* ---------------- import table patching ---------------- */

typedef struct { const char *name; void *hook; void **orig; } hook_def;

static hook_def g_hooks[] = {
    {"SetupDiGetClassDevsA",             (void *)h_GetClassDevsA,  (void **)&o_GetClassDevsA},
    {"SetupDiGetClassDevsW",             (void *)h_GetClassDevsW,  (void **)&o_GetClassDevsW},
    {"SetupDiEnumDeviceInterfaces",      (void *)h_EnumIfaces,     (void **)&o_EnumIfaces},
    {"SetupDiGetDeviceInterfaceDetailA", (void *)h_IfaceDetailA,   (void **)&o_IfaceDetailA},
    {"SetupDiGetDeviceInterfaceDetailW", (void *)h_IfaceDetailW,   (void **)&o_IfaceDetailW},
    {"SetupDiDestroyDeviceInfoList",     (void *)h_DestroyList,    (void **)&o_DestroyList},
    {"CreateFileA",                      (void *)h_CreateFileA,    (void **)&o_CreateFileA},
    {"CreateFileW",                      (void *)h_CreateFileW,    (void **)&o_CreateFileW},
    {"ReadFile",                         (void *)h_ReadFile,       (void **)&o_ReadFile},
    {"WriteFile",                        (void *)h_WriteFile,      (void **)&o_WriteFile},
    {"DeviceIoControl",                  (void *)h_DeviceIoControl,(void **)&o_DeviceIoControl},
    {"CloseHandle",                      (void *)h_CloseHandle,    (void **)&o_CloseHandle},
};
#define NHOOKS (sizeof(g_hooks) / sizeof(g_hooks[0]))

static void resolve_originals(void)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    /* setupapi.dll is a static import of this DLL, so it is already loaded. */
    HMODULE sapi = GetModuleHandleA("setupapi.dll");
    size_t i;
    for (i = 0; i < NHOOKS; i++) {
        HMODULE m = (strncmp(g_hooks[i].name, "SetupDi", 7) == 0) ? sapi : k32;
        *g_hooks[i].orig = (void *)GetProcAddress(m, g_hooks[i].name);
    }
}

static int patch_module(HMODULE mod)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int patched = 0;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return 0;

    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA *names, *iat;
        if (!imp->FirstThunk) continue;
        iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        names = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk) : NULL;
        for (; iat->u1.Function; iat++, names = names ? names + 1 : NULL) {
            size_t k;
            for (k = 0; k < NHOOKS; k++) {
                BOOL match = FALSE;
                if (names && !IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                    IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
                    match = strcmp((const char *)ibn->Name, g_hooks[k].name) == 0;
                } else if (!names) {
                    match = (void *)iat->u1.Function == *g_hooks[k].orig;
                }
                if (match) {
                    DWORD old;
                    if (!*g_hooks[k].orig) *g_hooks[k].orig = (void *)iat->u1.Function;
                    if (VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                        iat->u1.Function = (ULONG_PTR)g_hooks[k].hook;
                        VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
                        patched++;
                    }
                    break;
                }
            }
        }
    }
    return patched;
}

/* Safety net: patch again once the loader is done (a new thread only starts
 * running after the loader lock is released). Patching is idempotent.     */
static DWORD WINAPI repatch_thread(LPVOID p)
{
    HMODULE lw = GetModuleHandleA("LittlewriterDll.dll");
    (void)p;
    if (lw) {
        int n = patch_module(lw);
        if (g_log_level >= 2) shim_log("re-check: %d imports redirected", n);
    }
    return 0;
}

static void install_hooks(void)
{
    HMODULE lw = GetModuleHandleA("LittlewriterDll.dll");
    HANDLE t;
    int n;
    resolve_originals();
    if (!lw) {
        shim_log("LittlewriterDll.dll is not loaded in this process; nothing hooked");
        return;
    }
    n = patch_module(lw);
    shim_log("XRomShim 1.0 loaded, %d imports redirected in LittlewriterDll.dll", n);
    t = CreateThread(NULL, 0, repatch_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        load_config();
        xrom_set_logger(shim_log);
        install_hooks();
    }
    return TRUE;
}

/* ---------------- DlPortIO API (stubs) ---------------- */

UCHAR  WINAPI DlPortReadPortUchar(ULONG port)  { (void)port; return 0xFF; }
USHORT WINAPI DlPortReadPortUshort(ULONG port) { (void)port; return 0xFFFF; }
ULONG  WINAPI DlPortReadPortUlong(ULONG port)  { (void)port; return 0xFFFFFFFF; }
void WINAPI DlPortWritePortUchar(ULONG port, UCHAR v)   { (void)port; (void)v; }
void WINAPI DlPortWritePortUshort(ULONG port, USHORT v) { (void)port; (void)v; }
void WINAPI DlPortWritePortUlong(ULONG port, ULONG v)   { (void)port; (void)v; }
void WINAPI DlPortReadPortBufferUchar(ULONG p, PUCHAR b, ULONG n)   { (void)p; memset(b, 0xFF, n); }
void WINAPI DlPortReadPortBufferUshort(ULONG p, PUSHORT b, ULONG n) { (void)p; memset(b, 0xFF, n * 2); }
void WINAPI DlPortReadPortBufferUlong(ULONG p, PULONG b, ULONG n)   { (void)p; memset(b, 0xFF, n * 4); }
void WINAPI DlPortWritePortBufferUchar(ULONG p, PUCHAR b, ULONG n)   { (void)p; (void)b; (void)n; }
void WINAPI DlPortWritePortBufferUshort(ULONG p, PUSHORT b, ULONG n) { (void)p; (void)b; (void)n; }
void WINAPI DlPortWritePortBufferUlong(ULONG p, PULONG b, ULONG n)   { (void)p; (void)b; (void)n; }
