from os import *
import os as _o
VfsFat = None
def mount(*a): pass
def umount(*a): pass
def statvfs(p): return (4096, 4096, 512, 400, 400, 0, 0, 0, 0, 255)
def ilistdir(p="/"):
    for n in _o.listdir(p):
        st = _o.stat(p.rstrip("/") + "/" + n)
        yield (n, 0x4000 if _o.path.isdir(p.rstrip("/") + "/" + n) else 0x8000, 0, st.st_size)
