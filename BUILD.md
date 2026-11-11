# RTSP_OS (Polish UI, XMB menu) + Game Boy emulator: build and install

The emulator core (Peanut-GB, MIT license) is C code, so it is compiled **into the MicroPython firmware**. The build downloads the Peanut-GB header for you.

## 1. Build the firmware (GitHub, nothing to install)

1. Create a free **private** GitHub repository.
2. Upload the *contents* of this folder: `usermod`, `frozen`, `emulators`, `manifest.py`, `BUILD.md` and the hidden `.github` folder. When updating, overwrite the existing files.
3. Open **Actions**, choose **Build TinyOS firmware**, click **Run workflow**. Options:
   - **board**: `RPI_PICO_W` (default) or `RPI_PICO`. TinyOS does not use Wi-Fi, and the plain `RPI_PICO` build leaves it out, which should free some RAM. It runs fine on a Pico W. Compare *System info* (RAM free) of the two builds and keep the better one.
   - **freeze_os** (default on): puts the TinyOS system files into the firmware itself so they no longer take RAM at boot. If the build fails on this step, run it again with this switched off and send me the log.
4. After 5 to 15 minutes download the artifact `tinyos-firmware` (it contains `firmware.uf2`).

If a run fails, open the failed step and send me the last 30 lines of its log.

## 2. Flash it

Hold BOOTSEL, plug in the Pico, copy `firmware.uf2` to the `RPI-RP2` drive. Your files normally survive a firmware update.

## 3. If you built with freeze_os (the default)

After flashing, delete these six files from the Pico's root in Thonny, because a copy on the Pico would be used instead of the frozen one and would use RAM again:

`tinyos.py`, `st7789.py`, `buttons.py`, `filebrowser.py`, `sdcard.py`, `xmb.py`

**Keep** `main.py` and `config.py` (and `games/`, `emulators/`, `data/`). If the console fails to start after deleting, re-upload the six files and tell me, or rebuild with freeze_os off.

Changes to those files now need a new firmware build (edit the copies in `frozen/`). Settings in `config.py`, games and emulators stay normal files.

## RTSP_OS: two ways to install the new version

**A. Without rebuilding (quickest).** Upload the files from `pico_files.zip` to the Pico in Thonny (overwrite): `main.py`, `tinyos.py`, `st7789.py`, `filebrowser.py`, `xmb.py` to the root; `games/*.py`; `emulators/gb.py`, `emulators/gbc.py`. Do **not** overwrite `config.py` or `/data`. Files on the Pico override the frozen ones, so this works with any firmware, but if you have a frozen OS they use RAM again until you do path B.

**B. Rebuild the firmware.** Replace `frozen/` and `manifest.py` in your GitHub repo (the manifest now also freezes `xmb.py`), run the workflow, flash, then delete the six OS files from the Pico root (see section 3) and upload only `main.py`, `games/`, `emulators/`.

## Splash screen with your own image

Convert any picture on your PC (needs Python + Pillow):

    python tools/make_splash.py my_image.png

This writes `splash.raw` (320x240). Copy it to the Pico root (or the SD card root). Without the file a built-in RTSP_OS splash is shown. The splash can be switched off in *Ustawienia > Ekran startowy*.

## XMB menu

L/R: change category (Gry, Game Boy, Pliki, Ustawienia, System). Up/Down: choose an item. A: open. Game Boy lists the `.gb`/`.gbc` files from `/sd/roms`, `/sd` and `/roms`. Polish letters are drawn by `st7789.py` from the plain font, so no font file is needed.

## 4. Install the front-end

Copy `emulators/gb.py` and `emulators/gbc.py` to `/emulators` on the Pico (flash, not the SD card).

## 5. Play

Put `.gb` files on the SD card (for example `/sd/roms`) and open one in the file browser.

- **D-pad, A, B, START** work as on a Game Boy.
- **Hold START alone for 1 second** for the pause menu: press Select, save, scale (1x or 1.5x), frame skip, display clock, palette, overclock, diagnostics, quit. The title shows the current speed in fps (about 60 = full speed).
- For a real Select key, wire a button to a free GPIO and add `"SELECT": 9` (your pin) to `BUTTONS` in `config.py`.
- Battery saves are written next to the ROM as `.sav` files.

## Speed settings

- **Frame skip ON** (default): the emulator still runs at 60 Hz but draws every other frame. This halves display traffic and drawing work and is the easiest way to full speed with the 1.5x picture.
- **Display clock FAST** raises the screen's SPI clock to 62.5 MHz (needs the overclock on). Most ST7789 panels cope; if you see noise or glitches, switch back to NORMAL.
- **Scale 1x** sends 55% less data than 1.5x.
- **Overclock** runs the RP2040 at 250 MHz while a game is open.
- Optional in `config.py`: `GB_SD_BAUD = 16_000_000` is the SD clock used during games (default 16 MHz). Lower it if you get read errors, raise it (up to 25 MHz) for faster ROM bank loading.

## Diagnostics (pause menu)

Send me this screen if a game is still slow:

- **Speed** in fps.
- **ROM reads from SD per second** and **time loading**: big numbers mean the game keeps fetching ROM from the SD card (more RAM for the cache helps).
- **Waiting for screen**: big numbers mean the display link is the limit (use frame skip, fast display clock, or 1x).
- **Cache slots / free RAM**.

## Game Boy Color games

The emulator core only knows the **original Game Boy**. I found no Game Boy Color core for the RP2040, and even the original Game Boy needs about 266 MHz in C to hold 60 fps there.

- **Color-only games** (most late releases) now show a message instead of freezing.
- **Games that work on both** (most early colour releases) run in classic mode.
- **Colour:** the palettes *Color A/B/C* colour the background and the two sprite palettes separately, the way a Game Boy Color tints old games. This is a tint, not the game's own colour graphics.
- ROMs of colour cartridges are larger, so they load more banks from the SD card. If one is slow, check Diagnostics (*ROM bank loads/s*), try `GB_SD_BAUD = 20_000_000` in `config.py`, and give the cache more RAM (RPI_PICO build, frozen OS).

## ROM cache (changed)

The ROM cache now uses **4 KB pages** instead of 16 KB banks (same RAM, 4x more entries, each SD read about 4x cheaper). Games with big ROMs that used to drop to a few fps should improve a lot. This needs a **firmware rebuild** (C module changed) together with the new `emulators/gb.py`; the old `gb.py` will not work with the new firmware and the other way round.

## Direct SD reads (new)

ROM pages are now read straight from the SD card by the C module (the ROM file's sectors are looked up once when the game starts and checked against a normal read). This removes most of the Python driver's overhead. The Diagnostics screen shows *Odczyt SD: bezpośredni* when it is active; *przez Pythona* means it fell back to the old path (e.g. a card formatted with clusters smaller than 4 KB, or a file system problem), which still works, just slower. Try `GB_SD_BAUD = 20_000_000` (or 25 MHz) in `config.py` for faster transfers; go back down if you get read errors.

For the screen wait: pause menu -> *Skala* 1x and *Zegar ekranu* SZYBKI cut the display time a lot.

## Faster emulation and menus (new)

- **Display queue.** Rendered strips of the picture now wait in a queue and a DMA interrupt sends them one after another, so the emulator no longer stalls while the screen is fed, and keeps running into the next frame. The queue size depends on free RAM (Diagnostics shows *Kolejka ekranu*).
- **Emulator core in RAM.** The workflow option `ram_code` (default on) moves the hot Peanut-GB functions from flash to RAM, which usually speeds the CPU emulation up noticeably. It costs about 20 KB of RAM; switch it off if a game complains about memory.
- **Turbo 250 MHz** (Ustawienia, default on): the whole system, including the XMB menu animation, runs at 250 MHz with the display clock at 62.5 MHz (`SPI_BAUD_TURBO` in `config.py` if you want another value). Switch it off if you get glitches.

## Power off / wake (START)

In the XMB menu hold **START for about 4 seconds** (a progress bar appears) to switch the console "off": the SD card is unmounted, the screen and backlight go off and the CPU clock drops to 20 MHz. Hold START for 4 seconds again to wake it up (the splash screen appears). The same is available in *System > Wyłącz (uśpij)*.

A Pico has no real off state, so this is a low-power standby, not a power cut: a battery will still drain slowly (use a power switch for storage). Optional in `config.py`: `POWER_HOLD_MS = 4000` (hold time) and `PIN_BL = <gpio>` if your backlight is wired to a GPIO that can be switched off.

## Limits

- No sound.
- Original Game Boy hardware only (see above).
- Cartridge RAM above 32 KB is not saved.
