# File browser for TinyOS (internal flash filesystem).
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
        text += "\n\n[... truncated at 4 KB ...]"
    tos.viewer(path.rpartition("/")[2], text)


def _info(tos, path, is_dir):
    st = uos.stat(path)
    kind = "Folder" if is_dir else "File"
    text = "%s: %s\nPath: %s\nSize: %d bytes" % (kind, path.rpartition("/")[2], path, st[6])
    if not is_dir:
        e = ext_of(path)
        if e == "py":
            text += "\nType: program (A runs it)"
        else:
            if tos.find_emu(e):
                text += "\nHandler: emulator '%s'" % e
            else:
                text += "\nHandler: none installed"
    tos.message("Info", text)


def _delete(tos, path, name, is_dir):
    if path.count("/") == 1 and name.rpartition(".")[0] in PROTECTED:
        tos.message("Protected", "System files cannot be deleted here.", tos.ORANGE)
        return
    if not tos.confirm("Delete %s '%s'?" % ("folder" if is_dir else "file", name)):
        return
    try:
        if is_dir:
            uos.rmdir(path)
        else:
            uos.remove(path)
    except OSError:
        tos.message("Error", "Could not delete (folders must be empty).", tos.RED)


def _open(tos, path, name):
    ext = ext_of(name)
    tos.btn.release()
    if tos.launch_file(path):
        return
    if ext in TEXT_EXT:
        _view(tos, path)
    else:
        _info(tos, path, False)


def run(tos):
    path = "/"
    memo = {}
    while True:
        try:
            entries = _list(path)
        except OSError:
            path = "/"
            continue
        items = []
        for name, is_dir, size in entries:
            if is_dir:
                items.append((name + "/", tos.YELLOW, ""))
            else:
                e = ext_of(name)
                color = tos.GREEN if e == "py" else tos.WHITE
                items.append((name, color, _size(size)))
        hint = "A:Open B:Up START:Menu"
        idx, key = tos.menu(path, items, hint, memo.get(path, 0), allow_start=True)
        if key == "B":
            if path == "/":
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
            actions = ["Info", "Delete", "Cancel"]
            if not is_dir:
                actions = ["Open / Run", "View as text"] + actions
            a, k = tos.menu(name, actions, "A:Select  B:Cancel")
            if k != "A":
                continue
            act = actions[a]
            if act == "Open / Run":
                _open(tos, full, name)
            elif act == "View as text":
                _view(tos, full)
            elif act == "Info":
                _info(tos, full, is_dir)
            elif act == "Delete":
                _delete(tos, full, name, is_dir)
