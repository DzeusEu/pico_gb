# TinyOS hardware configuration - edit to match your wiring.
# Pin numbers are GPIO numbers (GP#), not physical pin numbers.

# ---- Display: GTM020-02-7P (ST7789, 240x320, SPI, 3.3V only) ----
SPI_ID = 1
PIN_SCK = 14      # module pin SCL
PIN_MOSI = 15     # module pin SDA
PIN_DC = 12       # module pin DC
PIN_CS = 11       # module pin CS
PIN_RST = 13      # module pin RES
SPI_BAUD = 40_000_000   # lower to 20_000_000 if you see noise with long wires

LCD_W = 240       # native panel size (portrait)
LCD_H = 320
LCD_ROTATION = 1  # 1 = landscape 320x240, 3 = landscape flipped 180 degrees
LCD_INVERT = True # most ST7789 IPS panels need inversion on
LCD_BGR = False   # set True if red and blue are swapped

# ---- Buttons (wire each between the GPIO and GND, internal pull-ups used) ----
# Optional: add "SELECT": <gpio> for a real Game Boy Select button.
BUTTONS = {
    "UP": 2,
    "DOWN": 3,
    "LEFT": 4,
    "RIGHT": 5,
    "A": 6,
    "B": 7,
    "START": 8,
}

# ---- Buzzer (optional, passive piezo/speaker to GND). None = no sound ----
# GP15 is used by the display here, so the buzzer is disabled. Use a free pin (e.g. 22) if you add one.
PIN_BUZZER = None

# ---- SD card (SPI mode, separate bus from the display) ----
# Remove or comment out SD_CS to disable SD support.
SD_SPI_ID = 0
SD_SCK = 18       # SD pin 5  (CLK)
SD_MOSI = 19      # SD pin 2  (DI / CMD)
SD_MISO = 16      # SD pin 7  (DO / DAT0)
SD_CS = 17        # SD pin 1  (CD / DAT3)
SD_BAUD = 10_000_000   # lower to 4_000_000 if the card is flaky on long wires

# Game Boy: SD clock used while a game is running (lower if you get read errors)
GB_SD_BAUD = 16_000_000
POWER_HOLD_MS = 1500
