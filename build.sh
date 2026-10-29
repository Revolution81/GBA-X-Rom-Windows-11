#!/bin/sh
# Build DlPortIO.dll (the shim) and xromtest.exe with Zig's C cross-compiler.
# Works on Linux, macOS and Windows (Git Bash). Get Zig from https://ziglang.org
# or with:  pip install ziglang==0.13.0   (then ZIG="python3 -m ziglang")
#
# Both are 32-bit: LittleWriter is a 32-bit program and the shim is loaded into it.
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"
CFLAGS="-target x86-windows-gnu -O2 -Wall -Wno-unused-function -Isrc"
LIBS="-lsetupapi -lwinusb -lole32 -luser32 -ladvapi32"
mkdir -p build
$ZIG cc $CFLAGS -shared -o build/DlPortIO.dll src/shim.c src/xrom_usb.c src/DlPortIO.def $LIBS -s
$ZIG cc $CFLAGS -o build/xromtest.exe src/xromtest.c src/xrom_usb.c $LIBS -s
rm -f build/*.pdb build/*.lib
ls -l build
