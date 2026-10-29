/*
 * xrom_usb.h - WinUSB access to the XROM GBA Link Port USB cable.
 *
 * The cable is a Cypress EZ-USB (AN21xx) chip:
 *   4542:4149  blank, waiting for firmware (handled by XROMLP.SYS in 2003)
 *   4542:4144  running X-ROM firmware, 1 bulk OUT + 1 bulk IN endpoint
 *              (handled by XROMIO.SYS = John Hyde's "BlockIO" sample driver,
 *               ReadFile -> bulk IN, WriteFile -> bulk OUT)
 *
 * Both IDs must be bound to Microsoft's WinUSB driver (e.g. with Zadig).
 */
#ifndef XROM_USB_H
#define XROM_USB_H

#include <windows.h>
#include <winusb.h>

#define XROM_VID          0x4542
#define XROM_PID_LOADER   0x4149
#define XROM_PID_RUNNING  0x4144

typedef enum {
    XDEV_NOT_FOUND = 0,   /* nothing plugged in with that VID/PID */
    XDEV_NO_WINUSB,       /* present, but not bound to WinUSB     */
    XDEV_OK,
    XDEV_FW_FAILED        /* firmware download was rejected        */
} xdev_state;

typedef struct {
    HANDLE file;
    WINUSB_INTERFACE_HANDLE wusb;
    UCHAR ep_in, ep_out;
    USHORT in_max_packet;
    USB_DEVICE_DESCRIPTOR devdesc;
} xrom_dev;

typedef void (*xlog_fn)(const char *fmt, ...);
void xrom_set_logger(xlog_fn fn);

/* Locate & open a device with the given PID (VID is fixed). */
xdev_state xrom_open(USHORT pid, xrom_dev *dev);
void       xrom_close(xrom_dev *dev);

/* Is a device with that PID present at all (any driver)? */
BOOL xrom_present(USHORT pid);

/* Download the built-in firmware to an open 4542:4149 device. */
BOOL xrom_load_firmware(xrom_dev *dev);

/* Make sure a running (4542:4144) device is available, loading firmware
 * if only the blank (4542:4149) device is found. wait_ms = how long to
 * wait for the re-enumeration. Returns state of the running device.     */
xdev_state xrom_ensure_running(DWORD wait_ms, xdev_state *loader_state);

/* Bulk transfers, BlockIO semantics. timeout_ms 0 = wait forever.        */
BOOL xrom_write(xrom_dev *dev, const void *buf, DWORD len, DWORD *done, DWORD timeout_ms);
BOOL xrom_read (xrom_dev *dev, void *buf, DWORD len, DWORD *done, DWORD timeout_ms);

#endif
