# RTSP_OS entry point - runs automatically on power-up.
import gc
import io
import sys
import time

import tinyos


def main():
    tos = tinyos.System()
    tos.splash()
    while True:
        try:
            tos.home()
        except Exception as e:
            buf = io.StringIO()
            sys.print_exception(e, buf)
            try:
                tos.viewer("Błąd systemu", buf.getvalue())
            except Exception:
                time.sleep(2)
            gc.collect()


main()
