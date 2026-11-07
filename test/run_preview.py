import sys, os, time, gc, io
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path[:0] = [os.path.join(ROOT, "frozen"), os.path.join(HERE, "cfg"), os.path.join(HERE, "shim")]
# MicroPython compat
time.sleep_ms = lambda ms: None
time.ticks_ms = lambda: int(time.monotonic() * 1000)
time.ticks_diff = lambda a, b: a - b
time.ticks_add = lambda a, b: a + b
gc.mem_free = lambda: 100000
gc.mem_alloc = lambda: 60000
import traceback
sys.print_exception = lambda e, f=sys.stdout: traceback.print_exception(type(e), e, e.__traceback__, file=f)
import numpy as np
from PIL import Image
import machine, fakelcd
lcd = fakelcd.FakeLCD()
machine.SPI.LCD = lcd
import config
import tinyos
tos = tinyos.System()
lcd.pin_dc = machine.Pin.reg[config.PIN_DC]
tos.sd = object()  # pretend an SD card is present
tos.btn.release = lambda: None
os.makedirs(os.path.join(HERE, "out"), exist_ok=True)

def shot(name, scale=2):
    im = Image.fromarray(lcd.img).resize((320 * scale, 240 * scale), Image.NEAREST)
    im.save(os.path.join(HERE, "out", name + ".png"))

class Done(Exception): pass

def drive(script):
    seq = list(script)
    state = {"idle": 0}
    def poll():
        if state["idle"] > 0:
            state["idle"] -= 1
            return 0
        if not seq:
            raise Done()
        x = seq.pop(0)
        if isinstance(x, tuple):
            if x[0] == "shot":
                shot(x[1])
            elif x[0] == "idle":
                state["idle"] = x[1]
            return 0
        return x
    return poll

if __name__ == "__main__":
    from buttons import UP, DOWN, LEFT, RIGHT, A, B
    which = sys.argv[1] if len(sys.argv) > 1 else "xmb"
    if which == "splash":
        tos.splash(); shot("splash_text")
    elif which == "xmb":
        tos.btn.poll = drive([("idle", 30), ("shot", "xmb_games"),
                              DOWN, ("idle", 30), ("shot", "xmb_games_2"),
                              RIGHT, ("idle", 30), ("shot", "xmb_gameboy"),
                              RIGHT, ("idle", 30), ("shot", "xmb_files"),
                              RIGHT, ("idle", 30), ("shot", "xmb_settings"),
                              DOWN, DOWN, ("idle", 30), ("shot", "xmb_settings_3"),
                              RIGHT, ("idle", 30), ("shot", "xmb_system"),
                              ("idle", 3)])
        try:
            tos.home()
        except Done:
            pass
    print("done")
