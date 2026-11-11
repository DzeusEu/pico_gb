# Game Boy front-end for TinyOS. The emulator core is the C module "gbemu" (Peanut-GB),
# which must be built into the firmware - see BUILD.md.
#
# Controls: D-pad, A, B, START as on a Game Boy.
#   Hold START alone for 1 second -> pause menu (Select, save, scale, frame skip, ...).
#   Optional: wire a button as "SELECT" in config.BUTTONS (e.g. "SELECT": 9) for a real Select key.
import gc
import time
import uos
from machine import freq

import config
from st7789 import rgb

SELECT_BIT = 128
FRAME_US = 16743            # 59.73 Hz
OC_HZ = 250_000_000
FAST_SPI = 62_500_000       # "fast" display clock (needs OC on to be exact; some panels glitch)

def _pal(name, bg, obj0=None, obj1=None):
    """(name, background shades, sprite palette 0 shades, sprite palette 1 shades);
    shades are (r, g, b) tuples, lightest first. Sprites default to the background colours."""
    conv = lambda t: tuple(rgb(*c) for c in t)
    bg = conv(bg)
    o0 = conv(obj0) if obj0 else bg
    o1 = conv(obj1) if obj1 else o0
    return (name, bg + o0 + o1)


# Several palettes colour the background and the two sprite palettes differently, like the
# Game Boy Color did for old black-and-white games.
PALETTES = (
    _pal("Zielona", ((224, 248, 208), (136, 192, 112), (52, 104, 86), (8, 24, 32))),
    _pal("Szara", ((255, 255, 255), (170, 170, 170), (85, 85, 85), (0, 0, 0))),
    _pal("Bursztyn", ((255, 230, 160), (220, 160, 60), (130, 70, 20), (30, 15, 5))),
    _pal("Kolor A",
         ((255, 255, 255), (123, 255, 49), (0, 99, 198), (0, 0, 0)),
         ((255, 255, 255), (255, 133, 132), (148, 58, 58), (0, 0, 0))),
    _pal("Kolor B",
         ((255, 255, 255), (255, 173, 99), (132, 49, 0), (0, 0, 0)),
         ((255, 255, 255), (255, 173, 99), (132, 49, 0), (0, 0, 0)),
         ((255, 255, 255), (99, 165, 255), (0, 0, 255), (0, 0, 0))),
    _pal("Kolor C",
         ((255, 255, 255), (165, 165, 255), (82, 82, 140), (0, 0, 0)),
         ((255, 255, 255), (255, 148, 148), (148, 58, 58), (0, 0, 0)),
         ((255, 255, 255), (148, 255, 148), (49, 132, 49), (0, 0, 0))),
)

# RAM the emulator needs besides the ROM bank cache: Peanut-GB state (~17 KB) + display
# strip buffers (~8 KB) + bank 0 (16 KB); RESERVE is left for Python (menus, saving ...).
CORE_BYTES = 61 * 1024      # includes ~20 KB for emulator code that runs from RAM
RESERVE = 14 * 1024
BANK = 16384
STRIP_BYTES = 3840           # one display strip (8 lines of 240 pixels)
PAGE = 4096                  # ROM cache page size used by the C module
RAM_SIZES = (0, 2048, 8192, 32768, 32768, 32768)   # header code -> bytes (capped at 32 KB)


def _set_clock(tos, hz):
    freq(hz)
    # SPI baud rates are derived from the system clock: re-apply them after a change
    tos.lcd.spi.init(baudrate=tos.spi_baud() if hasattr(tos, "spi_baud") else config.SPI_BAUD)
    if tos.sd:
        tos.sd.spi.init(baudrate=getattr(config, "GB_SD_BAUD", 16_000_000), phase=0, polarity=0)


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


def _plan(f):
    """(number of 16 KB ROM banks, cartridge RAM bytes) from the ROM header."""
    f.seek(0x147)
    h = f.read(3)
    f.seek(0, 2)
    banks = max(2, f.tell() // BANK)
    if h[0] in (5, 6):
        ram = 512
    else:
        ram = RAM_SIZES[h[2]] if h[2] < 6 else 0
    f.seek(0)
    return banks, ram


def _pick(tos):
    """ROM picker used when gb.py is started directly (no file given)."""
    roms = []
    for d in ("/sd/roms", "/sd", "/roms"):
        try:
            for name in sorted(uos.listdir(d)):
                low = name.lower()
                if low.endswith(".gb") or low.endswith(".gbc"):
                    roms.append((name, d + "/" + name))
        except OSError:
            pass
    if not roms:
        tos.message("Game Boy", "Nie znaleziono ROM-ów. Skopiuj pliki .gb do /sd/roms (lub /sd) albo otwórz plik z przeglądarki.", tos.ORANGE)
        return None
    items = [(n[:30], tos.GREEN, "SD" if p.startswith("/sd") else "") for n, p in roms]
    idx, key = tos.menu("ROM-y Game Boy", items, "A:Graj  B:Wstecz")
    return roms[idx][1] if key == "A" and idx >= 0 else None


def run(tos, path=None):
    if path is None:
        path = _pick(tos)
        if path is None:
            return
    try:
        import gbemu
    except ImportError:
        tos.message("Game Boy", "Ten firmware nie ma modułu 'gbemu'. Zbuduj go według BUILD.md i wgraj nowy UF2.", tos.RED)
        return

    saved = tos.load("gb", {})
    st = {
        "scale": saved.get("scale", 1),      # 1 = 1.5x (240x216), 0 = 1x (160x144)
        "pal": saved.get("pal", 0) % len(PALETTES),
        "oc": saved.get("oc", 1),
        "fskip": saved.get("fskip", 1),      # render every other frame (30 fps picture)
        "fast": saved.get("fast", 0),        # 62.5 MHz display clock
    }

    tos.clear(tos.BLACK)
    tos.ctext("Wczytywanie...", 110, tos.WHITE, tos.BLACK)
    gc.collect()
    f = open(path, "rb")
    emu = None
    has_ram = False
    need_save = False
    sav = ""
    orig_hz = freq()
    clocked = False
    slots = 0
    strips = 2
    try:
        # Game Boy Color-only games (header byte 0x143 = 0xC0) cannot run on this DMG emulator
        try:
            f.seek(0x143)
            cgb = f.read(1)
            f.seek(0)
        except Exception:
            cgb = b""
        if cgb and cgb[0] == 0xC0:
            tos.message("Game Boy Color", "Ta gra działa tylko na Game Boy Color, a ten emulator obsługuje oryginalnego Game Boya. Gry oznaczone jako działające na obu (większość wczesnych kolorowych) uruchomią się w trybie klasycznym z kolorowym odcieniem.", tos.ORANGE)
            return
        try:
            banks, ram = _plan(f)
            free = gc.mem_free()
            table = 2 * 4 * banks                    # page table of the C module
            avail = free - CORE_BYTES - ram - RESERVE - table
            # The ROM cache comes first: too few cache pages means constant SD reads (that is what
            # made the first queue version slow). The display queue only gets what is left over
            # above a comfortable cache. Override with GB_STRIPS (2 = no queue) in config.py.
            want = max(2, min(14, getattr(config, "GB_STRIPS", 4)))
            keep = min(60, (banks - 1) * 4)          # cache pages we try to protect
            strips = want
            while strips > 2 and (avail - (strips - 2) * STRIP_BYTES) // (PAGE + 8) < keep:
                strips -= 1
            slots = (avail - (strips - 2) * STRIP_BYTES) // (PAGE + 8)
            slots = max(4, min(slots, (banks - 1) * 4, 250))
        except Exception:
            banks, ram, slots, strips = 2, 0, 16, 2
        try:
            while True:
                try:
                    emu = gbemu.Emu(f, slots, strips)
                    break
                except MemoryError:
                    gc.collect()
                    if strips > 2:
                        strips = max(2, strips - 4)
                    elif slots <= 4:
                        break
                    else:
                        slots -= 4
        except ValueError as e:
            tos.message("Game Boy", str(e), tos.RED)
            return
        if emu is None:
            need = (CORE_BYTES + ram + 4 * PAGE + RESERVE) // 1024
            tos.message("Game Boy", "Za mało wolnej pamięci RAM: wolne %d KB, potrzeba ok. %d KB. Zobacz BUILD.md (zamroź system / zbuduj dla RPI_PICO)." % (gc.mem_free() // 1024, need), tos.RED)
            return

        try:
            slots, strips = emu.info()          # what the C module really got
        except Exception:
            pass
        direct = False
        if path.startswith("/sd") and tos.sd is not None and hasattr(emu, "sd_direct"):
            try:
                direct = emu.sd_direct(getattr(config, "SD_SPI_ID", 0), config.SD_CS, tos.sd.cdv)
            except Exception:
                direct = False
        sav = path.rpartition(".")[0] + ".sav"
        has_ram = emu.ram() is not None
        title = emu.title() or "Game Boy"
        _load_save(emu, sav)
        diag = [0, 0, 0, 0, 0, 0, 0]          # fps, bank loads/s, ms loading/s, ms waiting for display/s

        def setup():
            w, h = tos.W, tos.H
            emu.sync()
            tos.lcd.spi.init(baudrate=config.SPI_BAUD)
            if st["scale"]:
                ox, oy, mode = (w - 240) // 2, (h - 216) // 2, 1
            else:
                ox, oy, mode = (w - 160) // 2, (h - 144) // 2, 0
            tos.lcd.fill(tos.BLACK)
            baud = FAST_SPI if st["fast"] else config.SPI_BAUD
            emu.display_hw(config.SPI_ID, config.PIN_DC, config.PIN_CS, ox, oy, mode, baud)
            if tos.sd:
                # peripheral clock may have changed: re-apply the SD clock
                tos.sd.spi.init(baudrate=getattr(config, "GB_SD_BAUD", 16_000_000), phase=0, polarity=0)
            emu.palette(*PALETTES[st["pal"]][1])
            emu.options(bool(st["fskip"]))

        def remember():
            tos.save("gb", st)

        def set_clock(on):
            nonlocal clocked
            if on and not clocked:
                _set_clock(tos, OC_HZ)
                clocked = True
            elif not on and clocked:
                _set_clock(tos, orig_hz)
                clocked = False

        def pause_menu():
            emu.sync()
            tos.lcd.spi.init(baudrate=config.SPI_BAUD)
            sel = 0
            while True:
                rows = (
                    ("resume", "Wznów", tos.GREEN, ""),
                    ("select", "Wciśnij SELECT", None, ""),
                    ("save", "Zapisz grę", None, "" if has_ram else "brak RAM"),
                    ("scale", "Skala", None, "1.5x" if st["scale"] else "1x"),
                    ("fskip", "Pomijanie klatek", None, "WŁ." if st["fskip"] else "WYŁ."),
                    ("fast", "Zegar ekranu", None, "SZYBKI" if st["fast"] else "NORMALNY"),
                    ("pal", "Paleta", None, PALETTES[st["pal"]][0]),
                    ("oc", "Podkręcenie 250 MHz", None, "WŁ." if st["oc"] else "WYŁ."),
                    ("diag", "Diagnostyka", None, ""),
                    ("quit", "Zakończ grę", tos.ORANGE, ""),
                )
                items = [(r[1], r[2], r[3]) for r in rows]
                idx, key = tos.menu("%s %dfps" % (title[:15], diag[0]), items, "A:Wybierz  B:Wznów", sel)
                if key == "B":
                    return "resume"
                sel = idx
                what = rows[idx][0]
                if what == "resume":
                    return "resume"
                if what == "select":
                    return "select"
                if what == "save":
                    if has_ram:
                        ok = _write_save(emu, sav)
                        tos.message("Game Boy", "Zapisano." if ok else "Nie można zapisać pliku.",
                                    tos.GREEN if ok else tos.RED)
                elif what == "scale":
                    st["scale"] ^= 1
                elif what == "fskip":
                    st["fskip"] ^= 1
                elif what == "fast":
                    st["fast"] ^= 1
                elif what == "pal":
                    st["pal"] = (st["pal"] + 1) % len(PALETTES)
                elif what == "oc":
                    st["oc"] ^= 1
                    set_clock(bool(st["oc"]))
                elif what == "diag":
                    tos.message("Diagnostyka",
                                "Prędkość: %d fps (60 = pełna)\nOdczyty ROM z SD: %d/s\nCzas ładowania: %d ms/s\nCzekanie na ekran: %d ms/s\nCache ROM: %d z %d stron 4 KB\nRysowane klatki: %d/s\nZegar SPI ekranu: %d MHz (peryf. %d)\nKolejka ekranu: %d pasków\nWolny RAM: %d KB\nZegar: %d MHz\nOdczyt SD: %s" % (
                                    diag[0], diag[1], diag[2], diag[3], slots, (banks - 1) * 4, diag[4], diag[5], diag[6], strips,
                                    gc.mem_free() // 1024, freq() // 1000000, "bezpośredni" if direct else "przez Pythona"))
                elif what == "quit":
                    if tos.confirm("Zakończyć grę?"):
                        return "quit"

        set_clock(bool(st["oc"]))
        setup()
        btn = tos.btn
        btn.release()

        next_t = time.ticks_us()
        skipped = 0
        sel_frames = 0
        hold_start = None
        fcount = 0
        t_fps = time.ticks_ms()
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
                    act = pause_menu()
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

            # fps / diagnostics once a second
            fcount += 1
            dt = time.ticks_diff(now, t_fps)
            if dt >= 1000:
                loads, load_us, wait_us, drawn, baud, peri = emu.stats()
                diag[0] = fcount * 1000 // dt
                diag[1] = loads * 1000 // dt
                diag[2] = load_us // dt
                diag[3] = wait_us // dt
                diag[4] = drawn * 1000 // dt
                diag[5] = baud // 1000000
                diag[6] = peri
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
            if emu is not None:
                emu.sync()
                if has_ram and (need_save or emu.dirty()):
                    _write_save(emu, sav)
        except Exception:
            pass
        try:
            if clocked:
                _set_clock(tos, orig_hz)
            else:
                tos.lcd.spi.init(baudrate=tos.spi_baud())
        except Exception:
            pass
        f.close()
        emu = None
        tos.btn.release()
        gc.collect()
