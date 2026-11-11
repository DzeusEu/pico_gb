class Pin:
    IN, OUT, PULL_UP = 0, 1, 2
    state = {}
    reg = {}
    def __init__(self, n, mode=0, pull=None, value=None):
        self.n = n
        Pin.reg[n] = self
        self.v = 1 if value is None else value
    def __call__(self, v=None):
        if v is None:
            return self.v
        self.v = v
    def value(self, v=None):
        if v is None:
            return Pin.state.get(self.n, 1)
        self.v = v
class PWM:
    def __init__(self, p): pass
    def freq(self, f): pass
    def duty_u16(self, d): pass
class SPI:
    LCD = None
    def __init__(self, id, **kw):
        self.id = id
    def init(self, **kw): pass
    def write(self, data):
        if SPI.LCD is not None:
            SPI.LCD.write(bytes(data))
def freq(hz=None): return 125000000
def reset(): raise SystemExit("reset")
