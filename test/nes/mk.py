# builds a test NROM: CPU test results in RAM, then a picture
import struct,sys
code=bytearray()
def emit(*b): code.extend(b)
# CPU test: 100+50 ADC -> ram0 (150, V set), SBC 10-20 -> ram1, loop sum 1..10 -> ram2, JSR/RTS -> ram3
emit(0x78,0xD8,0xA2,0xFF,0x9A)               # SEI CLD LDX #FF TXS
emit(0x18,0xA9,100,0x69,50,0x85,0)            # CLC LDA#100 ADC#50 STA 0
emit(0x38,0xA9,10,0xE9,20,0x85,1)             # SEC LDA#10 SBC#20 STA 1  (=0xF6)
emit(0xA9,0,0xA2,10,0x18,0x65,0x00)           # placeholder replaced below
code=code[:-7]
emit(0xA9,0,0xA2,10)                          # LDA#0 LDX#10
loop=len(code); emit(0x86,0x10,0x18,0x65,0x10,0xCA,0xD0,0)   # STX 10; CLC; ADC 10; DEX; BNE loop
code[-1]=(loop-len(code))&0xFF
emit(0x85,2)                                  # STA 2 -> 55
emit(0x20,0,0)                                # JSR sub (patch)
jsr=len(code)-2
emit(0x4C,0,0)                                # JMP picture
jmp=len(code)-2
sub=len(code); emit(0xA9,0x77,0x85,3,0x60)    # LDA#77 STA 3 RTS
pic=len(code)
base=0x8000
code[jsr]=(base+sub)&0xFF; code[jsr+1]=(base+sub)>>8
code[jmp]=(base+pic)&0xFF; code[jmp+1]=(base+pic)>>8
# picture: wait vblank, palette, nametable fill, enable bg
emit(0xAD,0x02,0x20,0x10,0xFB)                # BIT loop: LDA $2002; BPL -5
emit(0xA9,0x3F,0x8D,0x06,0x20,0xA9,0x00,0x8D,0x06,0x20)  # PPUADDR 3F00
for c in (0x0F,0x16,0x2A,0x30): emit(0xA9,c,0x8D,0x07,0x20)
emit(0xA9,0x20,0x8D,0x06,0x20,0xA9,0x00,0x8D,0x06,0x20)  # PPUADDR 2000
# write 960 tiles: tile = (x+y)&3 using counter in X over 4 loops
emit(0xA2,0,0xA0,4)                            # LDX#0 LDY#4  (4*256 = 1024 writes)
l2=len(code); emit(0x8A,0x8D,0x07,0x20,0xE8,0xD0,0)   # TXA; STA 2007; INX; BNE
code[-1]=(l2-len(code))&0xFF
emit(0x88,0xD0,0)                              # DEY BNE
code[-1]=(l2-len(code))&0xFF
emit(0xA9,0,0x8D,0x05,0x20,0x8D,0x05,0x20)    # scroll 0,0
emit(0xA9,0x80,0x8D,0x00,0x20,0xA9,0x0A,0x8D,0x01,0x20) # NMI on, bg on+left col
fin=len(code); emit(0x4C,(base+fin)&0xFF,(base+fin)>>8)
# NMI/IRQ handlers
nmi=len(code); emit(0x40)
prg=bytearray(32768); prg[:len(code)]=code
prg[0x7FFA]=(base+nmi)&0xFF; prg[0x7FFB]=(base+nmi)>>8
prg[0x7FFC]=base&0xFF; prg[0x7FFD]=base>>8
prg[0x7FFE]=(base+nmi)&0xFF; prg[0x7FFF]=(base+nmi)>>8
chr=bytearray(8192)
# tile n (0..255): pattern by n: use bits of n as fill
for t in range(256):
    for r in range(8):
        chr[t*16+r]=(0xFF if (t&1) else (0xAA if r&1 else 0x55))
        chr[t*16+8+r]=(0xFF if (t&2) else (0x0F if r<4 else 0xF0))
hdr=b'NES\x1a'+bytes([2,1,1,0])+bytes(8)
open('t.nes','wb').write(hdr+prg+chr)
