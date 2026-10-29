# Changelog

## 0.7.4 (2026-10-29)

Fixes from player reports: the mouse camera in VOB missions, and the keys and the mouse on the
title menu, in briefings and in the hub.

Updating from 0.7.3: unzip the new release (into a new folder or over the old one) and start its
launcher, then click **Set up**: it rebuilds what changed.

- **MOUSE MODE: CAMERA** now works in missions that start with a VOB (Vanguard Overed Boost)
  flight (GitHub issue #5). The mouse did nothing for the first minute of the flight: the port
  found the player's AC only through the turning it does without the VOB. Now the camera turns from
  the first second, and the AC follows at the VOB's own turning speed (very slow at first, as with
  a controller). When the AC turns past where the camera points, it now turns back instead of taking
  the camera along (it went up to 40 degrees past it during the VOB flight).
- **MOUSE MODE: STICK** no longer keeps a slowly turning AC turning long after the mouse stops: the
  turn still to come is now limited to what the AC turns in about 2.5 seconds. A normal AC still
  turns up to half a turn per flick, but in the first part of a VOB flight, which turns less than a
  degree per second, a flick kept the AC turning for minutes.
- Keys and the mouse on screens that are not menus (GitHub issue #6). On the title menu (NEW GAME,
  LOAD GAME, ...) and in mission briefings, Enter, Space and a left click now choose, and a briefing
  skips to "Accept this mission?". Before, they pressed START and SELECT there, which those screens
  ignore. Enter, Space, Escape and Backspace also no longer open the hub's KEY GUIDE. That happened
  when they were pressed while the hub was opening, for example right after leaving a briefing. Tab
  still opens the KEY GUIDE.

## 0.7.3 (2026-10-24)

Crash and movie fixes from player reports, keyboard and mouse closer to the controller, the mouse
in more menus, an optional fix for some ORDER MATCH opponents.

Updating from 0.7.2: unzip the new release and start its launcher, then click **Set up**. This
time it rebuilds more of the game than usual (the recompiler changed), which takes a few
minutes.

- Fixed: the ORDER MATCH against Dario Empio (Collared 11) crashed once his Primal Armor was
  hit below 40 % ("host exception 0xc0000005 ... f_00d34a38"), and the one against Lilium
  Wolcott (Collared 2) could do the same when her energy ran low. Their AI scripts call a
  misspelled function there; the game recovers from such script errors on the console, and now
  does in the port too (the match goes on as on the PS3).
- Movies (the intro, briefings, chapter movies) on PCs with few processor cores: on two logical
  processors the intro ran at about a quarter of its speed with broken sound; it now plays at
  full speed. The movie player's helper processors waited for work by spinning, which took the
  processor time away from the threads that give them that work.
- New: **ORDER MATCH AI FIXES**, on by default (PC SETTINGS, new page GAME). Some ORDER MATCH
  opponents' AI scripts have slips that make them fight below what was written: Dario Empio
  (Collared 11) and Lilium Wolcott (Collared 2) call misspelled functions, Fragile (Collared 17)
  fires his back weapons with the arm weapons' odds, Kamisori Johnny's (Collared 21) extra attack
  stops with a script error, Collared 6 and ORCA 02 name weapons that do not exist. These are
  corrected when a match loads its opponent: the opponents do what their scripts say, and some
  fight harder than on the console. Turn it OFF for the matches as on the PS3.
- The mouse turns and looks as a precise right stick (new default, **MOUSE MODE: STICK** in PC
  SETTINGS): the AC turns exactly as far as the mouse moves, as fast as it can turn, with the
  game's own smoothing; a quick flick turns the whole way at the AC's top turning speed (up to half
  a turn), and quick turns work with a fast sideways flick and the quick boost key.
- The earlier free mouse camera stays as **MOUSE MODE: CAMERA**, and now only the camera turns
  freely: the FCS locks on and fires along the AC's front and the AC moves along its own facing, as
  in the game, so it no longer locks on to or shoots at targets behind it. Turning keys no longer
  cut quick turns short.
- Keys pressed and released within one frame are no longer lost.
- New keys for missions (PC SETTINGS, KEYBOARD): **QUICK TURN LEFT / RIGHT** (Z / X), the
  controller's quick turn (the right stick sideways with the quick boost); hold the key to keep
  turning. Without a key until you choose one: **2-STAGE QUICK TURN LEFT / RIGHT** (about 120
  degrees), **2-STAGE QUICK BOOST** (the quick boost half pressed for its charge, then fully:
  about twice as far), **QUICK BOOST HALF PRESS** (hold it, then press the quick boost key for
  the second stage) and **ASSAULT ARMOR** (both weapon switches and overed boost together; it
  needs an overed boost part that has it). A settings file from an earlier version gets Z and X
  where no other control uses them.
- Keys keep their actions under every control type: with KEY TYPE REGULAR-B or an own KEY
  ASSIGN (ACSIS > OPTION) the keys still fire the weapon, boost and so on that they do with
  SIMPLE / REGULAR-A; PC SETTINGS shows the button each key presses in missions.
- PC SETTINGS: long captions (MOUSE SENSITIVITY X / Y) and the render scale's resolution are no
  longer cut off.
- Launcher: a **Game update** row under Game folder, to choose the game's update package (01.35)
  or the regulation.bin from it directly. Still optional: without it the disc's balance data is
  used. An update chosen after the files were prepared installs with **Install update**.
- The mouse in more of the game's menus: ACSIS > ASSEMBLE, STABILIZER and SHOP (click a slot in
  the top row to pick it, a part to equip it; the wheel scrolls through the parts), TUNE (the
  cursor was missing there and a click pressed START; now categories and items by click, the
  value with the arrow keys), DESIGN's schematics, and the ◀ ▶ arrows of choice rows (OPTIONS >
  COCKPIT, ...) and of TUNE's categories. Lists move the way the D-pad moves them. Still by keys:
  the colour editor's sliders and palette, the emblem picker, DESIGN's tabs.

## 0.7.2 (2026-10-18)

Faster: much higher frame rates, smoother mission starts, less CPU time for movies.

Updating from 0.7.1: unzip the new release (into a new folder or over the old one) and start its
launcher, then click **Set up**: it rebuilds what changed.

- Higher frame rates: with the frame rate uncapped, the heaviest mission runs at about 190
  instead of 84 frames per second on the test PC (Ryzen 9 5900X, RTX 4090), the first mission at
  about 250. The game's main thread needs about a fifth of the CPU time per frame it did, which
  helps slower PCs at 60 fps too.
- Smoother mission starts: the first frame of a mission no longer stops for about a fifth of a
  second, and the frames of the sortie loading screen are shorter.
- Compressed textures stay compressed on the graphics card (less video memory), and movie frames
  need about a quarter of the CPU time they did to reach it.

## 0.7.1 (2026-10-17)

A fix for the sky on some graphics cards, and license notices and published sources in order.

Updating from 0.7.0: unzip the new release (into a new folder or over the old one) and start its
launcher, then click **Set up**: it rebuilds what changed.

- Fixed: on some graphics cards (reported on an AMD Radeon RX 6600), parts of the sky showed as
  large darker triangles that flickered from frame to frame.
- The zip's README.txt ends with the license notices of the mingw-w64 runtime that the launcher
  contains (also in `licenses/` in the repository).
- `LICENSE` is the plain MIT license; the credit and the note on what it covers are in
  `THIRD_PARTY.md`.
- The repository holds exactly the sources the launcher carries.

## 0.7.0 (2026-10-16)

PlayStation controllers without extra programs, and a launcher that says only what helps.

Updating from 0.6.0: unzip the new release (into a new folder or over the old one) and start its
launcher; in a new folder it finds your install (**Use it**). Then click **Set up**: it rebuilds
what changed.

Controllers
- PlayStation controllers work directly: DUALSHOCK 4, DualSense and DualSense Edge, over USB or
  Bluetooth, also when connected while the game runs (before, only Xbox and other XInput
  controllers were read, so a PlayStation controller needed DS4Windows or Steam). Buttons as on
  the PS3 controller; Share / Create or the touchpad click is SELECT. No vibration on them yet.

Launcher
- The window says only what helps: the next step, or what is wrong. No more "Ready." or
  "Ready to play." (PLAY becomes clickable when the game is ready), no repeated size and folder
  under the Game data row, no notes after the shortcuts change.

## 0.6.0 (2026-10-16)

One flow from the download to playing, updating and removing the game.

Updating from 0.5.0: unzip the new release into a new folder or over the old one and start its
launcher. In a new folder it finds your 0.5.0 install (beside it, in Downloads or under Local
AppData) and offers to use it; then click **Set up** (it rebuilds what changed; tools, files,
settings and saves stay).

Launcher
- A first start is guided step by step: the game folder, the decrypted files (Prepare files, or
  your own), the game data (a new row: where it goes, the space it needs, **Change...**) and Set up.
- Before anything goes into your Downloads folder (Windows' cleanup can delete old files there) or
  a folder OneDrive syncs (gigabytes to upload; files can become online-only), the launcher proposes
  your Local AppData folder or one you choose, showing the space needed.
- A new release unzipped into a new folder finds your earlier install and offers to use it, or to
  move its game data beside the new launcher (same drive): no Prepare files and no download of the
  build tools again.
- An update rebuilds only what changed (before, Set up rebuilt the whole game after each update).
- After the first Set up, the launcher offers a desktop shortcut and a Start menu entry
  (**More...** adds or removes them later); a newer launcher started from another folder makes
  them open itself.
- **More... > Remove game data...** deletes what the launcher and the game put on the disk, listed
  with sizes; your saves and settings only when you tick that box, and they are then kept as a
  backup. **More... > Install folder** became the Game data row's **Change...**.

Release
- The README explains Windows SmartScreen's warning (the launcher is not code-signed) and how to
  check the download against the SHA-256 on the release page.

## 0.5.0 (2026-10-16)

The mouse moves the camera, new default keys, and the port's icon.

Updating from 0.4.0: unzip over the old release (replace `ACFA-Launcher.exe`), start the launcher
and click **Set up** (it rebuilds what changed; tools, files, settings and saves stay).

Mouse and keyboard
- Mouse camera: the mouse moves the camera at once, at any speed, by exactly the angle the mouse
  moved (0.09 degrees per count at sensitivity 1.0; no smoothing, acceleration or dead zone), and
  the AC turns to where the camera points with its own turning and stops there. The game still
  places the camera (distance, height, the ground) and still smooths the AC's own movement; the
  camera's half-second swing after a turn of the view is gone while the mouse has the camera.
  Once the AC points where the camera does and the mouse rests, the camera rides with the AC
  again. A controller used after the mouse gives the camera back to the game.
- New default keys for missions: the mouse moves the camera, W A S D move, left click Square
  (left arm weapon), right click Cross (right arm weapon), middle click L3, Q L1 (overed boost),
  E R1 (shoulder weapon), Space L2 (boost), Shift R2 (quick boost), C Circle, Ctrl Triangle, V R3
  (reset view), the arrow keys the D-pad, Tab START (Escape too), Enter SELECT. A settings file
  that still has 0.4.0's default keys gets the new ones; keys you changed are kept.
- After the system's save or load list, its questions or its text entry, the game gets the
  controller's full state again as soon as the dialog closes (from the Armored Core 4 port, whose
  menus moved by themselves without it).

Release
- The launcher and the game have the port's icon.

## 0.4.0 (2026-10-16)

Fixes from play-testing 0.3.0, and a smaller download.

Release
- The release zip holds two files: `ACFA-Launcher.exe` and `README.txt`. The launcher carries the
  port's sources and unpacks them into its install folder (`local\source`) when it needs them;
  the launcher's file properties and title bar show its version. Updating: unzip over the old
  release, start the launcher, click **Set up** (it rebuilds what changed; tools, files,
  settings and saves stay). 0.3.0's source files beside the launcher are no longer used
  (README: Setup).

Game files
- **Prepare files**: you answer RPCS3's own questions. When RPCS3 asks "Install firmware:
  ...PS3UPDAT.PUP?" (click **Yes**) or, for the game's update, "Do you want to install this
  package?" (click **Install**), the launcher brings the question to the front, says what it asks
  and what to click, and waits for you. **No**, or closing the question, stops Prepare files with
  nothing left half-done; **Cancel** on the update's question goes on without the update (the
  disc's balance data is used). Other RPCS3 windows are handled the same way. Decrypting the files
  yourself and choosing their folder works as before.

Mouse and keyboard
- Mouse look: the AC turns and the view pitches by exactly the angle the mouse moved (0.09
  degrees per count at sensitivity 1.0), with no smoothing, acceleration or dead zone. A movement
  faster than the AC can turn is not cut short: the AC turns at its full turning speed until it
  points where the mouse points. The same at every frame rate.
- Tutorial: every step can be done with keyboard and mouse; turning and looking up and down with
  the mouse count like the stick held for as long.
- Menu cursor: in every menu it comes back as soon as the mouse moves (after PC SETTINGS, a
  mission's end, the pause menu, the tutorial and movies); keys or a controller hide it.
  Cutscenes, movies, the intro and other scenes without a menu show no cursor, even when the
  mouse moves. Outside missions the game no longer holds the mouse.
- Fixed: clicks on the game's small message boxes ("Enter pilot name.", the tutorial's restart
  notice, the client choice's notes and confirmation) did nothing.
- Fixed: the cursor blinked when it moved onto another client in CLIENT SELECT, and a click made
  just as it got there was lost.
- A click continues the mission report (it waits for the confirm button, which a click did not
  press).

Display
- Saving, loading, questions and text entry (the pilot name) appear inside the game's picture
  (they were Windows dialogs): the window keeps its mode, size and position during and after
  them, in borderless and windowed (exclusive fullscreen still to be checked). Point and click,
  or use the keyboard (type, Enter, Escape, arrows).
- Fixed: when the display went away in exclusive fullscreen while the game was in front (a
  monitor switched off or dropping out), the game switched the display mode again and again; it
  now enters exclusive fullscreen again when you come back to it (Alt+Tab) or choose the mode.
- Fixed: with no monitor attached, or a display that does not wait for its refresh, the game ran
  far too fast; its own clock paces it then.

## 0.3.0 (2026-10-15)

Game files
- `EBOOT.BIN` and 10 system modules of PS3 system software 4.93 are decrypted by your RPCS3. **Prepare files** in the launcher does it in one step with your `rpcs3.exe` and
  the update file `PS3UPDAT.PUP` (a private copy of RPCS3; your RPCS3 is not changed), or you
  choose a folder with files you decrypted yourself. Each file is checked by its SHA-256; the
  port decrypts nothing and downloads no system software.

Display
- The game renders at your window's or screen's resolution (text and interface sharp at 1440p
  and 4K), with a render scale from 50 % to 200 %; sun glare stays as on the PS3 at any
  resolution.
- Borderless fullscreen at your display's own resolution is the default; monitor choice;
  windowed, borderless and exclusive fullscreen as before.
- Frame rates: 30, 60, 120, 144, your display's refresh rate and uncapped, each checked in a
  mission (same movement and turning as at 60).

Mouse and keyboard
- Menus with the mouse: a cursor in the game's style, items highlighted under it, click to
  choose, right click to go back, wheel to move, sliders follow a click or drag, list rows.
- Menus with the keyboard: arrows or W A S D, Enter or Space, Escape or Backspace, Tab (START),
  Q / E (pages); Enter, Escape or a click on the title screen and in movies.
- Separate X and Y mouse sensitivity and inversion; a MENU KEYS page in PC SETTINGS to change the
  keys for menus.
- Mouse look also in the tutorial; the game takes the mouse as soon as a mission, the tutorial,
  AC TEST or an order match starts (also after menus used with the keyboard) and again when you
  come back with Alt+Tab.

Launcher, settings, saves
- A simpler launcher: the game folder, the decrypted files (**Prepare files**), **Set up**,
  display mode, resolution, frame rate and **PLAY**; the rest under **More...**.
- The game opens only its window (no console window).
- Settings and saves in `Saved Games\ARMORED CORE for Answer` (copied once from the install
  folder); presets for modest PCs and handhelds; **Reset all settings...**; **Play in safe mode**.
- When the game stops with an error, the launcher shows it; logs contain no personal data.

## 0.2.0 (2026-10-13)

- PS3 system software 4.93 as an alternative to the disc's 2.41 (replaced by 0.3.0's decrypted
  files).
- Fixed: a recompiler error that let some instructions overwrite a condition the game then
  tested (eleven places in the game's code).
- Fixed: a cause of a rare crash between the title and the hub (the graphics command buffer could
  be overrun by one command).

## 0.1.0 (2026-10-13)

- First release: the launcher (game file checks, setup with progress, settings, saves, Play),
  PC SETTINGS in the game's SYSTEM menu, keyboard and mouse bindings, frame rates above 60 in
  missions.
