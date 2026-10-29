@echo off
rem Build DlPortIO.dll and xromtest.exe with Zig (https://ziglang.org).
rem Put zig.exe on your PATH, then run this file.
setlocal
cd /d "%~dp0"
if not exist build mkdir build
set CFLAGS=-target x86-windows-gnu -O2 -Wall -Wno-unused-function -Isrc
set LIBS=-lsetupapi -lwinusb -lole32 -luser32 -ladvapi32
zig cc %CFLAGS% -shared -o build\DlPortIO.dll src\shim.c src\xrom_usb.c src\DlPortIO.def %LIBS% -s || goto :fail
zig cc %CFLAGS% -o build\xromtest.exe src\xromtest.c src\xrom_usb.c %LIBS% -s || goto :fail
del /q build\*.pdb build\*.lib 2>nul
echo Built build\DlPortIO.dll and build\xromtest.exe
exit /b 0
:fail
echo Build failed.
exit /b 1
