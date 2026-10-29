X-ROM GBA Link Port cable + LittleWriter 1.0.8.4 on Windows 10/11 (64-bit)
=========================================================================

WHY THE ORIGINAL SOFTWARE STOPPED WORKING
  The cable is a Cypress EZ-USB chip. In 2003 it needed two 32-bit kernel
  drivers (XROMLP.SYS loaded the cable's firmware, XROMIO.SYS moved the
  data). 64-bit Windows cannot load 32-bit drivers, and Windows 11 also
  rejects unsigned ones, so the drivers will never install there.

  LittleWriter itself is an ordinary 32-bit program and runs fine on
  Windows 11. Only the drivers were the problem.

WHAT THIS PACKAGE DOES INSTEAD
  * Uses Microsoft's own WinUSB driver (built into Windows) for the cable.
  * LittleWriter\bin\release unicode\DlPortIO.dll is a replacement for a
    small DLL LittleWriter already loads. It redirects LittleWriter's
    cable traffic to WinUSB, and it sends the cable its firmware (taken
    from the original XROMLP.SYS) every time the cable is plugged in.
  * None of LittleWriter's own files were changed. This is a "portable"
    copy of LittleWriter, so there's no installer to run.


ONE-TIME SETUP (about 5 minutes)
--------------------------------
1. Extract this whole folder somewhere you can write to, e.g. C:\XRom
   (NOT inside "Program Files"). If Windows says the files are blocked,
   right-click the .zip > Properties > tick "Unblock" > OK, then extract.

2. Download Zadig from https://zadig.akeo.ie (a single .exe, no install).

3. Plug the X-ROM cable into USB. Nothing needs to be attached to the
   other end yet.

4. In Zadig: Options > List All Devices. Pick the device whose USB ID
   shows 4542 4149. Make sure the target driver says WinUSB, then
   click "Install Driver" (or "Replace Driver"). Wait for it to finish.

5. Double-click xromtest.exe. It sends the firmware to the cable. The
   cable restarts and shows up as a NEW device, USB ID 4542 4144.
   The first time, xromtest will say that this new device needs WinUSB.

6. Back in Zadig (Options > List All Devices again; you may need to
   re-open Zadig to refresh the list), pick USB ID 4542 4144 and
   install WinUSB on it too.

7. Run xromtest.exe once more. It should end with:
       "The cable is ready. Start LittleWriter."

Both IDs need WinUSB because the cable shows up under one ID before its
firmware is loaded and under the other ID afterwards. Windows remembers
both, so you only do this once per PC.


EVERY TIME AFTER THAT
---------------------
1. Plug in the cable.
2. Double-click "Start LittleWriter.bat" (or make a shortcut to
   LittleWriter\bin\release unicode\LittleWriterWin32.exe).
3. Put the X-ROM cart in the Game Boy Advance and connect the cable to
   the GBA's link port.
4. Hold START + SELECT while you switch the GBA on. This puts it in
   transfer mode: instead of starting the cart, the GBA waits for
   LittleWriter to send it the transfer program over the link cable.
   Without this, LittleWriter cannot connect to the GBA.
5. In LittleWriter, click Refresh to read the cart, then Flash / Erase
   as before.

The firmware is loaded automatically when LittleWriter looks for the cable
(the cable forgets it whenever it's unplugged). That takes about a second.


TROUBLESHOOTING
---------------
* A log is written to LittleWriter\bin\release unicode\XRomShim.log.
  The first lines should say "N imports redirected". If something goes
  wrong, that log shows where.
* For a detailed log of every USB packet, open XRomShim.ini (same folder)
  and set Log=2.
* "Cable found but it is not using the WinUSB driver" pop-up: one of the
  two IDs (4542 4149 / 4542 4144) still needs WinUSB in Zadig.
* Don't run LittleWriter "as administrator". There's no need, and as
  admin it may try to register the old parallel-port driver.
* Parallel-port writers are not supported by this package (only the
  X-ROM USB link cable).
* If reads or writes time out on very long operations, raise
  ReadTimeoutMs in XRomShim.ini (0 = wait forever, like the old driver).
* Running xromtest.exe status only reports the state without loading
  the firmware.


FOLDER CONTENTS
---------------
  Start LittleWriter.bat   launcher
  xromtest.exe             cable checker / firmware loader
  LittleWriter\            LittleWriter 1.0.8.4 (Unicode build), its plugins,
                           languages and the Visual C++ 2003 runtime DLLs
  LittleWriter\bin\release unicode\DlPortIO.dll   the WinUSB shim
  LittleWriter\bin\release unicode\XRomShim.ini   shim settings
  source\                  source code of the shim and xromtest
