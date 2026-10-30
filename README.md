# X-Rom GBA flash cart on Windows 10/11

![X-Rom USB link cable and 512 Mbit cartridge](<docs/X-Rom cart & linker.jpg>)

Run the **X-Rom GBA Link Port USB cable** and **LittleWriter 1.0.8.4** on modern 64-bit Windows, without a Windows 98 virtual machine.

The X-Rom was a 512 Mbit Game Boy Advance flash cart kit sold around 2004. You put the cart in a GBA, connect the GBA's link port to the PC with a blue USB cable, and write games with LittleWriter. Its drivers stopped working after Windows XP. This project replaces them with Microsoft's built-in WinUSB driver and a small drop-in DLL.

**Status:** tested on Windows 11 with a real cable and cart. Reading the cart, erasing and flashing a new game all work.

---

## Why it stopped working

LittleWriter itself is an ordinary 32-bit Windows program, and it still runs on Windows 11. The problem was the cable's two kernel drivers from 2003:

| Driver | Job | Why it fails today |
|---|---|---|
| `XROMLP.SYS` | Loads the cable's firmware when you plug it in | 32-bit kernel driver, unsigned |
| `XROMIO.SYS` | Moves data between LittleWriter and the cable | 32-bit kernel driver, unsigned |

64-bit Windows can't load 32-bit drivers at all, and Windows 11 also rejects unsigned ones. Until now the only option was a Windows 98/XP virtual machine with USB passthrough.

## How this fixes it

- **WinUSB** (Microsoft's own generic USB driver, built into Windows) takes over the cable. You attach it once with [Zadig](https://zadig.akeo.ie).
- **`DlPortIO.dll`** is a replacement for a small DLL that LittleWriter already loads from its own folder. It redirects LittleWriter's cable traffic to WinUSB. It also contains the cable's firmware, extracted from the original driver, and sends it each time the cable is plugged in.
- **LittleWriter itself isn't modified at all.**

The details are in [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md).

---

## Download

Grab **`X-Rom-for-Windows-11-v1.0.zip`** from the [Releases](../../releases) page. It's a ready-to-run portable folder containing:

- LittleWriter 1.0.8.4 (Unicode build), its plugins and languages, and the Visual C++ 2003 runtime DLLs it needs
- `DlPortIO.dll`, the WinUSB shim, plus `XRomShim.ini` for its settings
- `xromtest.exe`, which checks the cable and loads its firmware
- `Start LittleWriter.bat`, a launcher

There's no installer, and nothing is copied into Windows.

## Setup (once per PC, about 5 minutes)

1. **Extract** the zip somewhere you can write to, e.g. `C:\XRom`. Don't use *Program Files*. If Windows says the files are blocked, right-click the zip, choose *Properties*, tick *Unblock*, then extract.
2. **Download Zadig** from <https://zadig.akeo.ie>. It's a single .exe and needs no install.
3. **Plug in the X-Rom cable.** Nothing needs to be attached to the other end yet.
4. **In Zadig, set up the cable before its firmware is loaded.** Choose *Options → List All Devices*, pick the device with USB ID **`4542 4149`**, make sure the target driver says **WinUSB**, then click *Install Driver*.
5. **Run `xromtest.exe`.** It sends the firmware, and the cable restarts as a new device with USB ID **`4542 4144`**. The first time, xromtest will say that this ID needs WinUSB too.
6. **In Zadig, set up the running cable.** Re-open Zadig, choose *List All Devices*, pick **`4542 4144`** and install **WinUSB**.
7. **Run `xromtest.exe` again.** It should end with *"The cable is ready. Start LittleWriter."*

Both USB IDs need WinUSB because the cable shows up under one ID before its firmware is loaded and under the other afterwards. Windows remembers both, so you only do this once per PC.

## Using it

1. Plug the cable into USB.
2. Run **`Start LittleWriter.bat`**.
3. Put the X-Rom cart in the Game Boy Advance and plug the cable into the GBA's **link port**.
4. **Hold START + SELECT while you switch the GBA on.** This puts the GBA into transfer mode: instead of starting the cart, it waits for LittleWriter to send it the transfer program over the link cable. If you skip this, LittleWriter can't connect to the GBA.
5. In LittleWriter, click **Refresh** to read the cart, then use **Flash** and **Erase** as usual.

The cable's firmware loads automatically whenever LittleWriter looks for the cable, because the cable forgets it each time it's unplugged. That takes about a second.

> **Back up first.** Before erasing or flashing, back up the save data on the cart and any games you care about.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Pop-up: *"Cable found but it is not using the WinUSB driver"* | One of the two IDs (`4542 4149` or `4542 4144`) still needs WinUSB in Zadig. |
| LittleWriter can't see the GBA / cart | Switch the GBA off, then on again **while holding START + SELECT**. Check the link cable is fully seated. |
| xromtest: *firmware download failed* | Unplug and replug the cable, then run xromtest again. |
| Erase or flash fails partway with a timeout | In `XRomShim.ini`, set `ReadTimeoutMs=0` (wait forever, like the original driver). |
| Anything else | Check `LittleWriter\bin\release unicode\XRomShim.log`. For a log of every USB packet, set `Log=2` in `XRomShim.ini`. |

Also:

- **Don't run LittleWriter as administrator.** There's no need, and as admin it may try to register the old parallel-port driver.
- **Only the X-Rom USB link cable is supported.** LittleWriter's parallel-port writers aren't.

### `XRomShim.ini`

```ini
[XRomShim]
Log=1               ; 0 off, 1 normal, 2 log every USB packet
ReadTimeoutMs=30000 ; 0 = wait forever
WriteTimeoutMs=30000
FirmwareWaitMs=10000
ShowErrors=1
```

---

## Building from source

The release binaries are built with [Zig](https://ziglang.org)'s C cross-compiler, so no Visual Studio is needed.

```sh
./build.sh                         # Linux / macOS / Git Bash  (needs zig on PATH)
ZIG="python3 -m ziglang" ./build.sh   # or with  pip install ziglang==0.13.0
build.bat                          # Windows cmd, needs zig.exe on PATH
```

The output goes to `build/DlPortIO.dll` and `build/xromtest.exe`. Both are 32-bit, because LittleWriter is a 32-bit program.

### Repository layout

```
src/shim.c              DlPortIO.dll: import-table hooks + DlPortIO stubs
src/xrom_usb.c/.h       WinUSB device discovery, firmware loader, bulk I/O
src/xromtest.c          cable checker / firmware loader
src/DlPortIO.def        export names matching the original DlPortIO.dll
src/xrom_firmware.h     cable firmware (generated)
firmware/               the same firmware as Intel HEX
tools/extract_firmware.py   regenerates the firmware from XROMLP.SYS
docs/HOW-IT-WORKS.md    technical write-up
docs/README-release.txt the README shipped inside the release zip
```

## Credits

- **LittleWriter:** dadycool (2004).
- **X-Rom kit:** sold by EasyBuy2000.com.
- **`XROMIO.SYS`:** based on John Hyde's *USB Design by Example* BlockIO sample, which builds on Walter Oney's WDM samples.
- **[Zadig](https://zadig.akeo.ie) / libwdi:** Pete Batard.

## License

The shim and tool source code is under the MIT License (see [LICENSE](LICENSE)). The cable firmware was extracted from the original 2003 XROM driver. LittleWriter and its runtime files belong to their respective authors.
