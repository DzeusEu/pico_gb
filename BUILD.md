# Game Boy emulator for TinyOS: build and install

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

After flashing, delete these five files from the Pico's root in Thonny, because a copy on the Pico would be used instead of the frozen one and would use RAM again:

`tinyos.py`, `st7789.py`, `buttons.py`, `filebrowser.py`, `sdcard.py`

**Keep** `main.py` and `config.py` (and `games/`, `emulators/`, `data/`). If the console fails to start after deleting, re-upload the five files and tell me, or rebuild with freeze_os off.

Changes to those five files now need a new firmware build (edit the copies in `frozen/`). Settings in `config.py`, games and emulators stay normal files.

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
- **ROM bank loads/s** and **time loading**: big numbers mean the game keeps fetching ROM from the SD card (more RAM for the cache helps).
- **Waiting for screen**: big numbers mean the display link is the limit (use frame skip, fast display clock, or 1x).
- **Cache slots / free RAM**.

## Limits

- No sound.
- Original Game Boy games only; Game Boy Color games that have a monochrome mode run in monochrome, GBC-only games do not work.
- Cartridge RAM above 32 KB is not saved.
