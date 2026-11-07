# Button input: active-low buttons with internal pull-ups.
from machine import Pin
import time

UP = 1
DOWN = 2
LEFT = 4
RIGHT = 8
A = 16
B = 32
START = 64
SELECT = 128          # optional extra button: add "SELECT": <gpio> to config.BUTTONS
DIRS = UP | DOWN | LEFT | RIGHT

_ORDER = (("UP", UP), ("DOWN", DOWN), ("LEFT", LEFT), ("RIGHT", RIGHT),
          ("A", A), ("B", B), ("START", START), ("SELECT", SELECT))


class Buttons:
    def __init__(self, mapping):
        self._pins = []
        for name, bit in _ORDER:
            gp = mapping.get(name)
            if gp is not None:
                self._pins.append((Pin(gp, Pin.IN, Pin.PULL_UP), bit))
        self._last = 0
        self._next = 0

    def read(self):
        """Bitmask of buttons currently held down."""
        m = 0
        for pin, bit in self._pins:
            if not pin.value():
                m |= bit
        return m

    def poll(self):
        """Newly pressed buttons since last poll; direction keys auto-repeat."""
        now = time.ticks_ms()
        m = self.read()
        new = m & ~self._last
        if m and m == self._last:
            if (m & DIRS) and time.ticks_diff(now, self._next) >= 0:
                new |= m & DIRS
                self._next = time.ticks_add(now, 90)
        elif new:
            self._next = time.ticks_add(now, 350)
        self._last = m
        return new

    def wait(self):
        """Block until something is pressed; returns the new-press mask."""
        while True:
            k = self.poll()
            if k:
                return k
            time.sleep_ms(12)

    def release(self):
        """Wait until all buttons are released (call when switching screens)."""
        while self.read():
            time.sleep_ms(10)
        self._last = 0
