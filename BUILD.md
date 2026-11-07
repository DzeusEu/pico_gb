# Game Boy emulator for TinyOS: build and install

The emulator core (Peanut-GB, MIT license) is C code, so it has to be compiled **into the MicroPython firmware**. This package contains everything except the Peanut-GB header, which the build downloads for you. Nothing here has been compiled or run on hardware yet, so expect to iterate on the first build.

## 1. Build the firmware (easiest: GitHub, nothing to install)

1. Make a free GitHub account and create a new **private** repository.
2. Upload the *contents* of this folder to it (Add file, Upload files; drag in `usermod`, `emulators`, `BUILD.md` and the hidden `.github` folder).
3. Open the **Actions** tab, choose **Build TinyOS firmware**, click **Run workflow**.
4. After a few minutes (about 5 to 15) download the artifact `tinyos-picow-firmware`; it contains `firmware.uf2`.

If the run fails, open the failed step and send me the last 30 lines of its log.

## 2. Build locally instead (Linux, WSL or macOS)

```
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential python3 curl git
git clone --depth 1 --branch v1.24.1 https://github.com/micropython/micropython.git
curl -fL -o usermod/gbemu/peanut_gb.h https://raw.githubusercontent.com/deltabeard/Peanut-GB/v1.2.0/peanut_gb.h
make -C micropython/mpy-cross
make -C micropython/ports/rp2 BOARD=RPI_PICO_W submodules
make -C micropython/ports/rp2 BOARD=RPI_PICO_W USER_C_MODULES=$PWD/usermod/micropython.cmake
```

The result is `micropython/ports/rp2/build-RPI_PICO_W/firmware.uf2`.

## 3. Flash it

Hold BOOTSEL, plug in the Pico W, copy `firmware.uf2` to the `RPI-RP2` drive. The board reboots by itself. Your files on the Pico normally survive a firmware update, but back them up first to be safe. Then check in Thonny's Shell: `import gbemu` should work.

## 4. Install the front-end

Copy `emulators/gb.py` and `emulators/gbc.py` to `/emulators` on the Pico (flash, not the SD card). Also replace `buttons.py` on the Pico with the new one from the main project folder (it adds an optional Select button).

## 5. Play

Put `.gb` files on the SD card (for example in `/sd/roms`) and open one in the file browser.

- **D-pad, A, B, START** work as on a Game Boy.
- **Hold START alone for 1 second** opens the pause menu: press Select, save, scale (1x or 1.5x), palette, overclock, quit.
- For a real Select key, wire a button to a free GPIO and add `"SELECT": 9` (your pin) to `BUTTONS` in `config.py`.
- Battery saves are written next to the ROM as `.sav` files, automatically a few seconds after the game writes to them and when you quit.
- Overclocking to 250 MHz is ON by default (needed for full speed in most games) and is restored when you leave the game. Turn it off in the pause menu if you see instability.
- The pause menu title shows the current fps; about 60 means full speed.

## Limits

- No sound.
- Original Game Boy games only; Game Boy Color games that have a monochrome mode run in monochrome, GBC-only games do not work.
- Cartridge RAM above 32 KB is not saved.
- Speed varies by game. If it is too slow, the pause menu shows the fps so we can tune things like the number of ROM bank cache slots (the `slots` value in `gb.py`).
