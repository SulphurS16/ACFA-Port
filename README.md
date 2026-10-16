# ARMORED CORE for Answer: unofficial PC port

A native Windows version of ARMORED CORE for Answer (PS3, USA, BLUS30187), built on your PC from your own copy of the game. This project contains no game data, no game code and no keys, and it decrypts nothing.

Not affiliated with, endorsed or sponsored by FromSoftware, Ubisoft, Sony Interactive Entertainment or any other rights holder. ARMORED CORE is a trademark of its owners.

## What you need

- Windows 10 (1903 or later) or Windows 11, 64-bit.
- A CPU from Intel Haswell (2013), AMD Excavator or Ryzen onward (AVX2; the launcher checks), and a Direct3D 11 graphics card.
- About 4 GB of free disk space, and an internet connection once for the build tools (about 250 MB).
- Your own copy of the game: the disc's files (the folder with `PS3_GAME`; BLUS30187, disc version 01.01).
- Once, for setup: RPCS3 (rpcs3.net) and the PS3 system software 4.93 file `PS3UPDAT.PUP` (playstation.com).
- Optional: the game's update 01.35 (`UP0001-BLUS30187_00-AC45000004141906-A0135-V0100-PE.pkg`), placed in the game folder or beside `PS3UPDAT.PUP`.

## Install

1. Download `ACFA-Port-<version>.zip` from the release page, right-click it, choose **Extract All...** and start `ACFA-Launcher.exe`. If Windows says "Windows protected your PC" (the launcher is not code-signed), click **More info**, then **Run anyway**.
2. **Game folder:** choose the folder with `PS3_GAME`.
3. **Prepare files:** choose your `rpcs3.exe` and `PS3UPDAT.PUP`. RPCS3 asks "Install firmware: ...?" (click **Yes**) and, if you have the update, "Do you want to install this package?" (click **Install**). It takes about a minute and does not change your RPCS3.
4. **Set up** (once): downloads the build tools and builds the game, about 4 minutes on a fast PC. If it stops, start it again; finished steps are kept.
5. Choose your shortcuts, then click **PLAY**.

The game data goes into the `local` folder beside the launcher; **Change...** picks another place.

Already decrypted the files yourself? Choose their folder under **Decrypted files** (**More... > Which files does the port need?** lists them).

Optional: check the download against the SHA-256 on the release page (PowerShell: `Get-FileHash ACFA-Port-<version>.zip`).

## Update

Unzip the new release into a new folder or over the old one, start its launcher and click **Set up**. It finds your earlier install and rebuilds only what changed; your settings and saves stay. If your game data is in the old release's folder, keep that folder, or choose **Move it beside this launcher**.

## Uninstall

**More... > Remove game data...**, then delete the launcher's folder. Saves and settings stay unless you tick their box; they then go to a backup folder.

## Controls

Controllers work like the PS3 pad and can be connected at any time:
- **PlayStation** (DUALSHOCK 4, DualSense, DualSense Edge; USB or Bluetooth): Options = START; Share / Create or the touchpad click = SELECT. No extra program needed. No vibration yet.
- **Xbox and other XInput controllers:** A = Cross, B = Circle, X = Square, Y = Triangle, LB / RB = L1 / R1, LT / RT = L2 / R2, Menu = START, View = SELECT, stick clicks = L3 / R3.

With several controllers connected, the first one plays (Xbox-style controllers first).

Keyboard and mouse in missions (defaults, for the game's SIMPLE control layout):

| Action | Keys |
| --- | --- |
| Move | W A S D |
| Camera | Mouse |
| Left / right arm weapon | Left / right click |
| Shoulder weapon | E |
| Switch right / left arm weapon | C / Ctrl |
| Boost (hold to rise) / quick boost / overed boost | Space / Shift / Q |
| Reset view | V |
| L3 | Middle click |
| D-pad | Arrow keys |
| Pause / SELECT | Tab or Escape / Enter |
| Release the mouse | F1 |
| Fullscreen | F11 or Alt+Enter |

The mouse moves the camera directly (raw input), and the AC turns to where the camera points at its own speed.

In menus:
- **Mouse:** point and click; right-click goes back; the wheel scrolls; drag sliders.
- **Keys:** arrows or W A S D move, Enter or Space choose, Escape or Backspace go back, Tab = START, Q / E = L1 / R1.

Saving, loading and text entry appear inside the game's window. The whole tutorial works with keyboard and mouse.

All keys can be changed in game under **SYSTEM > PC SETTINGS**.

## Settings and saves

- Display mode, resolution, render scale (50–200 %) and frame rate (30, 60, 120, 144, your display's rate or uncapped) are set in the launcher or in game under **SYSTEM > PC SETTINGS**. Game speed is the same at every frame rate.
- The launcher has presets for modest PCs and handhelds under **More...**. With a graphics card of 4 GB or less, keep the render scale at or below 100 % at 4K.
- Settings and saves are in `Saved Games\ARMORED CORE for Answer`. **More...** in the launcher backs up and restores saves. Saves don't work on a PS3.

## Known limitations

- No online features (multiplayer, rankings, regulation downloads).
- Emblem and paint editing results may not display correctly.
- On-screen prompts show PS3 buttons.

## License

MIT (`LICENSE`). Third-party notices: `THIRD_PARTY.md` and `licenses`. Version: the launcher's title bar; changes: `CHANGELOG.md`. To build the launcher from this repository: `powershell -ExecutionPolicy Bypass -File tools\build-launcher.ps1`.