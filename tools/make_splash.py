# Converts any image to splash.raw for RTSP_OS (320x240, RGB565, high byte first).
# Usage:  python make_splash.py moj_obraz.png            -> splash.raw
#         python make_splash.py moj_obraz.png out.raw
# Then copy splash.raw to the root of the Pico (or to the SD card) with Thonny.
# Needs Pillow:  pip install pillow
import sys
from PIL import Image

W, H = 320, 240


def convert(src, dst):
    im = Image.open(src).convert("RGB")
    # fill the whole screen (centre crop), keeping the aspect ratio
    scale = max(W / im.width, H / im.height)
    nw, nh = round(im.width * scale), round(im.height * scale)
    im = im.resize((nw, nh), Image.LANCZOS)
    x, y = (nw - W) // 2, (nh - H) // 2
    im = im.crop((x, y, x + W, y + H))
    out = bytearray()
    for r, g, b in im.getdata():
        c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += bytes((c >> 8, c & 0xFF))
    open(dst, "wb").write(out)
    print("OK:", dst, len(out), "bytes (powinno być 153600)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "splash.raw")
