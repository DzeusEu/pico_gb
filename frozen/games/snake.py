# Snake - D-pad steers, START pauses, B quits.
import time
import random
from buttons import UP, DOWN, LEFT, RIGHT, A, B, START

CELL = 10
TOP = 20


def run(tos):
    best = tos.load("snake", 0)
    while True:
        score = play(tos, best)
        if score is None:
            return
        if score > best:
            best = score
            tos.save("snake", best)
        if not game_over(tos, score, best):
            return


def cell(tos, ox, cx, cy, color):
    tos.lcd.fill_rect(ox + cx * CELL + 1, TOP + cy * CELL + 1, CELL - 2, CELL - 2, color)


def hud(tos, score, best):
    tos.lcd.fill_rect(0, 0, tos.W, TOP, tos.ACCENT)
    tos.lcd.text("WĄŻ", 6, 6, tos.WHITE, tos.ACCENT)
    s = "Wynik %d  Rekord %d" % (score, best)
    tos.lcd.text(s, tos.W - 6 - 8 * len(s), 6, tos.WHITE, tos.ACCENT)


def new_food(snake, cols, rows):
    if len(snake) >= cols * rows:
        return None
    while True:
        p = (random.randrange(cols), random.randrange(rows))
        if p not in snake:
            return p


def play(tos, best):
    cols = tos.W // CELL
    rows = (tos.H - TOP) // CELL
    ox = (tos.W - cols * CELL) // 2
    tos.clear(tos.DARK)
    hud(tos, 0, best)
    snake = [(cols // 2 - i, rows // 2) for i in range(3)]
    for c in snake:
        cell(tos, ox, c[0], c[1], tos.GREEN)
    d = (1, 0)
    nd = d
    food = new_food(snake, cols, rows)
    cell(tos, ox, food[0], food[1], tos.RED)
    score = 0
    delay = 140
    last = time.ticks_ms()
    tos.btn.release()
    while True:
        k = tos.btn.poll()
        if k & B:
            return None
        if k & START:
            tos.lcd.text("  PAUZA ", tos.W // 2 - 32, tos.H // 2 - 4, tos.YELLOW, tos.BLACK)
            while not (tos.btn.wait() & (START | B)):
                pass
            # repaint the cells hidden by the banner
            for c in snake:
                cell(tos, ox, c[0], c[1], tos.GREEN)
            tos.lcd.fill_rect(ox, TOP + (rows // 2 - 1) * CELL, cols * CELL, CELL * 3, tos.DARK)
            for c in snake:
                cell(tos, ox, c[0], c[1], tos.GREEN)
            cell(tos, ox, food[0], food[1], tos.RED)
            last = time.ticks_ms()
        if k & UP and d[1] == 0:
            nd = (0, -1)
        elif k & DOWN and d[1] == 0:
            nd = (0, 1)
        elif k & LEFT and d[0] == 0:
            nd = (-1, 0)
        elif k & RIGHT and d[0] == 0:
            nd = (1, 0)
        now = time.ticks_ms()
        if time.ticks_diff(now, last) < delay:
            time.sleep_ms(4)
            continue
        last = now
        d = nd
        nx, ny = snake[0][0] + d[0], snake[0][1] + d[1]
        eat = (nx, ny) == food
        body = snake if eat else snake[:-1]
        if not (0 <= nx < cols and 0 <= ny < rows) or (nx, ny) in body:
            tos.beep(180, 250)
            return score
        snake.insert(0, (nx, ny))
        cell(tos, ox, nx, ny, tos.GREEN)
        if eat:
            score += 10
            tos.beep(1200, 20)
            delay = max(60, delay - 3)
            food = new_food(snake, cols, rows)
            hud(tos, score, max(best, score))
            if food is None:
                return score
            cell(tos, ox, food[0], food[1], tos.RED)
        else:
            tx, ty = snake.pop()
            tos.lcd.fill_rect(ox + tx * CELL, TOP + ty * CELL, CELL, CELL, tos.DARK)


def game_over(tos, score, best):
    tos.lcd.fill_rect(60, 80, tos.W - 120, 70, tos.BLACK)
    tos.lcd.rect(60, 80, tos.W - 120, 70, tos.RED)
    tos.ctext("KONIEC GRY", 90, tos.RED, tos.BLACK, 2)
    tos.ctext("Wynik %d  Rekord %d" % (score, best), 116, tos.WHITE, tos.BLACK)
    tos.ctext("A:Jeszcze raz B:Wyjdź", 132, tos.GRAY, tos.BLACK)
    while True:
        k = tos.btn.wait()
        if k & A:
            return True
        if k & B:
            return False
