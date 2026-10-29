/*
 * xrom_usb.c - WinUSB access to the XROM GBA Link Port USB cable.
 * See xrom_usb.h for the hardware notes.
 */
#include "xrom_usb.h"
#include <setupapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include "xrom_firmware.h"

/* {A5DCBF10-6530-11D2-901F-00C04FB951ED} */
static const GUID GUID_USB_DEVICE_IFACE =
    {0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};

static xlog_fn g_log = NULL;
void xrom_set_logger(xlog_fn fn) { g_log = fn; }
#define LOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/* Device discovery                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    WCHAR instance_id[512];
    BOOL  is_winusb;
} found_dev;

/* Find the first present USB device instance with our VID and pid. */
static BOOL find_instance(USHORT pid, found_dev *out)
{
    WCHAR want[64];
    HDEVINFO set;
    SP_DEVINFO_DATA did;
    DWORD i;
    BOOL found = FALSE;

    swprintf(want, 64, L"USB\\VID_%04X&PID_%04X", XROM_VID, pid);

    set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return FALSE;

    did.cbSize = sizeof(did);
    for (i = 0; !found && SetupDiEnumDeviceInfo(set, i, &did); i++) {
        WCHAR hwids[2048];
        DWORD type = 0;
        WCHAR *p;
        memset(hwids, 0, sizeof(hwids));
        if (!SetupDiGetDeviceRegistryPropertyW(set, &did, SPDRP_HARDWAREID, &type,
                                               (BYTE *)hwids, sizeof(hwids) - 4, NULL))
            continue;
        for (p = hwids; *p; p += wcslen(p) + 1) {
            if (_wcsnicmp(p, want, wcslen(want)) == 0 &&
                (p[wcslen(want)] == 0 || p[wcslen(want)] == L'&')) {
                WCHAR svc[128];
                memset(svc, 0, sizeof(svc));
                if (!SetupDiGetDeviceInstanceIdW(set, &did, out->instance_id,
                                                 sizeof(out->instance_id) / sizeof(WCHAR), NULL))
                    break;
                out->is_winusb = FALSE;
                if (SetupDiGetDeviceRegistryPropertyW(set, &did, SPDRP_SERVICE, &type,
                                                      (BYTE *)svc, sizeof(svc) - 2, NULL))
                    out->is_winusb = (_wcsicmp(svc, L"WinUSB") == 0);
                /* Device registry key: interface GUIDs written by Zadig/INF */
                found = TRUE;
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

BOOL xrom_present(USHORT pid)
{
    found_dev fd;
    return find_instance(pid, &fd);
}

/* Get the first interface path of the given class for a device instance. */
static BOOL iface_path(const GUID *g, const WCHAR *instance, WCHAR *path, DWORD cch)
{
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA ifd;
    BOOL ok = FALSE;
    set = SetupDiGetClassDevsW(g, instance, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return FALSE;
    ifd.cbSize = sizeof(ifd);
    if (SetupDiEnumDeviceInterfaces(set, NULL, g, 0, &ifd)) {
        BYTE buf[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 1024 * sizeof(WCHAR)];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buf;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof(buf), NULL, NULL)) {
            wcsncpy(path, det->DevicePath, cch - 1);
            path[cch - 1] = 0;
            ok = TRUE;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

/* Read DeviceInterfaceGUIDs / DeviceInterfaceGUID from the device key. */
static int read_iface_guids(const WCHAR *instance, GUID *guids, int max)
{
    HDEVINFO set;
    SP_DEVINFO_DATA did;
    HKEY key;
    int n = 0;

    set = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (set == INVALID_HANDLE_VALUE) return 0;
    did.cbSize = sizeof(did);
    if (SetupDiOpenDeviceInfoW(set, instance, NULL, 0, &did)) {
        key = SetupDiOpenDevRegKey(set, &did, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            WCHAR buf[1024];
            DWORD type, sz;
            const WCHAR *names[2] = {L"DeviceInterfaceGUIDs", L"DeviceInterfaceGUID"};
            int k;
            for (k = 0; k < 2 && n == 0; k++) {
                WCHAR *p;
                memset(buf, 0, sizeof(buf));
                sz = sizeof(buf) - 4;
                if (RegQueryValueExW(key, names[k], NULL, &type, (BYTE *)buf, &sz) != ERROR_SUCCESS)
                    continue;
                for (p = buf; *p && n < max; p += wcslen(p) + 1) {
                    if (CLSIDFromString(p, &guids[n]) == NOERROR) n++;
                    if (type != REG_MULTI_SZ) break;
                }
            }
            RegCloseKey(key);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

static BOOL open_path(const WCHAR *path, xrom_dev *dev)
{
    USB_INTERFACE_DESCRIPTOR ifd;
    WINUSB_PIPE_INFORMATION pi;
    UCHAR i;
    ULONG got;

    dev->file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (dev->file == INVALID_HANDLE_VALUE) {
        LOG("  CreateFile failed (%lu) on %ls", GetLastError(), path);
        return FALSE;
    }
    if (!WinUsb_Initialize(dev->file, &dev->wusb)) {
        LOG("  WinUsb_Initialize failed (%lu) on %ls", GetLastError(), path);
        CloseHandle(dev->file);
        dev->file = INVALID_HANDLE_VALUE;
        return FALSE;
    }
    memset(&dev->devdesc, 0, sizeof(dev->devdesc));
    WinUsb_GetDescriptor(dev->wusb, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                         (PUCHAR)&dev->devdesc, sizeof(dev->devdesc), &got);

    dev->ep_in = dev->ep_out = 0;
    dev->in_max_packet = 64;
    if (WinUsb_QueryInterfaceSettings(dev->wusb, 0, &ifd)) {
        for (i = 0; i < ifd.bNumEndpoints; i++) {
            if (!WinUsb_QueryPipe(dev->wusb, 0, i, &pi)) continue;
            if (pi.PipeType != UsbdPipeTypeBulk) continue;
            if (USB_ENDPOINT_DIRECTION_IN(pi.PipeId)) {
                if (!dev->ep_in) { dev->ep_in = pi.PipeId; dev->in_max_packet = pi.MaximumPacketSize; }
            } else if (!dev->ep_out) {
                dev->ep_out = pi.PipeId;
            }
        }
    }
    return TRUE;
}

xdev_state xrom_open(USHORT pid, xrom_dev *dev)
{
    found_dev fd;
    GUID guids[8];
    WCHAR path[1024];
    int n, i;

    dev->file = INVALID_HANDLE_VALUE;
    dev->wusb = NULL;

    if (!find_instance(pid, &fd)) return XDEV_NOT_FOUND;
    LOG("Found %04X:%04X instance %ls (service is %sWinUSB)", XROM_VID, pid,
        fd.instance_id, fd.is_winusb ? "" : "NOT ");
    if (!fd.is_winusb) return XDEV_NO_WINUSB;

    /* Preferred: the interface GUID the WinUSB INF (Zadig) registered. */
    n = read_iface_guids(fd.instance_id, guids, 8);
    for (i = 0; i < n; i++) {
        if (iface_path(&guids[i], fd.instance_id, path, 1024) && open_path(path, dev))
            return XDEV_OK;
    }
    /* Fallback: the generic USB device interface (top of the stack = WinUSB). */
    if (iface_path(&GUID_USB_DEVICE_IFACE, fd.instance_id, path, 1024) && open_path(path, dev))
        return XDEV_OK;

    LOG("  Could not open %04X:%04X through WinUSB", XROM_VID, pid);
    return XDEV_NO_WINUSB;
}

void xrom_close(xrom_dev *dev)
{
    if (dev->wusb) { WinUsb_Free(dev->wusb); dev->wusb = NULL; }
    if (dev->file != INVALID_HANDLE_VALUE && dev->file != NULL) {
        CloseHandle(dev->file);
    }
    dev->file = INVALID_HANDLE_VALUE;
}

/* ------------------------------------------------------------------ */
/* Firmware download (EZ-USB AN21xx "Anchor Load", vendor request A0) */
/* ------------------------------------------------------------------ */

#define AN21XX_CPUCS 0x7F92

static BOOL anchor_write(xrom_dev *dev, USHORT addr, const UCHAR *data, USHORT len)
{
    WINUSB_SETUP_PACKET sp;
    ULONG done = 0;
    UCHAR tmp[64];
    /* WinUSB probes the buffer for write access even on OUT transfers, so a
     * pointer into read-only data (the firmware table) fails with error 998.
     * Always send from a writable copy.                                     */
    if (len > sizeof(tmp)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memcpy(tmp, data, len);
    sp.RequestType = 0x40;  /* vendor, host-to-device, device */
    sp.Request = 0xA0;
    sp.Value = addr;
    sp.Index = 0;
    sp.Length = len;
    return WinUsb_ControlTransfer(dev->wusb, sp, tmp, len, &done, NULL) && done == len;
}

BOOL xrom_load_firmware(xrom_dev *dev)
{
    UCHAR one = 1, zero = 0;
    int i;
    ULONG to = 2000;
    WinUsb_SetPipePolicy(dev->wusb, 0, PIPE_TRANSFER_TIMEOUT, sizeof(to), &to);

    LOG("Loading X-ROM firmware (%d records)...", XROM_FIRMWARE_RECORDS);
    if (!anchor_write(dev, AN21XX_CPUCS, &one, 1)) {
        LOG("  could not halt 8051 (err %lu)", GetLastError());
        return FALSE;
    }
    for (i = 0; i < XROM_FIRMWARE_RECORDS; i++) {
        const fw_rec *r = &xrom_firmware[i];
        if (!anchor_write(dev, r->addr, r->data, r->len)) {
            LOG("  firmware write failed at 0x%04X (err %lu)", r->addr, GetLastError());
            return FALSE;
        }
    }
    /* Release the 8051. The chip disconnects and re-enumerates as 4542:4144,
     * so an error on this last request is normal.                           */
    anchor_write(dev, AN21XX_CPUCS, &zero, 1);
    LOG("  firmware loaded, waiting for re-enumeration");
    return TRUE;
}

xdev_state xrom_ensure_running(DWORD wait_ms, xdev_state *loader_state)
{
    xrom_dev d;
    xdev_state st, ls;
    DWORD start;

    if (loader_state) *loader_state = XDEV_NOT_FOUND;

    st = xrom_open(XROM_PID_RUNNING, &d);
    if (st == XDEV_OK) { xrom_close(&d); return XDEV_OK; }
    if (st == XDEV_NO_WINUSB) return st;

    ls = xrom_open(XROM_PID_LOADER, &d);
    if (loader_state) *loader_state = ls;
    if (ls != XDEV_OK) return XDEV_NOT_FOUND;
    if (!xrom_load_firmware(&d)) { xrom_close(&d); return XDEV_FW_FAILED; }
    xrom_close(&d);

    start = GetTickCount();
    st = XDEV_NOT_FOUND;
    while (GetTickCount() - start < wait_ms) {
        Sleep(300);
        st = xrom_open(XROM_PID_RUNNING, &d);
        if (st == XDEV_OK) {
            xrom_close(&d);
            LOG("  running device is ready after %lu ms", GetTickCount() - start);
            return XDEV_OK;
        }
    }
    return st;
}

/* ------------------------------------------------------------------ */
/* Bulk I/O                                                            */
/* ------------------------------------------------------------------ */

static BOOL pipe_xfer(xrom_dev *dev, BOOL in, UCHAR ep, UCHAR *buf, DWORD len,
                      DWORD *got, DWORD timeout_ms, BOOL *timed_out)
{
    OVERLAPPED ov;
    BOOL ok;
    DWORD w, err;
    ULONG n = 0;

    *got = 0;
    *timed_out = FALSE;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return FALSE;

    ok = in ? WinUsb_ReadPipe(dev->wusb, ep, buf, len, NULL, &ov)
            : WinUsb_WritePipe(dev->wusb, ep, buf, len, NULL, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        err = GetLastError();
        CloseHandle(ov.hEvent);
        SetLastError(err);
        return FALSE;
    }
    w = WaitForSingleObject(ov.hEvent, timeout_ms ? timeout_ms : INFINITE);
    if (w == WAIT_TIMEOUT) {
        WinUsb_AbortPipe(dev->wusb, ep);
        WinUsb_GetOverlappedResult(dev->wusb, &ov, &n, TRUE);
        *got = n;
        *timed_out = TRUE;
        CloseHandle(ov.hEvent);
        SetLastError(ERROR_SEM_TIMEOUT);
        return FALSE;
    }
    ok = WinUsb_GetOverlappedResult(dev->wusb, &ov, &n, FALSE);
    err = GetLastError();
    *got = n;
    CloseHandle(ov.hEvent);
    if (!ok) {
        WinUsb_ResetPipe(dev->wusb, ep);
        SetLastError(err);
    }
    return ok;
}

BOOL xrom_write(xrom_dev *dev, const void *buf, DWORD len, DWORD *done, DWORD timeout_ms)
{
    BOOL to;
    if (!dev->ep_out) { SetLastError(ERROR_INVALID_HANDLE); *done = 0; return FALSE; }
    if (pipe_xfer(dev, FALSE, dev->ep_out, (UCHAR *)buf, len, done, timeout_ms, &to))
        return TRUE;
    if (GetLastError() == ERROR_NOACCESS && len) {
        UCHAR *copy = (UCHAR *)HeapAlloc(GetProcessHeap(), 0, len);
        BOOL ok;
        if (!copy) return FALSE;
        memcpy(copy, buf, len);
        ok = pipe_xfer(dev, FALSE, dev->ep_out, copy, len, done, timeout_ms, &to);
        HeapFree(GetProcessHeap(), 0, copy);
        return ok;
    }
    return FALSE;
}

/* XROMIO.SYS (Walter Oney / John Hyde BlockIO) splits a read into stages and
 * keeps going until the whole request is satisfied. WinUSB stops a read at
 * the first short packet, so keep reading until we have everything.        */
BOOL xrom_read(xrom_dev *dev, void *buf, DWORD len, DWORD *done, DWORD timeout_ms)
{
    UCHAR *p = (UCHAR *)buf;
    DWORD total = 0, got;
    BOOL to, ok = TRUE;
    DWORD follow_ms = timeout_ms ? timeout_ms : 10000;

    *done = 0;
    if (!dev->ep_in) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (len == 0) return TRUE;

    ok = pipe_xfer(dev, TRUE, dev->ep_in, p, len, &got, timeout_ms, &to);
    total = got;
    while (ok && total < len && got > 0) {
        ok = pipe_xfer(dev, TRUE, dev->ep_in, p + total, len - total, &got, follow_ms, &to);
        total += got;
        if (!ok && to && total > 0) {
            /* device simply had nothing more to say: return what we have */
            LOG("  read: short (%lu of %lu bytes)", total, len);
            ok = TRUE;
            break;
        }
    }
    *done = total;
    return ok;
}
