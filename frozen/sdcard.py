# Minimal SD / SDHC card driver (SPI mode) for MicroPython.
# Provides the block-device interface that uos.VfsFat() needs:
#   readblocks(block, buf), writeblocks(block, buf), ioctl(op, arg)
#
#   spi = SPI(0, baudrate=400000, polarity=0, phase=0, sck=..., mosi=..., miso=...)
#   card = SDCard(spi, Pin(17, Pin.OUT, value=1))
#   uos.mount(uos.VfsFat(card), "/sd")
import time

_TOKEN_DATA = 0xFE
_TOKEN_MULTI_WRITE = 0xFC
_TOKEN_STOP = 0xFD


class SDCard:
    def __init__(self, spi, cs, baudrate=10_000_000):
        self.spi = spi
        self.cs = cs
        self.cs(1)
        self.cmdbuf = bytearray(6)
        self.tok = bytearray(1)
        self.extra = bytearray(4)
        self.ff = bytearray(b"\xff" * 512)
        self.ffv = memoryview(self.ff)
        self.cdv = 1
        self.sectors = 0
        self._init_card(baudrate)

    # ---------------------------------------------------------------- low level
    def _release(self):
        self.cs(1)
        self.spi.write(b"\xff")

    def _cmd(self, cmd, arg, crc=0xFF, extra=0, release=True, skip1=False):
        """Send a command. Returns the R1 byte, or -1 on timeout.
        If extra > 0 that many trailing response bytes land in self.extra."""
        self.cs(0)
        b = self.cmdbuf
        b[0] = 0x40 | cmd
        b[1] = (arg >> 24) & 0xFF
        b[2] = (arg >> 16) & 0xFF
        b[3] = (arg >> 8) & 0xFF
        b[4] = arg & 0xFF
        b[5] = crc
        self.spi.write(b)
        if skip1:
            self.spi.readinto(self.tok, 0xFF)
        for _ in range(100):
            self.spi.readinto(self.tok, 0xFF)
            r = self.tok[0]
            if not (r & 0x80):
                if extra:
                    self.spi.readinto(self.extra, 0xFF)
                if release:
                    self._release()
                return r
        self._release()
        return -1

    def _wait_ready(self, ms=500):
        t = time.ticks_ms()
        while time.ticks_diff(time.ticks_ms(), t) < ms:
            self.spi.readinto(self.tok, 0xFF)
            if self.tok[0] == 0xFF:
                return True
        return False

    def _init_card(self, baudrate):
        spi = self.spi
        spi.init(baudrate=400000, phase=0, polarity=0)
        self.cs(1)
        spi.write(b"\xff" * 10)  # 80 clock cycles with CS high

        for _ in range(5):
            if self._cmd(0, 0, 0x95) == 1:  # go idle
                break
            time.sleep_ms(20)
        else:
            raise OSError("no SD card")

        r = self._cmd(8, 0x01AA, 0x87, extra=4)
        if r == 1:
            ready = False
            for _ in range(100):
                self._cmd(55, 0)
                if self._cmd(41, 0x40000000) == 0:
                    ready = True
                    break
                time.sleep_ms(20)
            if not ready:
                raise OSError("SD init timeout")
            if self._cmd(58, 0, extra=4) != 0:
                raise OSError("SD OCR error")
            self.cdv = 1 if (self.extra[0] & 0x40) else 512
        elif r == 5:  # v1 card (illegal command)
            ready = False
            for _ in range(100):
                self._cmd(55, 0)
                if self._cmd(41, 0) == 0:
                    ready = True
                    break
                time.sleep_ms(20)
            if not ready:
                raise OSError("SD init timeout")
            self.cdv = 512
        else:
            raise OSError("unsupported SD card")

        if self._cmd(16, 512) != 0:
            raise OSError("SD block size error")

        csd = bytearray(16)
        if self._cmd(9, 0, release=False) != 0:
            self._release()
            raise OSError("SD CSD error")
        self._read_data(csd)
        if (csd[0] & 0xC0) >> 6 == 1:  # CSD v2 (SDHC/SDXC)
            c_size = ((csd[7] & 0x3F) << 16) | (csd[8] << 8) | csd[9]
            self.sectors = (c_size + 1) * 1024
        else:                          # CSD v1
            c_size = ((csd[6] & 3) << 10) | (csd[7] << 2) | (csd[8] >> 6)
            mult = ((csd[9] & 3) << 1) | (csd[10] >> 7)
            blk = 1 << (csd[5] & 15)
            self.sectors = (c_size + 1) * (1 << (mult + 2)) * blk // 512

        spi.init(baudrate=baudrate, phase=0, polarity=0)

    def _read_data(self, buf):
        """Read one data block into buf (CS must already be low)."""
        t = time.ticks_ms()
        while True:
            self.spi.readinto(self.tok, 0xFF)
            if self.tok[0] == _TOKEN_DATA:
                break
            if time.ticks_diff(time.ticks_ms(), t) > 300:
                self._release()
                raise OSError(5)
        n = len(buf)
        self.spi.write_readinto(self.ffv[:n] if n != 512 else self.ffv, buf)
        self.spi.write(b"\xff\xff")  # CRC (ignored)
        self._release()

    def _write_data(self, token, buf):
        self.cs(0)
        self.spi.write(bytes((token,)))
        self.spi.write(buf)
        self.spi.write(b"\xff\xff")
        self.spi.readinto(self.tok, 0xFF)
        if (self.tok[0] & 0x1F) != 0x05:
            self._release()
            raise OSError(5)
        if not self._wait_ready(1000):
            self._release()
            raise OSError(5)
        self._release()

    # ---------------------------------------------------------------- block device
    def readblocks(self, block, buf):
        n = len(buf) // 512
        if n < 1 or len(buf) % 512:
            raise OSError(22)
        if n == 1:
            if self._cmd(17, block * self.cdv, release=False) != 0:
                self._release()
                raise OSError(5)
            self._read_data(buf)
            return
        if self._cmd(18, block * self.cdv, release=False) != 0:
            self._release()
            raise OSError(5)
        mv = memoryview(buf)
        for i in range(n):
            self._read_data_multi(mv[i * 512:(i + 1) * 512])
        self._cmd(12, 0, skip1=True)

    def _read_data_multi(self, buf):
        # like _read_data but keeps CS low between blocks
        t = time.ticks_ms()
        while True:
            self.spi.readinto(self.tok, 0xFF)
            if self.tok[0] == _TOKEN_DATA:
                break
            if time.ticks_diff(time.ticks_ms(), t) > 300:
                self._release()
                raise OSError(5)
        self.spi.write_readinto(self.ffv, buf)
        self.spi.write(b"\xff\xff")

    def writeblocks(self, block, buf):
        n = len(buf) // 512
        if n < 1 or len(buf) % 512:
            raise OSError(22)
        if n == 1:
            if self._cmd(24, block * self.cdv, release=False) != 0:
                self._release()
                raise OSError(5)
            self._write_data(_TOKEN_DATA, buf)
            return
        if self._cmd(25, block * self.cdv, release=False) != 0:
            self._release()
            raise OSError(5)
        mv = memoryview(buf)
        for i in range(n):
            self._write_data_multi(mv[i * 512:(i + 1) * 512])
        # stop transmission token
        self.cs(0)
        self.spi.write(bytes((_TOKEN_STOP,)))
        self.spi.write(b"\xff")
        if not self._wait_ready(1000):
            self._release()
            raise OSError(5)
        self._release()

    def _write_data_multi(self, buf):
        self.spi.write(bytes((_TOKEN_MULTI_WRITE,)))
        self.spi.write(buf)
        self.spi.write(b"\xff\xff")
        self.spi.readinto(self.tok, 0xFF)
        if (self.tok[0] & 0x1F) != 0x05:
            self._release()
            raise OSError(5)
        if not self._wait_ready(1000):
            self._release()
            raise OSError(5)

    def ioctl(self, op, arg):
        if op == 4:   # number of blocks
            return self.sectors
        if op == 5:   # block size
            return 512
        if op in (1, 2, 3):  # init, deinit, sync
            return 0
        return None
