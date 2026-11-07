# .gbc files use the same emulator (Game Boy Color games that also support the
# original Game Boy run in monochrome; GBC-only games are not supported).
import sys


def run(tos, path=None):
    import gb
    try:
        gb.run(tos, path)
    finally:
        sys.modules.pop("gb", None)
