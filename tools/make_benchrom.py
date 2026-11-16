#!/usr/bin/env python3
"""Tiny CPU benchmark ROM (integer loop with loads/stores/branches)."""
import struct, sys
def I(op, rs, rt, imm): return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)
def R(rs, rt, rd, sa, fn): return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn
code = [
    I(0x0F, 0, 8, 0xA000),              # lui  t0,0xA000
    I(0x0D, 8, 8, 0x2000),              # ori  t0,t0,0x2000
    I(0x0F, 0, 10, 0x0016),             # lui  t2,0x0016
    I(0x0D, 10, 10, 0xE360),            # ori  t2,t2,0xE360   (1,500,000 iterations)
    # loop:
    I(0x23, 8, 11, 0),                  # lw   t3,0(t0)
    R(11, 10, 11, 0, 0x21),             # addu t3,t3,t2
    I(0x2B, 8, 11, 0),                  # sw   t3,0(t0)
    I(0x09, 9, 9, 1),                   # addiu t1,t1,1
    R(0, 9, 12, 2, 0x00),               # sll  t4,t1,2
    R(12, 11, 13, 0, 0x25),             # or   t5,t4,t3
    I(0x09, 10, 10, 0xFFFF),            # addiu t2,t2,-1
    I(0x05, 10, 0, 0xFFF8),             # bne  t2,zero,loop  (-8 words -> loop start)
    0,                                  # nop
    I(0x04, 0, 0, 0xFFFF),              # b .
    0,
]
rom = bytearray(0x1000)
rom[0:4] = bytes([0x80, 0x37, 0x12, 0x40]); struct.pack_into('>I', rom, 8, 0x80000400)
rom[0x20:0x20 + 20] = b'BENCH'.ljust(20)
ram = bytearray(0x400)
# code placed at RAM 0x400 -> ROM offset 0x1000+0x400
body = bytearray(0x1000)
for i, w in enumerate(code): struct.pack_into('>I', body, 0x400 + 4 * i, w)
open(sys.argv[1] if len(sys.argv) > 1 else 'benchrom.z64', 'wb').write((bytes(rom) + bytes(body)).ljust(0x10000, b'\0'))
