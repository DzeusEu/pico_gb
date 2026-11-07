# Przeglądarka plików RTSP_OS.
import uos
from tinyos import PROTECTED, TEXT_EXT, ext_of


def _join(path, name):
    return path.rstrip("/") + "/" + name


def _parent(path):
    return path.rstrip("/").rpartition("/")[0] or "/"


def _list(path):
    out = []
    for e in uos.ilistdir(path):
        size = e[3] if len(e) > 3 else 0
        out.append((e[0], e[1] == 0x4000, size))
    out.sort(key=lambda e: (not e[1], e[0].lower()))
    return out


def _size(n):
    if n < 1024:
        return "%dB" % n
    if n < 1024 * 1024:
        return "%.1fK" % (n / 1024)
    return "%.1fM" % (n / 1048576)


def _view(tos, path):
    with open(path, "rb") as f:
        data = f.read(4096)
    chars = []
    for b in data:
        if b == 10 or 32 <= b < 127:
            chars.append(chr(b))
        elif b == 9:
            chars.append("    ")
        elif b != 13:
            chars.append(".")
    text = "".join(chars)
    if len(data) == 4096:
        text += "\n\n[... ucięto po 4 KB ...]"
    tos.viewer(path.rpartition("/")[2], text)


def _info(tos, path, is_dir):
    st = uos.stat(path)
    kind = "Folder" if is_dir else "Plik"
    text = "%s: %s\nŚcieżka: %s\nRozmiar: %d B" % (kind, path.rpartition("/")[2], path, st[6])
    if not is_dir:
        e = ext_of(path)
        if e == "py":
            text += "\nTyp: program (A uruchamia)"
        else:
            if tos.find_emu(e):
                text += "\nObsługa: emulator '%s'" % e
            else:
                text += "\nObsługa: brak"
    tos.message("Informacje", text)


def _delete(tos, path, name, is_dir):
    if path.count("/") == 1 and name.rpartition(".")[0] in PROTECTED:
        tos.message("Chronione", "Plików systemowych nie można tu usunąć.", tos.ORANGE)
        return
    if not tos.confirm("Usunąć %s '%s'?" % ("folder" if is_dir else "plik", name)):
        return
    try:
        if is_dir:
            uos.rmdir(path)
        else:
            uos.remove(path)
    except OSError:
        tos.message("Błąd", "Nie udało się usunąć (folder musi być pusty).", tos.RED)


def _open(tos, path, name):
    ext = ext_of(name)
    tos.btn.release()
    if tos.launch_file(path):
        return
    if ext in TEXT_EXT:
        _view(tos, path)
    else:
        _info(tos, path, False)


def run(tos, start="/"):
    root = start
    path = start
    memo = {}
    while True:
        try:
            entries = _list(path)
        except OSError:
            path = root
            continue
        items = []
        for name, is_dir, size in entries:
            if is_dir:
                items.append((name + "/", tos.YELLOW, ""))
            else:
                e = ext_of(name)
                color = tos.GREEN if e == "py" else tos.WHITE
                items.append((name, color, _size(size)))
        hint = "A:Otwórz B:Wyżej START:Menu"
        idx, key = tos.menu(path, items, hint, memo.get(path, 0), allow_start=True)
        if key == "B":
            if path == root:
                return
            memo.pop(path, None)
            path = _parent(path)
            continue
        if idx < 0:
            continue
        memo[path] = idx
        name, is_dir, _ = entries[idx]
        full = _join(path, name)
        if key == "A":
            if is_dir:
                path = full
            else:
                _open(tos, full, name)
        else:  # START: context menu
            actions = ["Informacje", "Usuń", "Anuluj"]
            if not is_dir:
                actions = ["Otwórz / Uruchom", "Podgląd tekstu"] + actions
            a, k = tos.menu(name, actions, "A:Wybierz  B:Anuluj")
            if k != "A":
                continue
            act = actions[a]
            if act == "Otwórz / Uruchom":
                _open(tos, full, name)
            elif act == "Podgląd tekstu":
                _view(tos, full)
            elif act == "Informacje":
                _info(tos, full, is_dir)
            elif act == "Usuń":
                _delete(tos, full, name, is_dir)
