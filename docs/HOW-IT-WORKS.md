# How it works

These are notes from reverse engineering the 2003 X-Rom drivers and LittleWriter 1.0.8.4, and how the WinUSB replacement fits in.

## The hardware

The blue X-Rom cable is a **Cypress EZ-USB AN21xx** chip, an 8051 microcontroller with a USB 1.1 full-speed core. It has no firmware of its own; a small EEPROM only gives it a USB ID:

| State | USB ID | What it is |
|---|---|---|
| Blank | `4542:4149` | EZ-USB core waiting for firmware |
| Running | `4542:4144` | X-Rom firmware: 1 bulk OUT endpoint (`0x02`) and 1 bulk IN endpoint (`0x82`), 64-byte packets |

The cable is essentially a USB-to-GBA-link-port bridge. All the cartridge logic runs either in LittleWriter on the PC or in a small program that LittleWriter sends to the GBA.

## The original drivers

`X-Rom Drivers.exe` is a Wise installer containing an MSI, whose CAB holds:

- **`XROMLP.SYS`** ("XROM LP Init Driver", 1.0.0.4, Dec 2003). This is Cypress's *ezloader* sample. When `4542:4149` appears, it downloads an embedded firmware image into the 8051's RAM and starts it. The chip then disconnects and comes back as `4542:4144`.
- **`XROMIO.SYS`** ("XROM LP Final Driver"). Its version resource still reads *BlockIO.sys, "USB Design By Example", © 1999 Walter Oney Software, © 2000 John Hyde*. It is the textbook BlockIO sample, almost unchanged:
  - it registers device interface `{606377C1-2270-11D4-BFD8-00207812F5D5}`
  - `ReadFile` becomes a bulk IN transfer and `WriteFile` a bulk OUT transfer, both split into stages
  - its only IOCTL, `0x222000`, returns the USB device descriptor
- **`Generic.sys`**: Walter Oney's support library for the above.

All three are 32-bit kernel drivers. 64-bit Windows can't load them, and Windows 10/11 would reject them anyway, because they're unsigned.

## How LittleWriter talks to the cable

Disassembling `LittlewriterDll.dll` shows that the X-Rom code path uses only plain Win32 calls:

1. `SetupDiGetClassDevs` with the BlockIO GUID, then `SetupDiEnumDeviceInterfaces` and `SetupDiGetDeviceInterfaceDetail` to get a device path.
2. `CreateFile` on that path.
3. `WriteFile` and `ReadFile` for everything else. Commands are small packets (many start with `0x55AA00xx`), followed by data blocks and short acknowledgements.

LittleWriter also embeds a GBA **multiboot** program, about 5.4 KB, inside `LittlewriterDll.dll`. When you switch the GBA on while holding **START + SELECT**, the GBA skips the cartridge and waits on the link port. LittleWriter runs the standard multiboot handshake (`0x6202`, `0x63C1`, …) through the cable and uploads that program. From then on, the program on the GBA does the actual flash reading, erasing and writing on the X-Rom cart.

`LittlewriterDll.dll` also imports `DlPortIO.dll`, a port-I/O helper it used for older parallel-port writers. Windows looks for that DLL in the program's own folder first, which is what makes a drop-in replacement possible.

## The replacement

### WinUSB

Both USB IDs are bound to Microsoft's WinUSB driver with Zadig. WinUSB is signed by Microsoft and built into Windows, so no kernel code from this project is involved.

### `DlPortIO.dll` (src/shim.c)

When LittleWriter loads, Windows loads this DLL as one of `LittlewriterDll.dll`'s dependencies. Its `DllMain`:

1. Walks `LittlewriterDll.dll`'s import table and redirects nine entries: `SetupDiGetClassDevs`, `SetupDiEnumDeviceInterfaces`, `SetupDiGetDeviceInterfaceDetail` and `SetupDiDestroyDeviceInfoList` (A and W variants), plus `CreateFile`, `ReadFile`, `WriteFile`, `DeviceIoControl` and `CloseHandle`.
2. Starts a thread that repeats the patch once the loader has finished, as a safety net.

The hooks only act on the BlockIO device. Every other call, such as opening ROM files, goes straight to Windows:

- **Device lookup.** `SetupDiGetClassDevs(BlockIO GUID)` makes sure the cable is running, loading the firmware if only `4542:4149` is present. It then returns a fake device list containing one path, `\\.\XRomShim-WinUSB-0`.
- **Open.** `CreateFile` on that path opens the cable through WinUSB. The handle returned to LittleWriter is a real (unused) Windows event object, so it can never collide with a file handle.
- **Writes.** `WriteFile` on that handle becomes `WinUsb_WritePipe` on the bulk OUT endpoint.
- **Reads.** `ReadFile` on that handle becomes `WinUsb_ReadPipe` on the bulk IN endpoint. The read repeats until the full requested length arrives, matching BlockIO's staged reads. WinUSB alone would stop at the first short packet.
- **IOCTL.** `DeviceIoControl(0x222000)` returns the USB device descriptor.

The DlPortIO port functions themselves are stubs, so parallel-port writers aren't supported.

### Firmware loading (src/xrom_usb.c)

The EZ-USB core handles vendor request `0xA0` ("Anchor Load") in hardware, so the firmware can be loaded with plain WinUSB control transfers:

1. Write `0x01` to `CPUCS` (`0x7F92`) to hold the 8051 in reset.
2. Write the 182 firmware records (2,854 bytes, all in internal RAM `0x0000–0x0BBA`).
3. Write `0x00` to `CPUCS` to start the 8051. It re-enumerates as `4542:4144`.

The firmware was extracted from `XROMLP.SYS` (an array of 22-byte `INTEL_HEX_RECORD` structs); see `tools/extract_firmware.py`.

**Gotcha found during testing:** `WinUsb_ControlTransfer` failed with error 998 (`ERROR_NOACCESS`) when given a pointer into read-only memory, even for host-to-device transfers. WinUSB checks the buffer for write access. The fix was to send every record from a writable stack buffer.

## Limitations and ideas

- **Only the X-Rom USB link cable is supported** (LittleWriter's other writer types are not).
- **A standalone writer would be possible.** The cable protocol and the GBA multiboot program are all inside `LittlewriterDll.dll`, so a tool that writes ROMs without LittleWriter could be built. A `Log=2` capture of a LittleWriter session would be the starting point.
