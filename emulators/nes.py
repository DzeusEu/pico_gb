# Nintendo (NES / Famicom) front-end for RTSP_OS. The emulator core is the C module "nesemu",
# which must be built into the firmware - see BUILD.md. Dźwięk: brak (APU nie jest emulowane).
#
# Controls: D-pad, A, B, START as on a NES.
#   Hold START alone for 1 second -> pause menu (Select, save, frame skip, ...).
#   Optional: wire a button as "SELECT" in config.BUTTONS (e.g. "SELECT": 9) for a real Select key.
import gc
import time
import uos
from machine import freq

import config

SELECT_BIT = 128
FRAME_US = 16639            # 60.10 Hz
OC_HZ = 250_000_000
FAST_SPI = 62_500_000

CORE_BYTES = 28 * 1024      # nes_t + SD buffer + work RAM + small tables (the code itself lives outside the Python heap)
RESERVE = 12 * 1024
SPARE = 6 * 1024            # contiguous block released while the pause menu / diagnostics draw text
SLOT_BYTES = 4096 + 8       # one 4 KB ROM cache unit
STRIP_BYTES = 4096          # one display strip (8 lines of 256 pixels)
MIN_SLOTS = 20


def _set_clock(tos, hz):
    freq(hz)
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


def _units(f):
    """Number of 4 KB ROM units (PRG + CHR) from the iNES header."""
    f.seek(0)
    h = f.read(16)
    f.seek(0)
    if len(h) < 16 or h[:4] != b"NES\x1a":
        return 0
    return (h[4] * 16384 + h[5] * 8192) // 4096


def _pick(tos):
    roms = []
    for d in ("/sd/roms", "/sd", "/roms"):
        try:
            for name in sorted(uos.listdir(d)):
                if name.lower().endswith(".nes"):
                    roms.append((name, d + "/" + name))
        except OSError:
            pass
    if not roms:
        tos.message("NES", "Nie znaleziono ROM-ów. Skopiuj pliki .nes do /sd/roms (lub /sd) albo otwórz plik z przeglądarki.", tos.ORANGE)
        return None
    items = [(n[:30], tos.GREEN, "SD" if p.startswith("/sd") else "") for n, p in roms]
    idx, key = tos.menu("ROM-y NES", items, "A:Graj  B:Wstecz")
    return roms[idx][1] if key == "A" and idx >= 0 else None


def run(tos, path=None):
    if path is None:
        path = _pick(tos)
        if path is None:
            return
    try:
        import nesemu
    except ImportError:
        tos.message("NES", "Ten firmware nie ma modułu 'nesemu'. Zbuduj go według BUILD.md i wgraj nowy UF2.", tos.RED)
        return

    saved = tos.load("nes", {})
    st = {
        "oc": saved.get("oc", 1),
        "fskip": saved.get("fskip", 0),      # render every other frame
        "fast": saved.get("fast", 0),        # 62.5 MHz display clock
    }

    tos.clear(tos.BLACK)
    tos.ctext("Wczytywanie...", 110, tos.WHITE, tos.BLACK)
    gc.collect()
    try:
        spare = [bytearray(SPARE)]
    except MemoryError:
        spare = []
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
        units = _units(f)
        if units == 0:
            tos.message("NES", "To nie jest poprawny plik .nes (brak nagłówka iNES).", tos.RED)
            return
        try:
            avail = gc.mem_free() - CORE_BYTES - RESERVE - 2 * units
            want = max(2, min(8, getattr(config, "NES_STRIPS", 2)))
            strips = want
            while strips > 2 and (avail - (strips - 2) * STRIP_BYTES) // SLOT_BYTES < min(units, 32):
                strips -= 1
            slots = (avail - (strips - 2) * STRIP_BYTES) // SLOT_BYTES
            slots = max(MIN_SLOTS, min(slots, units, 120))
        except Exception:
            slots, strips = MIN_SLOTS, 2
        try:
            while True:
                try:
                    emu = nesemu.Emu(f, slots, strips)
                    break
                except MemoryError:
                    gc.collect()
                    if strips > 2:
                        strips -= 1
                    elif slots <= MIN_SLOTS or slots <= 4:
                        break
                    else:
                        slots -= 4
        except ValueError as e:
            tos.message("NES", str(e).replace("mapper", "Mapper").replace("not supported", "nie jest obsługiwany"), tos.RED)
            return
        except RuntimeError as e:
            tos.message("NES", str(e), tos.RED)
            return
        if emu is None:
            need = (CORE_BYTES + MIN_SLOTS * SLOT_BYTES + RESERVE) // 1024
            tos.message("NES", "Za mało wolnej pamięci RAM: wolne %d KB, potrzeba ok. %d KB. Zobacz BUILD.md (zamroź system / zbuduj dla RPI_PICO)." % (gc.mem_free() // 1024, need), tos.RED)
            return

        try:
            slots, strips = emu.info()
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
        title = emu.title() or "NES"
        mapper = emu.mapper()
        _load_save(emu, sav)
        diag = [0, 0, 0, 0, 0, 0, 0]

        def setup():
            emu.sync()
            tos.lcd.spi.init(baudrate=config.SPI_BAUD)
            ox, oy = (tos.W - 256) // 2, (tos.H - 240) // 2
            tos.lcd.fill(tos.BLACK)
            baud = FAST_SPI if st["fast"] else config.SPI_BAUD
            emu.display_hw(config.SPI_ID, config.PIN_DC, config.PIN_CS, ox, oy, 0, baud)
            if tos.sd:
                tos.sd.spi.init(baudrate=getattr(config, "GB_SD_BAUD", 16_000_000), phase=0, polarity=0)
            emu.options(bool(st["fskip"]))

        def remember():
            tos.save("nes", st)

        def set_clock(on):
            nonlocal clocked
            if on and not clocked:
                _set_clock(tos, OC_HZ)
                clocked = True
            elif not on and clocked:
                _set_clock(tos, orig_hz)
                clocked = False

        def pause_menu():
            spare.clear()
            gc.collect()
            try:
                return _pause()
            finally:
                try:
                    spare.append(bytearray(SPARE))
                except MemoryError:
                    pass

        def _pause():
            emu.sync()
            tos.lcd.spi.init(baudrate=config.SPI_BAUD)
            sel = 0
            while True:
                rows = (
                    ("resume", "Wznów", tos.GREEN, ""),
                    ("select", "Wciśnij SELECT", None, ""),
                    ("save", "Zapisz grę", None, "" if has_ram else "brak RAM"),
                    ("fskip", "Pomijanie klatek", None, "WŁ." if st["fskip"] else "WYŁ."),
                    ("fast", "Zegar ekranu", None, "SZYBKI" if st["fast"] else "NORMALNY"),
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
                if what in ("resume", "select"):
                    return what
                if what == "save":
                    if has_ram:
                        ok = _write_save(emu, sav)
                        tos.message("NES", "Zapisano." if ok else "Nie można zapisać pliku.",
                                    tos.GREEN if ok else tos.RED)
                elif what == "fskip":
                    st["fskip"] ^= 1
                elif what == "fast":
                    st["fast"] ^= 1
                elif what == "oc":
                    st["oc"] ^= 1
                    set_clock(bool(st["oc"]))
                elif what == "diag":
                    tos.message("Diagnostyka",
                                "Prędkość: %d fps (60 = pełna)\nOdczyty ROM z SD: %d/s\nCzas ładowania: %d ms/s\nCzekanie na ekran: %d ms/s\nCache ROM: %d z %d jednostek 4 KB\nRysowane klatki: %d/s\nZegar SPI ekranu: %d MHz (peryf. %d)\nKolejka ekranu: %d pasków\nMapper: %d\nWolny RAM: %d KB\nZegar: %d MHz\nOdczyt SD: %s" % (
                                    diag[0], diag[1], diag[2], diag[3], slots, units, diag[4], diag[5], diag[6], strips, mapper,
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

            next_t = time.ticks_add(next_t, FRAME_US)
            d = time.ticks_diff(next_t, time.ticks_us())
            if d > 0:
                time.sleep_us(d)
            elif d < -4 * FRAME_US:
                next_t = time.ticks_us()

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
