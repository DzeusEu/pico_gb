# Game Boy front-end for TinyOS. The emulator core is the C module "gbemu" (Peanut-GB),
# which must be built into the firmware - see BUILD.md.
#
# Controls: D-pad, A, B, START as on a Game Boy.
#   Hold START alone for 1 second -> pause menu (Select, save, scale, palette, overclock, quit).
#   Optional: wire a button as "SELECT" in config.BUTTONS (e.g. "SELECT": 9) for a real Select key.
import gc
import time
from machine import freq

import config
from st7789 import rgb

SELECT_BIT = 128
FRAME_US = 16743            # 59.73 Hz
OC_HZ = 250_000_000

PALETTES = (
    ("Green", (rgb(224, 248, 208), rgb(136, 192, 112), rgb(52, 104, 86), rgb(8, 24, 32))),
    ("Gray", (rgb(255, 255, 255), rgb(170, 170, 170), rgb(85, 85, 85), rgb(0, 0, 0))),
    ("Amber", (rgb(255, 230, 160), rgb(220, 160, 60), rgb(130, 70, 20), rgb(30, 15, 5))),
)


def _set_clock(tos, hz):
    freq(hz)
    # SPI baud rates are derived from the system clock: re-apply them after a change
    tos.lcd.spi.init(baudrate=config.SPI_BAUD)
    if tos.sd:
        tos.sd.spi.init(baudrate=getattr(config, "SD_BAUD", 10_000_000), phase=0, polarity=0)


def _load_save(emu, sav):
    ram = emu.ram()
    if ram is None:
        return
    try:
        with open(sav, "rb") as f:
            f.readinto(ram)
    except OSError:
        pass


def _write_save(emu, sav):
    ram = emu.ram()
    if ram is None:
        return True
    try:
        with open(sav, "wb") as f:
            f.write(ram)
        return True
    except OSError:
        return False


def run(tos, path):
    try:
        import gbemu
    except ImportError:
        tos.message("Game Boy", "This firmware has no 'gbemu' module. Build it as described in BUILD.md and flash the new UF2.", tos.RED)
        return

    saved = tos.load("gb", {})
    st = {
        "scale": saved.get("scale", 1),      # 1 = 1.5x (240x216), 0 = 1x (160x144)
        "pal": saved.get("pal", 0) % len(PALETTES),
        "oc": saved.get("oc", 1),
    }

    tos.clear(tos.BLACK)
    tos.ctext("Loading...", 110, tos.WHITE, tos.BLACK)
    gc.collect()
    f = open(path, "rb")
    emu = None
    orig_hz = freq()
    clocked = False
    try:
        try:
            for slots in (4, 3, 2, 1):
                try:
                    emu = gbemu.Emu(f, slots)
                    break
                except MemoryError:
                    gc.collect()
        except ValueError as e:
            tos.message("Game Boy", str(e), tos.RED)
            return
        if emu is None:
            tos.message("Game Boy", "Not enough free RAM to start the emulator.", tos.RED)
            return

        sav = path.rpartition(".")[0] + ".sav"
        has_ram = emu.ram() is not None
        title = emu.title() or "Game Boy"
        _load_save(emu, sav)

        def setup():
            w, h = tos.W, tos.H
            if st["scale"]:
                ox, oy, mode = (w - 240) // 2, (h - 216) // 2, 1
            else:
                ox, oy, mode = (w - 160) // 2, (h - 144) // 2, 0
            tos.lcd.fill(tos.BLACK)
            emu.display(tos.lcd.blit, ox, oy, mode)
            emu.palette(*PALETTES[st["pal"]][1])

        def remember():
            tos.save("gb", st)

        def pause_menu(fps):
            sel = 0
            while True:
                items = [
                    ("Resume", tos.GREEN),
                    ("Press SELECT", None),
                    ("Save game", None, "" if has_ram else "no RAM"),
                    ("Scale", None, "1.5x" if st["scale"] else "1x"),
                    ("Palette", None, PALETTES[st["pal"]][0]),
                    ("Overclock 250 MHz", None, "ON" if st["oc"] else "OFF"),
                    ("Quit game", tos.ORANGE),
                ]
                idx, key = tos.menu("%s  %d fps" % (title[:14], fps), items, "A:Select  B:Resume", sel)
                if key == "B" or idx == 0:
                    return "resume"
                sel = idx
                if idx == 1:
                    return "select"
                if idx == 2:
                    if has_ram:
                        ok = _write_save(emu, sav)
                        tos.message("Game Boy", "Saved." if ok else "Could not write the save file.",
                                    tos.GREEN if ok else tos.RED)
                elif idx == 3:
                    st["scale"] ^= 1
                elif idx == 4:
                    st["pal"] = (st["pal"] + 1) % len(PALETTES)
                elif idx == 5:
                    st["oc"] ^= 1
                    nonlocal_clock(bool(st["oc"]))
                elif idx == 6:
                    if tos.confirm("Quit the game?"):
                        return "quit"

        def nonlocal_clock(on):
            nonlocal clocked
            if on and not clocked:
                _set_clock(tos, OC_HZ)
                clocked = True
            elif not on and clocked:
                _set_clock(tos, orig_hz)
                clocked = False

        setup()
        nonlocal_clock(bool(st["oc"]))
        btn = tos.btn
        btn.release()

        next_t = time.ticks_us()
        skipped = 0
        sel_frames = 0
        hold_start = None
        fps = 0
        fcount = 0
        t_fps = time.ticks_ms()
        need_save = False
        last_change = 0
        n = 0
        while True:
            m = btn.read()
            now = time.ticks_ms()

            # hold START alone for 1 s -> pause menu
            if m == 64:
                if hold_start is None:
                    hold_start = now
                elif time.ticks_diff(now, hold_start) > 1000:
                    hold_start = None
                    act = pause_menu(fps)
                    remember()
                    if act == "quit":
                        return
                    if act == "select":
                        sel_frames = 8
                    btn.release()
                    setup()
                    next_t = time.ticks_us()
                    skipped = 0
                    continue
            else:
                hold_start = None

            if sel_frames:
                m |= SELECT_BIT
                sel_frames -= 1

            behind = time.ticks_diff(time.ticks_us(), next_t)
            draw = not (behind > FRAME_US and skipped < 2)
            emu.frame(m, draw)
            skipped = 0 if draw else skipped + 1

            # frame pacing
            next_t = time.ticks_add(next_t, FRAME_US)
            d = time.ticks_diff(next_t, time.ticks_us())
            if d > 0:
                time.sleep_us(d)
            elif d < -4 * FRAME_US:
                next_t = time.ticks_us()

            # fps counter
            fcount += 1
            if time.ticks_diff(now, t_fps) >= 1000:
                fps = fcount * 1000 // time.ticks_diff(now, t_fps)
                fcount = 0
                t_fps = now

            # autosave battery RAM a few seconds after the game last wrote to it
            n += 1
            if has_ram and n >= 30:
                n = 0
                if emu.dirty():
                    need_save = True
                    last_change = now
                if need_save and time.ticks_diff(now, last_change) > 3000:
                    _write_save(emu, sav)
                    need_save = False
                    next_t = time.ticks_us()
    finally:
        try:
            if emu is not None and has_ram and (need_save or emu.dirty()):
                _write_save(emu, sav)
        except Exception:
            pass
        if clocked:
            _set_clock(tos, orig_hz)
        f.close()
        emu = None
        tos.btn.release()
        gc.collect()
