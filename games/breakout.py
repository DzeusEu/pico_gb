# Breakout - LEFT/RIGHT move paddle, A launches ball, START pauses, B quits.
import time
import random
from buttons import LEFT, RIGHT, A, B, START

TOP = 20
COLS = 10
ROWS = 5
BW = 30
BH = 12
BX0 = 10
BY0 = 34
BALL = 6
PW = 48
PH = 6


def run(tos):
    best = tos.load("breakout", 0)
    while True:
        score = play(tos, best)
        if score is None:
            return
        if score > best:
            best = score
            tos.save("breakout", best)
        if not game_over(tos, score, best):
            return


def hud(tos, score, lives, best):
    tos.lcd.fill_rect(0, 0, tos.W, TOP, tos.ACCENT)
    tos.lcd.text("CEGIEŁKI", 6, 6, tos.WHITE, tos.ACCENT)
    s = "Życia %d Wynik %d Rekord %d" % (lives, score, best)
    tos.lcd.text(s, tos.W - 6 - 8 * len(s), 6, tos.WHITE, tos.ACCENT)


def draw_brick(tos, colors, r, c, alive):
    x = BX0 + c * BW
    y = BY0 + r * BH
    tos.lcd.fill_rect(x + 1, y + 1, BW - 2, BH - 2, colors[r] if alive else tos.DARK)
    if not alive:
        tos.lcd.fill_rect(x, y, BW, BH, tos.DARK)


def build(tos, colors):
    alive = [[True] * COLS for _ in range(ROWS)]
    for r in range(ROWS):
        for c in range(COLS):
            draw_brick(tos, colors, r, c, True)
    return alive


def play(tos, best):
    W, H = tos.W, tos.H
    colors = (tos.RED, tos.ORANGE, tos.YELLOW, tos.GREEN, tos.CYAN)
    py = H - 14
    tos.clear(tos.DARK)
    score = 0
    lives = 3
    level = 1
    alive = build(tos, colors)
    bricks = ROWS * COLS
    hud(tos, score, lives, best)
    px = (W - PW) // 2
    tos.lcd.fill_rect(px, py, PW, PH, tos.WHITE)
    stuck = True
    bx = px + PW // 2 - BALL // 2
    by = py - BALL
    vx = vy = 0
    last = time.ticks_ms()
    tos.btn.release()
    while True:
        k = tos.btn.poll()
        if k & B:
            return None
        if k & START:
            tos.ctext("  PAUZA ", H // 2, tos.YELLOW, tos.BLACK)
            while not (tos.btn.wait() & (START | B)):
                pass
            tos.lcd.fill_rect(0, H // 2, W, 8, tos.DARK)
            for r in range(ROWS):
                for c in range(COLS):
                    if alive[r][c]:
                        draw_brick(tos, colors, r, c, True)
            last = time.ticks_ms()
        now = time.ticks_ms()
        if time.ticks_diff(now, last) < 20:
            time.sleep_ms(2)
            continue
        last = now

        # paddle
        held = tos.btn.read()
        npx = px
        if held & LEFT:
            npx -= 6
        if held & RIGHT:
            npx += 6
        npx = max(0, min(W - PW, npx))
        if npx != px:
            tos.lcd.fill_rect(px, py, PW, PH, tos.DARK)
            px = npx
            tos.lcd.fill_rect(px, py, PW, PH, tos.WHITE)

        # ball
        tos.lcd.fill_rect(bx, by, BALL, BALL, tos.DARK)
        if stuck:
            bx = px + PW // 2 - BALL // 2
            by = py - BALL
            if k & A:
                stuck = False
                vx = random.choice((-2, -1, 1, 2))
                vy = -min(6, 3 + level // 2)
        else:
            bx += vx
            by += vy
            if bx <= 0:
                bx = 0
                vx = abs(vx)
                tos.beep(600, 8)
            elif bx >= W - BALL:
                bx = W - BALL
                vx = -abs(vx)
                tos.beep(600, 8)
            if by <= TOP:
                by = TOP
                vy = abs(vy)
                tos.beep(600, 8)
            # paddle bounce
            if vy > 0 and py <= by + BALL <= py + PH + vy and bx + BALL > px and bx < px + PW:
                off = (bx + BALL // 2 - (px + PW // 2)) * 2 / PW  # -1..1
                vx = int(off * 4)
                if vx == 0:
                    vx = random.choice((-1, 1))
                vy = -abs(vy)
                by = py - BALL
                tos.beep(900, 8)
            # brick hit
            cx = bx + BALL // 2
            cy = by + BALL // 2
            if BY0 <= cy < BY0 + ROWS * BH:
                r = (cy - BY0) // BH
                c = (cx - BX0) // BW
                if 0 <= c < COLS and alive[r][c]:
                    alive[r][c] = False
                    draw_brick(tos, colors, r, c, False)
                    vy = -vy
                    score += 10 * (ROWS - r)
                    bricks -= 1
                    tos.beep(1100 + 100 * (ROWS - r), 10)
                    hud(tos, score, lives, max(best, score))
            # lost ball
            if by > H:
                lives -= 1
                tos.beep(200, 200)
                if lives == 0:
                    return score
                hud(tos, score, lives, max(best, score))
                stuck = True
                bx = px + PW // 2 - BALL // 2
                by = py - BALL
            # level cleared
            if bricks == 0:
                level += 1
                tos.beep(1500, 120)
                alive = build(tos, colors)
                bricks = ROWS * COLS
                stuck = True
                by = py - BALL
        tos.lcd.fill_rect(bx, by, BALL, BALL, tos.WHITE)


def game_over(tos, score, best):
    tos.lcd.fill_rect(60, 90, tos.W - 120, 70, tos.BLACK)
    tos.lcd.rect(60, 90, tos.W - 120, 70, tos.RED)
    tos.ctext("KONIEC GRY", 100, tos.RED, tos.BLACK, 2)
    tos.ctext("Wynik %d  Rekord %d" % (score, best), 126, tos.WHITE, tos.BLACK)
    tos.ctext("A:Jeszcze raz B:Wyjdź", 142, tos.GRAY, tos.BLACK)
    while True:
        k = tos.btn.wait()
        if k & A:
            return True
        if k & B:
            return False
