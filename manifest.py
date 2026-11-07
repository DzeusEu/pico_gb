# Firmware manifest: keeps the standard MicroPython modules and freezes the RTSP_OS system
# files into the firmware image. Frozen code runs straight from flash and uses almost no RAM
# (normally the .py files are compiled into RAM at every boot, which costs tens of KB and
# fragments the heap). config.py, main.py, games and emulators stay as normal files.
include("$(PORT_DIR)/boards/manifest.py")
freeze("frozen", ("tinyos.py", "st7789.py", "buttons.py", "filebrowser.py", "sdcard.py", "xmb.py"))
