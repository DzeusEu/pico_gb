# Interprets ST7789 SPI traffic (DC pin is tracked via Pin objects) onto a 320x240 canvas.
import numpy as np
import machine
class FakeLCD:
    def __init__(self):
        self.img = np.zeros((240, 320, 3), np.uint8)
        self.pin_dc = None
        self.cmd = None
        self.args = b""
        self.win = [0, 319, 0, 239]
        self.cx = self.cy = 0
        self.pend = b""
        self.rot = 1
    def write(self, data):
        if machine.Pin.reg[12].v == 0:
            for b in data:
                self.cmd = b
                self.args = b""
                if b == 0x2C:
                    self.cx, self.cy = self.win[0], self.win[2]
            return
        if self.cmd == 0x2A or self.cmd == 0x2B:
            self.args += data
            if len(self.args) >= 4:
                a, b = int.from_bytes(self.args[0:2], "big"), int.from_bytes(self.args[2:4], "big")
                if self.cmd == 0x2A: self.win[0], self.win[1] = a, b
                else: self.win[2], self.win[3] = a, b
        elif self.cmd == 0x2C:
            d = self.pend + data
            n = len(d) // 2 * 2
            self.pend = d[n:]
            px = np.frombuffer(d[:n], dtype=">u2")  # big endian on the wire
            self.put(px)
    def put(self, px):
        x0, x1, y0, y1 = self.win
        w = x1 - x0 + 1
        i = 0
        while i < len(px):
            row = min(len(px) - i, x1 - self.cx + 1)
            seg = px[i:i + row]
            r = ((seg >> 11) & 31) << 3
            g = ((seg >> 5) & 63) << 2
            b = (seg & 31) << 3
            if self.cy < 240:
                self.img[self.cy, self.cx:self.cx + row] = np.stack([r, g, b], 1)
            i += row
            self.cx += row
            if self.cx > x1:
                self.cx = x0
                self.cy += 1
                if self.cy > y1:
                    self.cy = y0
