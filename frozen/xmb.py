# RTSP_OS - ekran glowny w stylu XMB (XrossMediaBar) z PSP.
#
# Cala scena (tlo z falami, ikony kategorii, lista pozycji) jest skladana z poziomych
# pasow 320x48 w jednym malym buforze RAM (30 KB) i wysylana na ekran pas po pasie.
# Dzieki temu animacje sa plynne bez pelnego bufora ramki (150 KB, ktorego nie ma).
import config
import framebuf
import gc
import io
import math
import sys
import time

from buttons import UP, DOWN, LEFT, RIGHT, A, START
from st7789 import rgb, pl_text

STRIP = 48          # rows per strip
BAND = 4            # rows per background colour band
CAT_X0 = 70         # x of the selected category icon
CAT_DX = 76         # distance between category icons
CAT_Y = 56          # icon row centre
LABEL_Y = 86        # category name
LIST_TOP = 112      # items are not drawn above this line
SEL_Y = 122         # y of the selected item
ROW_H = 26
TEXT_X = 94

NAMES = ("Gry", "Emulatory", "Pliki", "Ustawienia", "System")


# ---------------------------------------------------------------- icon drawing
def _rrect(fb, x, y, w, h, r, c):
    if r > w // 2:
        r = w // 2
    if r > h // 2:
        r = h // 2
    if r < 1:
        fb.fill_rect(x, y, w, h, c)
        return
    fb.fill_rect(x + r, y, w - 2 * r, h, c)
    fb.fill_rect(x, y + r, w, h - 2 * r, c)
    fb.ellipse(x + r, y + r, r, r, c, True)
    fb.ellipse(x + w - r - 1, y + r, r, r, c, True)
    fb.ellipse(x + r, y + h - r - 1, r, r, c, True)
    fb.ellipse(x + w - r - 1, y + h - r - 1, r, r, c, True)


def _cross(fb, x, y, u, c):
    fb.fill_rect(x - 3 * u, y - u, 6 * u, 2 * u, c)
    fb.fill_rect(x - u, y - 3 * u, 2 * u, 6 * u, c)


def draw_icon(fb, kind, cx, cy, s, col, dark):
    """Category icon inside an s x s box centred on (cx, cy). Drawn in `col`, details in `dark`."""
    u = max(1, s // 14)
    if kind == 0:  # gamepad
        w = s
        h = s * 11 // 20
        x = cx - w // 2
        y = cy - h // 2
        _rrect(fb, x, y, w, h, h // 2, col)
        _cross(fb, x + w * 28 // 100, cy, u, dark)
        fb.ellipse(x + w * 68 // 100, cy + u, u, u, dark, True)
        fb.ellipse(x + w * 80 // 100, cy - u, u, u, dark, True)
    elif kind == 1:  # handheld
        w = s * 11 // 16
        h = s
        x = cx - w // 2
        y = cy - h // 2
        _rrect(fb, x, y, w, h, max(2, s // 8), col)
        fb.fill_rect(x + w // 8, y + h // 9, w - w // 4, h * 4 // 10, dark)
        dy = y + h * 70 // 100
        _cross(fb, x + w * 28 // 100, dy, max(1, u * 3 // 4), dark)
        fb.ellipse(x + w * 66 // 100, dy + u, max(1, u * 3 // 4), max(1, u * 3 // 4), dark, True)
        fb.ellipse(x + w * 80 // 100, dy - u, max(1, u * 3 // 4), max(1, u * 3 // 4), dark, True)
    elif kind == 2:  # folder
        w = s
        h = s * 3 // 4
        x = cx - w // 2
        y = cy - h // 2
        fb.fill_rect(x, y, w * 2 // 5, h // 4, col)
        _rrect(fb, x, y + h // 6, w, h - h // 6, max(2, s // 10), col)
        fb.hline(x + 3, y + h // 6 + 3, w - 6, dark)
    elif kind == 3:  # gear
        rad = s * 40 // 100
        t = max(2, s // 9)
        for k in range(8):
            a = k * 0.7853982
            px = cx + int(rad * math.cos(a))
            py = cy + int(rad * math.sin(a))
            fb.fill_rect(px - t // 2, py - t // 2, t, t, col)
        fb.ellipse(cx, cy, s * 33 // 100, s * 33 // 100, col, True)
        fb.ellipse(cx, cy, s * 14 // 100, s * 14 // 100, dark, True)
    else:  # power
        r = s * 42 // 100
        fb.ellipse(cx, cy, r, r, col, True)
        fb.ellipse(cx, cy, s * 24 // 100, s * 24 // 100, dark, True)
        fb.ellipse(cx, cy, s * 16 // 100, s * 16 // 100, col, True)
        bw = max(2, s // 10)
        fb.fill_rect(cx - bw, cy - s * 34 // 100, 2 * bw, s * 18 // 100, col)
        fb.fill_rect(cx - bw // 2, cy - s * 30 // 100, bw, s * 28 // 100, dark)


# ---------------------------------------------------------------- the menu
class XMB:
    def __init__(self, tos):
        self.tos = tos
        self.W = tos.W
        self.H = tos.H
        self.buf = None
        self.fb = None
        self.items = {}
        self.sel = [0] * len(NAMES)
        self.cat = getattr(tos, "xmb_cat", 0)
        self._colours()

    def _colours(self):
        n = self.H // BAND
        self.nb = n
        self.c1, self.c2, self.c3, self.hl, self.barc = [], [], [], [], []
        self.xb, self.xb2 = [], []
        for i in range(n):
            t = i / (n - 1)
            r = int(8 + 34 * t)
            g = int(16 + 80 * t)
            b = int(60 + 118 * t)
            self.c1.append(rgb(r, g, b))
            self.c2.append(rgb(min(255, r + 10), min(255, g + 14), min(255, b + 18)))
            self.c3.append(rgb(min(255, r + 22), min(255, g + 30), min(255, b + 34)))
            self.hl.append(rgb(min(255, r + 70), min(255, g + 90), min(255, b + 90)))
            self.barc.append(rgb(min(255, r + 42), min(255, g + 58), min(255, b + 64)))
            x = int(30 + 250 * t + 26 * math.sin(i * 0.19))
            self.xb.append(x)
            self.xb2.append(x + 64 + int(16 * math.sin(i * 0.11 + 1.3)))
        self.white = rgb(255, 255, 255)
        self.dark = rgb(12, 26, 74)
        self.dim_text = rgb(176, 196, 238)
        self.green = rgb(120, 255, 150)
        self.line = rgb(110, 150, 215)

    # ------------------------------------------------------------ memory
    def alloc(self):
        if self.buf is None:
            gc.collect()
            self.buf = bytearray(self.W * STRIP * 2)
            self.fb = framebuf.FrameBuffer(self.buf, self.W, STRIP, framebuf.RGB565)

    def free(self):
        self.fb = None
        self.buf = None
        gc.collect()

    # ------------------------------------------------------------ content
    def get_items(self, cat):
        it = self.items.get(cat)
        if it is None:
            it = self.build(cat)
            self.items[cat] = it
        return it

    def build(self, cat):
        tos = self.tos
        out = []
        if cat == 0:
            for title, path, where in tos.list_games():
                out.append((title, (lambda p=path: tos.run_py(p)), where))
            if not out:
                out.append(("Brak gier", (lambda: tos.message("Gry", "Nie znaleziono gier. Skopiuj pliki .py do /games lub /sd/games.", tos.ORANGE)), ""))
        elif cat == 1:
            for title, path, where in tos.list_roms():
                out.append((title, (lambda p=path: self.play_rom(p)), where))
            if not out:
                out.append(("Brak ROM-ów", (lambda: tos.message("Emulatory", "Nie znaleziono plików .gb, .gbc ani .nes. Skopiuj je do /sd/roms na karcie SD.", tos.ORANGE)), ""))
        elif cat == 2:
            out.append(("Pamięć wewnętrzna", (lambda: tos.browser("/")), ""))
            out.append(("Karta SD", self.open_sd, "" if tos.sd else "brak"))
        elif cat == 3:
            s = tos.s
            onoff = lambda v: "WŁ." if v else "WYŁ."
            out.append(("Dźwięk", (lambda: tos.toggle_setting("sound")), onoff(s["sound"])))
            out.append(("Głośność", (lambda: tos.toggle_setting("vol")), "%d/3" % s.get("vol", 2)))
            out.append(("Odwróć kolory", (lambda: tos.toggle_setting("invert")), onoff(s["invert"])))
            out.append(("Kolejność kolorów", (lambda: tos.toggle_setting("bgr")), "BGR" if s["bgr"] else "RGB"))
            out.append(("Obróć ekran", (lambda: tos.toggle_setting("rot")), "TAK" if s["rot"] == 3 else "NIE"))
            out.append(("Turbo 250 MHz", (lambda: tos.toggle_setting("turbo")), onoff(s.get("turbo", 1))))
            out.append(("Ekran startowy", (lambda: tos.toggle_setting("splash")), onoff(s.get("splash", 1))))
            out.append(("Test kolorów", tos.color_test, ""))
        else:
            out.append(("Informacje o systemie", tos.info, ""))
            out.append(("Karta SD", tos.sd_menu, "OK" if tos.sd else "brak"))
            out.append(("Uruchom ponownie", tos.reboot, ""))
            out.append(("Wyłącz (uśpij)", tos.power_off, "START"))
        return out

    def play_rom(self, path):
        if not self.tos.launch_file(path):
            self.tos.message("Emulatory", "Brak emulatora dla tego pliku. Skopiuj gb.py / nes.py do /emulators.", self.tos.ORANGE)

    def open_sd(self):
        if self.tos.sd:
            self.tos.browser("/sd")
        else:
            self.tos.message("Karta SD", "Brak zamontowanej karty SD.", self.tos.ORANGE)

    def clamp_sel(self):
        for c in range(len(NAMES)):
            if c in self.items:
                n = len(self.items[c])
                if self.sel[c] > n - 1:
                    self.sel[c] = max(0, n - 1)

    # ------------------------------------------------------------ drawing
    def mix(self, f):
        """Icon/text colour: f = 0 dim ... 1 bright."""
        return rgb(int(150 + 105 * f), int(172 + 83 * f), int(222 + 33 * f))

    def bg(self, fb, y0):
        W = self.W
        for i in range(y0 // BAND, (y0 + STRIP) // BAND):
            ys = i * BAND - y0
            a = self.xb[i]
            b = self.xb2[i]
            fb.fill_rect(0, ys, a, BAND, self.c1[i])
            fb.fill_rect(a, ys, 3, BAND, self.hl[i])
            fb.fill_rect(a + 3, ys, b - a - 3, BAND, self.c2[i])
            fb.fill_rect(b, ys, 2, BAND, self.hl[i])
            fb.fill_rect(b + 2, ys, W - b - 2, BAND, self.c3[i])

    def bold(self, fb, s, x, y, col):
        pl_text(fb, s, x, y, col)
        pl_text(fb, s, x + 1, y, col)

    def header(self, fb):
        self.bold(fb, "RTSP_OS", 10, 6, self.white)
        if self.tos.sd:
            pl_text(fb, "SD", self.W - 26, 6, self.green)
        fb.hline(0, 19, self.W, self.line)

    def cats(self, fb, y0, pos):
        if y0 > CAT_Y + 26 or y0 + STRIP < CAT_Y - 26:
            return
        cy = CAT_Y - y0
        for i in range(len(NAMES)):
            d = abs(i - pos)
            cx = int(CAT_X0 + CAT_DX * (i - pos))
            if cx < -30 or cx > self.W + 30:
                continue
            near = 1.0 - d if d < 1.0 else 0.0
            s = int(28 + 16 * near)
            draw_icon(fb, i, cx, cy, s, self.mix(near), self.dark)

    def label(self, fb, y0, pos):
        if not (y0 <= LABEL_Y < y0 + STRIP):
            return
        cat = int(pos + 0.5)
        if abs(pos - cat) > 0.3:
            return
        name = NAMES[cat]
        cx = int(CAT_X0 + CAT_DX * (cat - pos))
        self.bold(fb, name, cx - 4 * len(name), LABEL_Y - y0, self.white)

    def bar(self, fb, y0, y, x1, w):
        top = y - 4
        bot = y + 16
        for i in range(max(0, top // BAND), min(self.nb, (bot + BAND - 1) // BAND)):
            a = max(top, i * BAND)
            b = min(bot, i * BAND + BAND)
            fb.fill_rect(x1, a - y0, w, b - a, self.barc[i])
        fb.fill_rect(x1, top - y0, 2, bot - top, self.white)

    def list(self, fb, y0, items, ipos):
        W = self.W
        isel = int(ipos + 0.5)
        for k in range(len(items)):
            y = int(SEL_Y + ROW_H * (k - ipos))
            if y < LIST_TOP or y > self.H - 28:
                continue
            if y + 16 <= y0 or y - 4 >= y0 + STRIP:
                continue
            label, _, right = items[k]
            ry = y - y0
            if k == isel:
                self.bar(fb, y0, y, 40, W - 52)
                fb.ellipse(CAT_X0, ry + 6, 4, 4, self.white, True)
                self.bold(fb, label, TEXT_X, ry + 2, self.white)
                if right:
                    self.bold(fb, right, W - 20 - 8 * len(right), ry + 2, self.white)
            else:
                fb.ellipse(CAT_X0, ry + 6, 2, 2, self.dim_text, True)
                pl_text(fb, label, TEXT_X, ry + 2, self.dim_text)
                if right:
                    pl_text(fb, right, W - 20 - 8 * len(right), ry + 2, self.dim_text)

    def footer(self, fb, y0, n, isel):
        y = self.H - 14 - y0
        pl_text(fb, "L/P:dział G/D:pozycja A:OK", 8, y, self.dim_text)
        if n:
            c = "%d/%d" % (isel + 1, n)
            pl_text(fb, c, self.W - 8 - 8 * len(c), y, self.dim_text)

    def render(self, pos, ipos, ymin=0):
        tos = self.tos
        cat = int(pos + 0.5)
        items = self.get_items(cat)
        fb = self.fb
        for y0 in range(0, self.H, STRIP):
            if y0 + STRIP <= ymin:
                continue
            self.bg(fb, y0)
            if y0 == 0:
                self.header(fb)
            self.cats(fb, y0, pos)
            self.label(fb, y0, pos)
            self.list(fb, y0, items, ipos)
            if y0 + STRIP >= self.H:
                self.footer(fb, y0, len(items), int(ipos + 0.5))
            tos.lcd.blit(self.buf, 0, y0, self.W, STRIP)

    # ------------------------------------------------------------ actions
    def activate(self):
        tos = self.tos
        items = self.get_items(self.cat)
        if not items:
            return
        action = items[self.sel[self.cat]][1]
        self.free()  # give the RAM back to games / emulators
        try:
            action()
        except Exception as e:
            b = io.StringIO()
            sys.print_exception(e, b)
            tos.btn.release()
            tos.viewer("Błąd", b.getvalue())
        tos.btn.release()
        self.items.clear()  # lists may have changed (SD card, settings, games ...)
        self.alloc()
        self.get_items(self.cat)
        self.clamp_sel()

    def hold_bar(self, dt, hold):
        """Progress bar while START is held (draws straight onto the screen)."""
        tos = self.tos
        w = self.W - 80
        y = self.H - 34
        fill = min(w, w * dt // hold)
        tos.lcd.fill_rect(38, y - 2, w + 4, 22, self.dark)
        tos.lcd.fill_rect(40, y + 12, w, 4, rgb(60, 80, 130))
        tos.lcd.fill_rect(40, y + 12, fill, 4, self.white)
        tos.lcd.text("Trzymaj START: wyłączanie", 40, y, self.white, self.dark)

    def power(self):
        self.free()
        self.tos.power_off()
        self.tos.btn.release()
        self.alloc()

    def move(self, d):
        n = len(self.get_items(self.cat))
        self.sel[self.cat] = max(0, min(n - 1, self.sel[self.cat] + d))

    def loop(self):
        tos = self.tos
        btn = tos.btn
        count = len(NAMES)
        self.alloc()
        self.get_items(self.cat)
        self.clamp_sel()
        pos = float(self.cat)
        disp = self.cat
        ipos = float(self.sel[disp])
        self.render(pos, ipos)
        btn.release()
        hold = getattr(config, "POWER_HOLD_MS", 4000)
        hold0 = None
        bar = False
        while True:
            if btn.read() & START:
                now = time.ticks_ms()
                if hold0 is None:
                    hold0 = now
                dt = time.ticks_diff(now, hold0)
                if dt >= hold:
                    self.power()
                    hold0 = None
                    bar = False
                    pos = float(self.cat)
                    disp = self.cat
                    ipos = float(self.sel[disp])
                    self.render(pos, ipos)
                    continue
                if dt > 700:
                    self.hold_bar(dt, hold)
                    bar = True
            elif hold0 is not None:
                hold0 = None
                if bar:
                    bar = False
                    self.render(pos, ipos)
            k = btn.poll()
            if k & LEFT and self.cat > 0:
                self.cat -= 1
            elif k & RIGHT and self.cat < count - 1:
                self.cat += 1
            elif k & UP:
                self.move(-1)
            elif k & DOWN:
                self.move(1)
            elif k & A:
                self.activate()
                tos.xmb_cat = self.cat
                pos = float(self.cat)
                disp = self.cat
                ipos = float(self.sel[disp])
                self.render(pos, ipos)
                continue
            tos.xmb_cat = self.cat

            horiz = False
            tpos = float(self.cat)
            if pos != tpos:
                d = tpos - pos
                pos = tpos if abs(d) < 0.04 else pos + d * 0.55
                horiz = True
            nd = int(pos + 0.5)
            if nd != disp:
                disp = nd
                ipos = float(self.sel[disp])
            vert = False
            tip = float(self.sel[disp])
            if ipos != tip:
                d = tip - ipos
                ipos = tip if abs(d) < 0.04 else ipos + d * 0.6
                vert = True
            if horiz:
                self.render(pos, ipos)
            elif vert:
                self.render(pos, ipos, LIST_TOP)
            else:
                time.sleep_ms(15)


def run(tos):
    XMB(tos).loop()
