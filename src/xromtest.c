/*
 * xromtest - check the XROM GBA Link Port USB cable under WinUSB.
 *
 *   xromtest            load the firmware if needed, then show status
 *   xromtest status     only show which IDs are present and their driver
 *   xromtest -v         verbose
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "xrom_usb.h"

static void con_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static const char *state_txt(xdev_state s)
{
    switch (s) {
    case XDEV_OK:        return "present, WinUSB OK";
    case XDEV_NO_WINUSB: return "present, but NOT using WinUSB  -> fix with Zadig";
    default:             return "not plugged in / not visible";
    }
}

static void pause_exit(void)
{
    printf("\nPress Enter to close.");
    getchar();
}

static void status(void)
{
    xrom_dev d;
    xdev_state s;

    s = xrom_open(XROM_PID_LOADER, &d);
    printf("4542:4149 (cable without firmware): %s\n", state_txt(s));
    if (s == XDEV_OK) xrom_close(&d);

    s = xrom_open(XROM_PID_RUNNING, &d);
    printf("4542:4144 (cable running X-ROM firmware): %s\n", state_txt(s));
    if (s == XDEV_OK) {
        printf("    device descriptor: VID %04X PID %04X bcdDevice %04X\n",
               d.devdesc.idVendor, d.devdesc.idProduct, d.devdesc.bcdDevice);
        printf("    bulk OUT endpoint 0x%02X, bulk IN endpoint 0x%02X (max packet %u)\n",
               d.ep_out, d.ep_in, d.in_max_packet);
        if (d.ep_in && d.ep_out)
            printf("\nThe cable is ready. Start LittleWriter.\n");
        else
            printf("\nUnexpected endpoint layout - please report this.\n");
        xrom_close(&d);
    }
}

int main(int argc, char **argv)
{
    int verbose = 0, load = 1, i;
    for (i = 1; i < argc; i++) {
        if (_stricmp(argv[i], "status") == 0) load = 0;
        else if (_stricmp(argv[i], "-v") == 0) verbose = 1;
    }
    printf("xromtest - XROM GBA Link Port USB cable check (WinUSB)\n\n");
    if (verbose) xrom_set_logger(con_log);

    if (load) {
        xdev_state ls, st;
        xrom_set_logger(con_log);
        if (!xrom_present(XROM_PID_RUNNING)) {
            if (xrom_present(XROM_PID_LOADER)) {
                printf("Cable found without firmware, loading it...\n");
                st = xrom_ensure_running(10000, &ls);
                if (ls == XDEV_NO_WINUSB) {
                    printf("\nThe cable (4542:4149) is not using WinUSB yet. Install WinUSB for it with Zadig first.\n");
                } else if (st == XDEV_FW_FAILED) {
                    printf("\nThe firmware download failed (see the error above). Unplug/replug the cable and try again.\n");
                } else if (st == XDEV_NOT_FOUND) {
                    printf("\nFirmware sent, but 4542:4144 did not appear. Unplug/replug and try again.\n");
                } else if (st == XDEV_NO_WINUSB) {
                    printf("\nFirmware loaded. The cable is now 4542:4144, but it needs WinUSB too:\n"
                           "open Zadig (Options > List All Devices), pick USB ID 4542 4144, Install Driver,\n"
                           "then run xromtest again.\n");
                }
            } else {
                printf("No XROM cable found. Plug it in (it should appear as USB ID 4542 4149).\n\n");
            }
        }
        xrom_set_logger(verbose ? con_log : NULL);
        printf("\n");
    }
    status();
    pause_exit();
    return 0;
}
