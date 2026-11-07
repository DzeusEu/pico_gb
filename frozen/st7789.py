# Minimal ST7789 driver for MicroPython (no full framebuffer - RAM friendly).
#
# Colors are "byte-swapped RGB565" ints produced by rgb(); they can be passed
# straight to every drawing call here and to framebuf.RGB565 buffers.
import struct
import time
import framebuf


def rgb(r, g, b):
    c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((c & 0xFF) << 8) | (c >> 8)


class ST7789:
    def __init__(self, spi, dc, cs, rst, width=240, height=320,
                 rotation=1, invert=True, bgr=False):
        self.spi = spi
        self.dc = dc
        self.cs = cs
        self.rst = rst
        self.native_w = width
        self.native_h = height
        self.width = width
        self.height = height
        self.rotation = rotation
        self.bgr = bgr
        cs(1)
        dc(0)
        if rst is not None:
            rst(1)
            time.sleep_ms(10)
            rst(0)
            time.sleep_ms(20)
            rst(1)
            time.sleep_ms(120)
        else:
            self._cmd(0x01)
            time.sleep_ms(150)
        self._cmd(0x11)                 # sleep out
        time.sleep_ms(120)
        self._cmd(0x3A, b"\x55")        # 16 bit color
        self.set_rotation(rotation, bgr)
        self.set_invert(invert)
        self._cmd(0x13)                 # normal display mode
        time.sleep_ms(10)
        self._cmd(0x29)                 # display on
        time.sleep_ms(120)
        self.fill(0)

    # ---- low level ----
    def _cmd(self, cmd, data=None):
        self.cs(0)
        self.dc(0)
        self.spi.write(bytes([cmd]))
        if data:
            self.dc(1)
            self.spi.write(data)
        self.cs(1)

    def _begin(self, x0, y0, x1, y1):
        self._cmd(0x2A, struct.pack(">HH", x0, x1))
        self._cmd(0x2B, struct.pack(">HH", y0, y1))
        self.cs(0)
        self.dc(0)
        self.spi.write(b"\x2C")
        self.dc(1)

    def _end(self):
        self.cs(1)

    # ---- configuration ----
    def set_invert(self, on):
        self._cmd(0x21 if on else 0x20)

    def set_rotation(self, rotation, bgr=False):
        self.rotation = rotation & 3
        self.bgr = bgr
        madctl = (0x00, 0x60, 0xC0, 0xA0)[self.rotation]
        if bgr:
            madctl |= 0x08
        self._cmd(0x36, bytes([madctl]))
        if self.rotation & 1:
            self.width, self.height = self.native_h, self.native_w
        else:
            self.width, self.height = self.native_w, self.native_h

    # ---- drawing ----
    def fill_rect(self, x, y, w, h, color):
        if x < 0:
            w += x
            x = 0
        if y < 0:
            h += y
            y = 0
        if x + w > self.width:
            w = self.width - x
        if y + h > self.height:
            h = self.height - y
        if w <= 0 or h <= 0:
            return
        self._begin(x, y, x + w - 1, y + h - 1)
        total = w * h
        n = 1024 if total > 1024 else total
        chunk = struct.pack("<H", color) * n
        write = self.spi.write
        for _ in range(total // n):
            write(chunk)
        rest = total % n
        if rest:
            write(chunk[:rest * 2])
        self._end()

    def fill(self, color):
        self.fill_rect(0, 0, self.width, self.height, color)

    def pixel(self, x, y, color):
        self.fill_rect(x, y, 1, 1, color)

    def hline(self, x, y, w, color):
        self.fill_rect(x, y, w, 1, color)

    def vline(self, x, y, h, color):
        self.fill_rect(x, y, 1, h, color)

    def rect(self, x, y, w, h, color):
        self.fill_rect(x, y, w, 1, color)
        self.fill_rect(x, y + h - 1, w, 1, color)
        self.fill_rect(x, y, 1, h, color)
        self.fill_rect(x + w - 1, y, 1, h, color)

    def blit(self, buf, x, y, w, h):
        """Draw a ready RGB565 (byte-swapped, see rgb()) buffer."""
        if x < 0 or y < 0 or x + w > self.width or y + h > self.height:
            return
        self._begin(x, y, x + w - 1, y + h - 1)
        self.spi.write(buf)
        self._end()

    def text(self, s, x, y, fg, bg, scale=1):
        """Opaque 8x8 text (built-in MicroPython font), optional integer scale."""
        if x < 0 or y < 0 or x >= self.width or y >= self.height:
            return
        maxc = (self.width - x) // (8 * scale)
        s = s[:maxc]
        n = len(s)
        if n == 0 or y + 8 * scale > self.height:
            return
        w = n * 8
        buf = bytearray(w * 16)
        fb = framebuf.FrameBuffer(buf, w, 8, framebuf.RGB565)
        fb.fill(bg)
        fb.text(s, 0, 0, fg)
        if scale == 1:
            self.blit(buf, x, y, w, 8)
            return
        row = bytearray(w * scale * 2)
        self._begin(x, y, x + w * scale - 1, y + 8 * scale - 1)
        for r in range(8):
            base = r * w * 2
            for px in range(w):
                p = buf[base + px * 2: base + px * 2 + 2]
                for k in range(scale):
                    o = (px * scale + k) * 2
                    row[o:o + 2] = p
            for _ in range(scale):
                self.spi.write(row)
        self._end()
