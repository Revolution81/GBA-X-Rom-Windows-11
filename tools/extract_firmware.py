#!/usr/bin/env python3
"""
Extract the X-ROM Link Port cable firmware from the original XROMLP.SYS driver
(XROM LP Init Driver 1.0.0.4, 12/10/2003, shipped in "X-Rom Drivers.exe").

The driver is based on Cypress's EZ-USB "ezloader" sample, which stores the
8051 firmware as an array of INTEL_HEX_RECORD structs:

    struct { BYTE Length; /*pad*/ WORD Address; BYTE Type; BYTE Data[16]; /*pad*/ }  // 22 bytes

The array whose USB device descriptor says VID 4542 / PID 4144 is the X-ROM
firmware. (The driver also contains Cypress's Vend_Ax loader, which isn't
needed because the firmware fits in the chip's internal RAM.)

Usage:
    python3 extract_firmware.py XROMLP.SYS            -> xrom_firmware.hex / .h
    python3 extract_firmware.py XROMLP.SYS outdir/

How to get XROMLP.SYS: "X-Rom Drivers.exe" is a Wise installer containing an
MSI, whose CAB holds XROMLP.SYS. Extracting the installer with 7-Zip
(twice: .exe -> .msi -> files) works.
"""
import os
import struct
import sys

REC = 22
VID_PID_4144 = bytes.fromhex("12011001") + b"\x00\x00\x00\x40" + bytes.fromhex("42454441")


def scan(data):
    """Yield (offset, records) for each complete hex-record array."""
    seen_ends = set()
    for start in range(len(data) - REC * 4):
        recs, i = [], start
        while i + REC <= len(data):
            ln, addr, typ = data[i], struct.unpack_from("<H", data, i + 2)[0], data[i + 4]
            if typ == 1 and ln == 0:
                recs.append((0, addr, 1, b""))
                break
            if typ != 0 or not 1 <= ln <= 16:
                recs = None
                break
            recs.append((ln, addr, 0, data[i + 5:i + 5 + ln]))
            i += REC
        if recs and len(recs) > 20 and recs[-1][2] == 1:
            end = start + REC * len(recs)
            if end not in seen_ends:
                seen_ends.add(end)
                yield start, recs


def image(recs):
    img = bytearray(0x2000)
    hi = 0
    for ln, addr, typ, d in recs:
        if typ == 0:
            img[addr:addr + ln] = d
            hi = max(hi, addr + ln)
    return bytes(img[:hi])


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    data = open(sys.argv[1], "rb").read()
    out = sys.argv[2] if len(sys.argv) > 2 else "."
    os.makedirs(out, exist_ok=True)

    for off, recs in scan(data):
        if VID_PID_4144 not in image(recs):
            continue
        hex_lines, c_lines = [], []
        for ln, addr, typ, d in recs:
            raw = bytes([ln, addr >> 8, addr & 0xFF, typ]) + d
            hex_lines.append(":" + raw.hex().upper() + "%02X" % ((-sum(raw)) & 0xFF))
            if typ == 0:
                c_lines.append("  {%d,0x%04X,{%s}}," % (ln, addr, ",".join("0x%02X" % b for b in d)))
        open(os.path.join(out, "xrom_firmware.hex"), "w").write("\n".join(hex_lines) + "\n")
        open(os.path.join(out, "xrom_firmware.h"), "w").write(
            "/* X-ROM Link Port cable EZ-USB (AN21xx) firmware.\n"
            " * Extracted from XROMLP.SYS (XROM LP Init Driver v1.0.0.4, 12/10/2003).\n"
            " * Each record: length, address, data[16]. */\n"
            "typedef struct { unsigned char len; unsigned short addr; unsigned char data[16]; } fw_rec;\n"
            "static const fw_rec xrom_firmware[] = {\n" + "\n".join(c_lines) + "\n};\n"
            "#define XROM_FIRMWARE_RECORDS %d\n" % len(c_lines))
        print("Found firmware at file offset 0x%X: %d records" % (off, len(c_lines)))
        print("Wrote xrom_firmware.hex and xrom_firmware.h to", out)
        return 0
    print("No X-ROM firmware (VID 4542 / PID 4144) found in", sys.argv[1])
    return 1


if __name__ == "__main__":
    sys.exit(main())
