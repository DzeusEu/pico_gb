# Pong vs CPU - UP/DOWN move your paddle (left), START pauses, B quits. First to 7.
import time
import random
from buttons import UP, DOWN, A, B, START

TOP = 20
PW = 6
PH = 40
BALL = 6
WIN = 7


def run(tos):
    while True:
        res = match(tos)
        if res is None:
            return
        tos.lcd.fill_rect(70, 90, tos.W - 140, 60, tos.BLACK)
        tos.lcd.rect(70, 90, tos.W - 140, 60, tos.WHITE)
        tos.ctext("WYGRAŁEŚ!" if res else "PRZEGRAŁEŚ", 100, tos.GREEN if res else tos.RED, tos.BLACK, 2)
        tos.ctext("A:Jeszcze raz B:Wyjdź", 126, tos.GRAY, tos.BLACK)
        while True:
            k = tos.btn.wait()
            if k & A:
                break
            if k & B:
                return


def hud(tos, you, cpu):
    tos.lcd.fill_rect(0, 0, tos.W, TOP, tos.ACCENT)
    tos.lcd.text("PONG", 6, 6, tos.WHITE, tos.ACCENT)
    s = "Ty %d : %d KOMP" % (you, cpu)
    tos.lcd.text(s, tos.W - 6 - 8 * len(s), 6, tos.WHITE, tos.ACCENT)


def centerline(tos):
    x = tos.W // 2 - 1
    for y in range(TOP + 4, tos.H, 16):
        tos.lcd.fill_rect(x, y, 2, 8, tos.GRAY)


def match(tos):
    W, H = tos.W, tos.H
    you = cpu = 0
    ly = ry = (H + TOP) // 2 - PH // 2
    lx = 8
    rx = W - 8 - PW
    tos.clear(tos.DARK)
    hud(tos, you, cpu)
    centerline(tos)
    tos.lcd.fill_rect(lx, ly, PW, PH, tos.WHITE)
    tos.lcd.fill_rect(rx, ry, PW, PH, tos.WHITE)
    tos.btn.release()
    while True:
        speed = 4.0
        bx = W / 2
        by = (H + TOP) / 2
        vx = speed * random.choice((-1, 1))
        vy = random.choice((-2.0, -1.0, 1.0, 2.0))
        ox, oy = int(bx), int(by)
        last = time.ticks_ms()
        scored = None
        while scored is None:
            k = tos.btn.poll()
            if k & B:
                return None
            if k & START:
                tos.ctext("  PAUZA ", H // 2, tos.YELLOW, tos.BLACK)
                while not (tos.btn.wait() & (START | B)):
                    pass
                tos.lcd.fill_rect(W // 2 - 32, H // 2, 64, 8, tos.DARK)
                centerline(tos)
                last = time.ticks_ms()
            now = time.ticks_ms()
            if time.ticks_diff(now, last) < 20:
                time.sleep_ms(2)
                continue
            last = now
            held = tos.btn.read()
            # your paddle
            nly = ly
            if held & UP:
                nly -= 5
            if held & DOWN:
                nly += 5
            nly = max(TOP, min(H - PH, nly))
            if nly != ly:
                tos.lcd.fill_rect(lx, ly, PW, PH, tos.DARK)
                ly = nly
                tos.lcd.fill_rect(lx, ly, PW, PH, tos.WHITE)
            # cpu paddle (only tracks when ball comes toward it, limited speed)
            target = by - PH / 2 if vx > 0 else (H + TOP) / 2 - PH / 2
            nry = ry
            if target > ry + 4:
                nry += 3
            elif target < ry - 4:
                nry -= 3
            nry = max(TOP, min(H - PH, nry))
            if nry != ry:
                tos.lcd.fill_rect(rx, ry, PW, PH, tos.DARK)
                ry = nry
                tos.lcd.fill_rect(rx, ry, PW, PH, tos.WHITE)
            # ball
            tos.lcd.fill_rect(ox, oy, BALL, BALL, tos.DARK)
            bx += vx
            by += vy
            if by <= TOP:
                by = TOP
                vy = abs(vy)
                tos.beep(500, 6)
            elif by >= H - BALL:
                by = H - BALL
                vy = -abs(vy)
                tos.beep(500, 6)
            if vx < 0 and bx <= lx + PW and bx + BALL >= lx and ly - BALL < by < ly + PH:
                bx = lx + PW
                speed = min(9.0, speed + 0.4)
                vx = speed
                vy = (by + BALL / 2 - (ly + PH / 2)) / (PH / 2) * 4
                tos.beep(900, 8)
            elif vx > 0 and bx + BALL >= rx and bx <= rx + PW and ry - BALL < by < ry + PH:
                bx = rx - BALL
                speed = min(9.0, speed + 0.4)
                vx = -speed
                vy = (by + BALL / 2 - (ry + PH / 2)) / (PH / 2) * 4
                tos.beep(700, 8)
            if bx < -BALL:
                scored = "cpu"
            elif bx > W:
                scored = "you"
            else:
                ox, oy = int(bx), int(by)
                tos.lcd.fill_rect(ox, oy, BALL, BALL, tos.WHITE)
                if abs(ox - W // 2) < 12:
                    centerline(tos)
        if scored == "you":
            you += 1
            tos.beep(1300, 80)
        else:
            cpu += 1
            tos.beep(180, 160)
        hud(tos, you, cpu)
        tos.lcd.fill_rect(ox, oy, BALL, BALL, tos.DARK)
        centerline(tos)
        if you >= WIN:
            return True
        if cpu >= WIN:
            return False
        time.sleep_ms(500)
