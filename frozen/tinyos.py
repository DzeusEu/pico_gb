# TinyOS - tiny console OS for Raspberry Pi Pico W + ST7789 LCD.
# System object `tos` is handed to games/apps: run(tos) / emulator run(tos, path).
import gc
import io
import json
import sys
import time
import uos
from machine import Pin, PWM, SPI, freq, reset

import config
import buttons
from buttons import Buttons, UP, DOWN, LEFT, RIGHT, A, B, START
from st7789 import ST7789, rgb

VERSION = "0.2"
SD_DIR = "/sd"
HEADER_H = 20
FOOTER_H = 16
ROW_H = 16
ROWS = 12

# files that must never be launched from the browser or deleted by it
PROTECTED = ("main", "boot", "config", "tinyos", "st7789", "buttons", "filebrowser", "sdcard")
TEXT_EXT = ("txt", "md", "json", "cfg", "ini", "csv", "log", "py")


def wrap(text, cols):
    out = []
    for line in text.replace("\r", "").replace("\t", "    ").split("\n"):
        if not line:
            out.append("")
        while line:
            out.append(line[:cols])
            line = line[cols:]
    return out


def ext_of(name):
    if "." not in name:
        return ""
    return name.rpartition(".")[2].lower()


def pretty(fname):
    return fname.rpartition(".")[0].replace("_", " ").title()


class System:
    def __init__(self):
        # palette (byte-swapped RGB565 ints, see st7789.rgb)
        self.BLACK = rgb(0, 0, 0)
        self.WHITE = rgb(255, 255, 255)
        self.GRAY = rgb(130, 130, 140)
        self.DARK = rgb(30, 30, 40)
        self.RED = rgb(230, 60, 60)
        self.GREEN = rgb(60, 220, 100)
        self.BLUE = rgb(70, 120, 255)
        self.YELLOW = rgb(255, 210, 60)
        self.ORANGE = rgb(255, 150, 40)
        self.CYAN = rgb(60, 220, 230)
        self.PURPLE = rgb(180, 90, 230)
        self.BG = rgb(12, 14, 24)
        self.ACCENT = rgb(40, 70, 170)
        self.HILITE = rgb(40, 55, 110)
        self.FOOT = rgb(25, 28, 45)

        # settings (saved in /data/settings.json, defaults from config.py)
        self.s = {"sound": 1, "invert": 1 if config.LCD_INVERT else 0,
                  "bgr": 1 if config.LCD_BGR else 0, "rot": config.LCD_ROTATION}
        saved = self.load("settings", None)
        if isinstance(saved, dict):
            for k in self.s:
                if k in saved:
                    self.s[k] = saved[k]

        spi = SPI(config.SPI_ID, baudrate=config.SPI_BAUD, polarity=0, phase=0,
                  sck=Pin(config.PIN_SCK), mosi=Pin(config.PIN_MOSI))
        rst = Pin(config.PIN_RST, Pin.OUT) if config.PIN_RST is not None else None
        self.lcd = ST7789(spi, Pin(config.PIN_DC, Pin.OUT), Pin(config.PIN_CS, Pin.OUT),
                          rst, config.LCD_W, config.LCD_H, self.s["rot"],
                          bool(self.s["invert"]), bool(self.s["bgr"]))
        self.W = self.lcd.width
        self.H = self.lcd.height
        self.btn = Buttons(config.BUTTONS)
        self.pwm = None
        if config.PIN_BUZZER is not None:
            self.pwm = PWM(Pin(config.PIN_BUZZER))
            self.pwm.duty_u16(0)
        self.sd = None
        self.sd_error = ""
        self.sd_mount()

    # ------------------------------------------------------------ SD card
    def sd_mount(self):
        """(Re)mount the SD card at /sd. Returns True on success; reason in self.sd_error."""
        cs = getattr(config, "SD_CS", None)
        if cs is None:
            self.sd_error = "disabled (no SD_CS in config.py)"
            return False
        self.sd_unmount()
        try:
            import sdcard
            spi = SPI(getattr(config, "SD_SPI_ID", 0), baudrate=400000,
                      polarity=0, phase=0, sck=Pin(config.SD_SCK),
                      mosi=Pin(config.SD_MOSI), miso=Pin(config.SD_MISO))
            card = sdcard.SDCard(spi, Pin(cs, Pin.OUT, value=1),
                                 getattr(config, "SD_BAUD", 10_000_000))
            uos.mount(uos.VfsFat(card), SD_DIR)
            self.sd = card
            self.sd_error = ""
            return True
        except Exception as e:
            self.sd = None
            self.sd_error = str(e) or e.__class__.__name__
            return False

    def sd_unmount(self):
        if self.sd is not None:
            try:
                uos.umount(SD_DIR)
            except Exception:
                pass
            self.sd = None

    def sd_menu(self):
        sel = 0
        while True:
            state = "mounted" if self.sd else "none"
            items = [("Mount / remount", self.GREEN),
                     ("Unmount (safe to remove)", self.ORANGE),
                     ("Card info", self.WHITE)]
            idx, key = self.menu("SD card: " + state, items, "A:Select  B:Back", sel)
            if key == "B":
                return
            sel = idx
            if idx == 0:
                if self.sd_mount():
                    self.message("SD card", "Mounted at /sd", self.GREEN)
                else:
                    self.message("SD card", "Failed: " + self.sd_error, self.RED)
            elif idx == 1:
                if self.sd:
                    self.sd_unmount()
                    self.message("SD card", "Unmounted. You can remove the card.", self.GREEN)
                else:
                    self.message("SD card", "Nothing mounted.")
            else:
                self.message("SD card", self.sd_summary())

    def sd_summary(self):
        if not self.sd:
            return "No card mounted.\n" + (self.sd_error or "")
        try:
            st = uos.statvfs(SD_DIR)
            total = st[0] * st[2] // 1048576
            free = st[0] * st[3] // 1048576
            return "Mounted at /sd\nSize: %d MB\nFree: %d MB" % (total, free)
        except Exception as e:
            return "Card error: %s" % e

    def find_emu(self, ext):
        """Path of the emulator handling .ext files, or None."""
        for d in ("/emulators", SD_DIR + "/emulators"):
            p = "%s/%s.py" % (d, ext)
            try:
                uos.stat(p)
                return p
            except OSError:
                pass
        return None

    # ------------------------------------------------------------ storage
    def load(self, key, default=None):
        try:
            with open("/data/%s.json" % key) as f:
                return json.load(f)
        except Exception:
            return default

    def save(self, key, value):
        try:
            try:
                uos.mkdir("/data")
            except OSError:
                pass
            with open("/data/%s.json" % key, "w") as f:
                json.dump(value, f)
        except Exception:
            pass

    # ------------------------------------------------------------ sound
    def beep(self, hz=880, ms=25):
        if not self.pwm or not self.s["sound"]:
            return
        self.pwm.freq(hz)
        self.pwm.duty_u16(16000)
        time.sleep_ms(ms)
        self.pwm.duty_u16(0)

    # ------------------------------------------------------------ drawing helpers
    def clear(self, color=None):
        self.lcd.fill(self.BG if color is None else color)

    def header(self, title, right=""):
        lcd = self.lcd
        lcd.fill_rect(0, 0, self.W, HEADER_H, self.ACCENT)
        cols = (self.W - 16) // 8
        if right:
            title = title[:cols - len(right) - 1]
            lcd.text(right, self.W - 8 - 8 * len(right), 6, self.WHITE, self.ACCENT)
        lcd.text(title[:cols], 8, 6, self.WHITE, self.ACCENT)

    def footer(self, hint):
        y = self.H - FOOTER_H
        self.lcd.fill_rect(0, y, self.W, FOOTER_H, self.FOOT)
        self.lcd.text(hint[:self.W // 8 - 1], 4, y + 4, self.GRAY, self.FOOT)

    def ctext(self, s, y, fg, bg=None, scale=1):
        bg = self.BG if bg is None else bg
        x = (self.W - len(s) * 8 * scale) // 2
        self.lcd.text(s, max(0, x), y, fg, bg, scale)

    # ------------------------------------------------------------ UI widgets
    def _row(self, item, y, selected):
        if isinstance(item, str):
            label, color, right = item, self.WHITE, ""
        else:
            label = item[0]
            color = item[1] if len(item) > 1 and item[1] is not None else self.WHITE
            right = item[2] if len(item) > 2 else ""
        bg = self.HILITE if selected else self.BG
        self.lcd.fill_rect(0, y, self.W, ROW_H, bg)
        cols = (self.W - 16) // 8
        if right:
            label = label[:max(0, cols - len(right) - 1)]
            self.lcd.text(right, self.W - 8 - 8 * len(right), y + 4, self.GRAY, bg)
        self.lcd.text(label[:cols], 8, y + 4, color, bg)

    def _rows(self, items, top, sel):
        for i in range(ROWS):
            idx = top + i
            y = HEADER_H + 4 + i * ROW_H
            if idx < len(items):
                self._row(items[idx], y, idx == sel)
            else:
                self.lcd.fill_rect(0, y, self.W, ROW_H, self.BG)

    def menu(self, title, items, hint="", selected=0, allow_start=False):
        """Scrolling list. items: str or (label, color, right_text).
        Returns (index, key) where key is 'A', 'B' or 'START'."""
        n = len(items)
        self.clear()
        self.footer(hint)
        if n == 0:
            self.header(title)
            self.ctext("(empty)", 100, self.GRAY)
            while True:
                k = self.btn.wait()
                if k & B:
                    return -1, "B"
                if allow_start and k & START:
                    return -1, "START"
        sel = max(0, min(selected, n - 1))
        top = max(0, sel - ROWS + 1) if sel >= ROWS else 0
        self.header(title, "%d/%d" % (sel + 1, n))
        self._rows(items, top, sel)
        while True:
            k = self.btn.wait()
            old, oldtop = sel, top
            if k & UP:
                sel = (sel - 1) % n
            elif k & DOWN:
                sel = (sel + 1) % n
            elif k & LEFT:
                sel = max(0, sel - ROWS)
            elif k & RIGHT:
                sel = min(n - 1, sel + ROWS)
            elif k & A:
                return sel, "A"
            elif k & B:
                return sel, "B"
            elif k & START and allow_start:
                return sel, "START"
            else:
                continue
            if sel < top:
                top = sel
            elif sel >= top + ROWS:
                top = sel - ROWS + 1
            if top != oldtop:
                self._rows(items, top, sel)
            elif sel != old:
                self._row(items[old], HEADER_H + 4 + (old - top) * ROW_H, False)
                self._row(items[sel], HEADER_H + 4 + (sel - top) * ROW_H, True)
            self.header(title, "%d/%d" % (sel + 1, n))

    def message(self, title, text, color=None):
        cols = (self.W - 16) // 8
        lines = wrap(text, cols)
        self.clear()
        self.header(title)
        y = 28
        for ln in lines[:16]:
            self.lcd.text(ln, 8, y, color or self.WHITE, self.BG)
            y += 12
        self.footer("A/B: OK")
        while not (self.btn.wait() & (A | B)):
            pass

    def confirm(self, text):
        cols = (self.W - 16) // 8
        lines = wrap(text, cols)
        self.clear()
        self.header("Confirm")
        y = 40
        for ln in lines[:10]:
            self.lcd.text(ln, 8, y, self.WHITE, self.BG)
            y += 12
        self.footer("A:Yes  B:No")
        while True:
            k = self.btn.wait()
            if k & A:
                return True
            if k & B:
                return False

    def viewer(self, title, text):
        cols = (self.W - 8) // 8
        lines = wrap(text, cols)
        per = (self.H - HEADER_H - FOOTER_H - 6) // 10
        last = max(0, len(lines) - per)
        pos = 0
        self.clear()
        self.footer("UP/DOWN:Scroll L/R:Page B:Back")
        while True:
            self.header(title, "%d/%d" % (min(pos + per, len(lines)), len(lines)))
            self.lcd.fill_rect(0, HEADER_H + 2, self.W, per * 10 + 4, self.BG)
            for i in range(per):
                if pos + i < len(lines):
                    self.lcd.text(lines[pos + i], 4, HEADER_H + 4 + i * 10,
                                  self.WHITE, self.BG)
            while True:
                k = self.btn.wait()
                np = pos
                if k & UP:
                    np = pos - 1
                elif k & DOWN:
                    np = pos + 1
                elif k & LEFT:
                    np = pos - per
                elif k & RIGHT:
                    np = pos + per
                elif k & (A | B):
                    return
                np = max(0, min(last, np))
                if np != pos:
                    pos = np
                    break

    # ------------------------------------------------------------ running programs
    def run_py(self, path, entry="run", *args):
        """Import path (a .py file) and call entry(tos, *args). Errors are shown, not fatal."""
        d, _, fname = path.rpartition("/")
        name = fname.rpartition(".")[0]
        d = d or "/"
        if name in PROTECTED or name in sys.modules:
            self.message("Cannot run", "'%s' is a system file or clashes with a loaded module." % fname, self.ORANGE)
            return
        added = False
        if d not in sys.path:
            sys.path.insert(0, d)
            added = True
        gc.collect()
        try:
            mod = __import__(name)
            getattr(mod, entry)(self, *args)
        except Exception as e:
            buf = io.StringIO()
            sys.print_exception(e, buf)
            self.btn.release()
            self.viewer("Error: " + fname, buf.getvalue())
        finally:
            sys.modules.pop(name, None)
            if added and d in sys.path:
                sys.path.remove(d)
            if self.pwm:
                self.pwm.duty_u16(0)
            gc.collect()
            self.btn.release()

    def launch_file(self, path):
        """Open a file with the right program: .py runs, other types go to /emulators/<ext>.py"""
        ext = ext_of(path)
        if ext == "py":
            self.run_py(path)
            return True
        if ext:
            emu = self.find_emu(ext)
            if emu is None:
                return False
            self.run_py(emu, "run", path)
            return True
        return False

    # ------------------------------------------------------------ screens
    def splash(self):
        self.clear()
        self.ctext("TINY OS", 70, self.CYAN, scale=4)
        self.ctext("Pico W game console", 125, self.GRAY)
        self.ctext("v" + VERSION, 145, self.GRAY)
        if self.sd:
            self.ctext("SD card: OK", 175, self.GREEN)
        elif getattr(config, "SD_CS", None) is not None:
            self.ctext("SD card: not found", 175, self.ORANGE)
        time.sleep_ms(900 if self.sd or getattr(config, "SD_CS", None) is None else 1500)

    def games(self):
        sel = 0
        while True:
            games = []  # (title, path, where)
            for d, where in (("/games", ""), (SD_DIR + "/games", "SD")):
                try:
                    for f in sorted(uos.listdir(d)):
                        if f.endswith(".py") and not f.startswith("_"):
                            games.append((pretty(f), d + "/" + f, where))
                except OSError:
                    pass
            if not games:
                self.message("Games", "No games found. Copy .py files into /games or /sd/games (see README).", self.ORANGE)
                return
            items = [(g[0], self.GREEN, g[2]) for g in games]
            idx, key = self.menu("Games", items, "A:Play  B:Back", sel)
            if key == "B":
                return
            sel = idx
            self.btn.release()
            self.run_py(games[idx][1])

    def save_settings(self):
        self.save("settings", self.s)

    def color_test(self):
        lcd = self.lcd
        bars = (self.RED, self.GREEN, self.BLUE, self.WHITE, self.BLACK,
                self.YELLOW, self.CYAN, self.PURPLE)
        bw = self.W // len(bars)
        for i, c in enumerate(bars):
            lcd.fill_rect(i * bw, 0, bw, self.H - 60, c)
        for x in range(self.W):
            v = x * 255 // (self.W - 1)
            lcd.fill_rect(x, self.H - 60, 1, 20, rgb(v, v, v))
        lcd.fill_rect(0, self.H - 40, self.W, 40, self.BG)
        lcd.text("Bars: red green blue white black", 4, self.H - 34, self.WHITE, self.BG)
        lcd.text("Wrong colors? Toggle Invert/Order", 4, self.H - 20, self.GRAY, self.BG)
        while not (self.btn.wait() & (A | B)):
            pass

    def settings(self):
        sel = 0
        while True:
            s = self.s
            items = [
                ("Sound", None, "ON" if s["sound"] else "OFF"),
                ("Invert colors", None, "ON" if s["invert"] else "OFF"),
                ("Color order", None, "BGR" if s["bgr"] else "RGB"),
                ("Flip screen 180", None, "YES" if s["rot"] == 3 else "NO"),
                ("Color test", None, ""),
            ]
            idx, key = self.menu("Settings", items, "A:Change  B:Back", sel)
            if key == "B":
                return
            sel = idx
            if idx == 0:
                s["sound"] ^= 1
                self.beep()
            elif idx == 1:
                s["invert"] ^= 1
                self.lcd.set_invert(bool(s["invert"]))
            elif idx == 2:
                s["bgr"] ^= 1
                self.lcd.set_rotation(s["rot"], bool(s["bgr"]))
            elif idx == 3:
                s["rot"] = 1 if s["rot"] == 3 else 3
                self.lcd.set_rotation(s["rot"], bool(s["bgr"]))
            elif idx == 4:
                self.color_test()
            self.save_settings()

    def info(self):
        gc.collect()
        free = gc.mem_free()
        total = free + gc.mem_alloc()
        try:
            st = uos.statvfs("/")
            fs_free = st[0] * st[3] // 1024
            fs_total = st[0] * st[2] // 1024
            fs = "%d / %d KB free" % (fs_free, fs_total)
        except Exception:
            fs = "n/a"
        sd = self.sd_summary().replace("\n", ", ") if self.sd else "none"
        text = "TinyOS v%s\n%s\nCPU: %d MHz\nRAM: %d / %d KB free\nFlash: %s\nSD: %s\nScreen: %dx%d ST7789\n" % (
            VERSION, sys.version.split(";")[0] if ";" in sys.version else sys.version,
            freq() // 1000000, free // 1024, total // 1024, fs, sd, self.W, self.H)
        self.viewer("System info", text)

    def home(self):
        sel = 0
        while True:
            items = [("Games", self.GREEN), ("File browser", self.YELLOW),
                     ("SD card", self.PURPLE, "OK" if self.sd else "none"),
                     ("Settings", self.CYAN), ("System info", self.WHITE),
                     ("Reboot", self.ORANGE)]
            idx, key = self.menu("TinyOS", items, "UP/DOWN:Select  A:OK", sel)
            if key != "A":
                continue
            sel = idx
            self.btn.release()
            if idx == 0:
                self.games()
            elif idx == 1:
                self.browser()
            elif idx == 2:
                self.sd_menu()
            elif idx == 3:
                self.settings()
            elif idx == 4:
                self.info()
            elif idx == 5:
                if self.confirm("Reboot the console?"):
                    self.sd_unmount()
                    reset()

    def browser(self):
        import filebrowser
        try:
            filebrowser.run(self)
        except Exception as e:
            buf = io.StringIO()
            sys.print_exception(e, buf)
            self.viewer("File browser error", buf.getvalue())
        finally:
            sys.modules.pop("filebrowser", None)
            gc.collect()
