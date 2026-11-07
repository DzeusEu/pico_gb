# Desktop stand-in for MicroPython's framebuf (RGB565 + MONO_HLSB only). Preview font is approximate.
import numpy as np
from PIL import Image, ImageDraw, ImageFont

RGB565 = 1
MONO_HLSB = 3
_font = {}


def _glyph(ch):
    g = _font.get(ch)
    if g is None:
        f = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 9)
        im = Image.new("L", (8, 8), 0)
        ImageDraw.Draw(im).text((0, -1), ch, font=f, fill=255)
        g = np.array(im) > 110
        _font[ch] = g
    return g


class FrameBuffer:
    def __init__(self, buf, w, h, fmt):
        self.buf, self.w, self.h, self.fmt = buf, w, h, fmt
        if fmt == RGB565:
            self.a = np.frombuffer(buf, dtype=np.uint16).reshape(h, w)  # shares memory (writable bytearray)
        else:
            self.a = np.frombuffer(buf, dtype=np.uint8).reshape(h, (w + 7) // 8)

    def _set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            if self.fmt == RGB565:
                self.a[y, x] = c
            else:
                m = 0x80 >> (x & 7)
                if c:
                    self.a[y, x >> 3] |= m
                else:
                    self.a[y, x >> 3] &= ~m & 0xFF

    def fill(self, c):
        self.fill_rect(0, 0, self.w, self.h, c)

    def pixel(self, x, y, c):
        self._set(x, y, c)

    def fill_rect(self, x, y, w, h, c):
        if self.fmt == RGB565:
            x0, y0 = max(0, x), max(0, y)
            x1, y1 = min(self.w, x + w), min(self.h, y + h)
            if x1 > x0 and y1 > y0:
                self.a[y0:y1, x0:x1] = c
        else:
            for yy in range(y, y + h):
                for xx in range(x, x + w):
                    self._set(xx, yy, c)

    def hline(self, x, y, w, c):
        self.fill_rect(x, y, w, 1, c)

    def vline(self, x, y, h, c):
        self.fill_rect(x, y, 1, h, c)

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            self._set(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    def ellipse(self, cx, cy, xr, yr, c, fill=False):
        for yy in range(-yr, yr + 1):
            for xx in range(-xr, xr + 1):
                v = (xx * xx) / max(1, xr * xr) + (yy * yy) / max(1, yr * yr)
                if (v <= 1.0) if fill else (0.75 <= v <= 1.0):
                    self._set(cx + xx, cy + yy, c)

    def text(self, s, x, y, c=1):
        for i, ch in enumerate(s):
            g = _glyph(ch)
            for gy in range(8):
                for gx in range(8):
                    if g[gy, gx]:
                        self._set(x + 8 * i + gx, y + gy, c)
