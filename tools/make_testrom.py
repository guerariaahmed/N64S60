#!/usr/bin/env python3
"""
Builds testrom.z64 / testrom.v64 / testrom.n64: a tiny self-checking N64
program used by tools/test_core.cpp to verify the emulator core on a PC.
You can also copy testrom.z64 to your phone: it fills the screen red.
"""
import struct, sys

def R(rs, rt, rd, sa, fn): return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn
def I(op, rs, rt, imm): return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)
def C1(fmt, ft, fs, fd, fn): return (0x11 << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn

class Asm:
    def __init__(self):
        self.w = {}          # address -> word
        self.pc = 0
        self.labels = {}
        self.fix = []
    def org(self, a): self.pc = a
    def emit(self, x):
        self.w[self.pc] = x & 0xFFFFFFFF
        self.pc += 4
    def label(self, n): self.labels[n] = self.pc
    def br(self, op, rs, rt, lab):
        self.fix.append((self.pc, lab, 'b')); self.emit(I(op, rs, rt, 0))
    def j(self, op, lab):
        self.fix.append((self.pc, lab, 'j')); self.emit(op << 26)
    def resolve(self):
        for a, lab, k in self.fix:
            t = self.labels[lab]
            if k == 'b':
                off = (t - (a + 4)) // 4
                self.w[a] = (self.w[a] & 0xFFFF0000) | (off & 0xFFFF)
            else:
                self.w[a] |= (t >> 2) & 0x3FFFFFF
    # helpers
    def lui(self, rt, imm): self.emit(I(0x0F, 0, rt, imm))
    def ori(self, rt, rs, imm): self.emit(I(0x0D, rs, rt, imm))
    def addiu(self, rt, rs, imm): self.emit(I(0x09, rs, rt, imm))
    def li(self, rt, v):
        self.lui(rt, (v >> 16) & 0xFFFF)
        if v & 0xFFFF: self.ori(rt, rt, v & 0xFFFF)
    def sw(self, rt, off, base): self.emit(I(0x2B, base, rt, off))
    def lw(self, rt, off, base): self.emit(I(0x23, base, rt, off))
    def nop(self): self.emit(0)
    def mtc1(self, rt, fs): self.emit((0x11 << 26) | (4 << 21) | (rt << 16) | (fs << 11))
    def mfc1(self, rt, fs): self.emit((0x11 << 26) | (0 << 21) | (rt << 16) | (fs << 11))

a = Asm()
ZERO, V0, A0 = 0, 2, 4
T0, T1, T2, T3, T4, T5, T6, T7 = 8, 9, 10, 11, 12, 13, 14, 15
T8, T9, K0, K1, RA = 24, 25, 26, 27, 31
RB = 0x8000     # results base (physical RAM)

# 0x000: j main
a.org(0x000); a.j(2, 'main'); a.nop()

# 0x180: interrupt handler: ++counter @0x104, clear VI int, eret
a.org(0x180)
a.lui(K0, 0xA000)
a.lw(K1, 0x104, K0)
a.addiu(K1, K1, 1)
a.sw(K1, 0x104, K0)
a.lui(K0, 0xA440)
a.sw(ZERO, 0x10, K0)
a.emit(0x42000018)          # ERET

# subroutine for jal test
a.org(0x3F00)
a.label('func')
a.ori(V0, ZERO, 0x77)
a.emit(R(RA, 0, 0, 0, 0x08))   # jr ra
a.nop()

# main
a.org(0x400)
a.label('main')
# framebuffer @0x100000: fill with red (RGBA5551 F801)
a.lui(T0, 0xA010)
a.ori(T2, ZERO, 0x9600)
a.lui(T1, 0xF801); a.ori(T1, T1, 0xF801)
a.label('fill')
a.sw(T1, 0, T0)
a.addiu(T0, T0, 4)
a.addiu(T2, T2, -1)
a.br(5, T2, ZERO, 'fill')
a.nop()
# VI setup
a.lui(T3, 0xA440)
a.lui(T4, 0x0010); a.sw(T4, 4, T3)       # ORIGIN
a.ori(T5, ZERO, 320); a.sw(T5, 8, T3)    # WIDTH
a.ori(T6, ZERO, 2);   a.sw(T6, 0, T3)    # CONTROL: 16bpp
# enable VI interrupt in MI + CPU
a.lui(T7, 0xA430); a.ori(T8, ZERO, 0x80); a.sw(T8, 0xC, T7)
a.emit((0x10 << 26) | (0 << 21) | (T9 << 16) | (12 << 11))   # mfc0 t9,Status
a.ori(T9, T9, 0x401)
a.emit((0x10 << 26) | (4 << 21) | (T9 << 16) | (12 << 11))   # mtc0 t9,Status

a.lui(A0, 0xA000); a.ori(A0, A0, RB)

# --- FPU ---
a.li(T0, 0x3FC00000); a.mtc1(T0, 0)       # f0 = 1.5
a.li(T0, 0x40000000); a.mtc1(T0, 2)       # f2 = 2.0
a.emit(C1(16, 2, 0, 4, 2))                # mul.s f4,f0,f2
a.emit(I(0x39, A0, 4, 0x00))              # swc1 f4,0(a0)
a.emit(C1(16, 0, 4, 6, 33))               # cvt.d.s f6,f4
a.emit(I(0x3D, A0, 6, 0x08))              # sdc1 f6,8(a0)
a.li(T0, 0x406CCCCD); a.mtc1(T0, 10)      # f10 = 3.7
a.emit(C1(16, 0, 10, 8, 13)); a.mfc1(T1, 8); a.sw(T1, 0x10, A0)   # trunc.w
a.emit(C1(16, 0, 10, 8, 12)); a.mfc1(T1, 8); a.sw(T1, 0x14, A0)   # round.w
a.li(T0, 0xBFC00000); a.mtc1(T0, 12)      # f12 = -1.5
a.emit(C1(16, 0, 12, 8, 15)); a.mfc1(T1, 8); a.sw(T1, 0x18, A0)   # floor.w
a.emit(C1(16, 0, 12, 8, 14)); a.mfc1(T1, 8); a.sw(T1, 0x1C, A0)   # ceil.w
a.li(T0, 0x41100000); a.mtc1(T0, 14)      # f14 = 9.0
a.emit(C1(16, 0, 14, 16, 4)); a.emit(I(0x39, A0, 16, 0x20))       # sqrt.s ; swc1
# c.lt.s f0,f2 ; bc1t
a.emit(C1(16, 2, 0, 0, 60))
a.ori(T1, ZERO, 0)
a.emit((0x11 << 26) | (8 << 21) | (1 << 16) | 2)    # bc1t +2 (skip delay+1)
a.nop()
a.ori(T1, ZERO, 0)       # skipped if taken
a.ori(T1, ZERO, 1)
a.sw(T1, 0x24, A0)
a.li(T0, 0x3F800000); a.mtc1(T0, 18)      # 1.0
a.li(T0, 0x40800000); a.mtc1(T0, 20)      # 4.0
a.emit(C1(16, 20, 18, 22, 3)); a.emit(I(0x39, A0, 22, 0x28))      # div.s
a.ori(T0, ZERO, 7); a.mtc1(T0, 24)
a.emit(C1(20, 0, 24, 26, 32)); a.emit(I(0x39, A0, 26, 0x2C))      # cvt.s.w

# --- integer ---
a.lui(T0, 1); a.ori(T0, T0, 0x86A0)       # 100000
a.emit(R(T0, T0, 0, 0, 0x18))             # mult
a.emit(R(0, 0, T1, 0, 0x12)); a.sw(T1, 0x40, A0)   # mflo
a.emit(R(0, 0, T1, 0, 0x10)); a.sw(T1, 0x44, A0)   # mfhi
a.ori(T0, ZERO, 100); a.ori(T2, ZERO, 7)
a.emit(R(T0, T2, 0, 0, 0x1A))             # div
a.emit(R(0, 0, T1, 0, 0x12)); a.sw(T1, 0x48, A0)
a.emit(R(0, 0, T1, 0, 0x10)); a.sw(T1, 0x4C, A0)
a.ori(T0, ZERO, 1)
a.emit(R(0, T0, T0, 0, 0x3C))             # dsll32 t0,t0,0
a.emit(I(0x3F, A0, T0, 0x50))             # sd
# lwl/lwr
a.li(T0, 0x11223344); a.sw(T0, 0x60, A0)
a.li(T0, 0x55667788); a.sw(T0, 0x64, A0)
a.ori(T1, ZERO, 0)
a.emit(I(0x22, A0, T1, 0x61))             # lwl t1,0x61(a0)
a.emit(I(0x26, A0, T1, 0x64))             # lwr t1,0x64(a0)
a.sw(T1, 0x68, A0)
# swl/swr
a.sw(ZERO, 0x70, A0); a.sw(ZERO, 0x74, A0)
a.li(T0, 0x01020304)
a.emit(I(0x2A, A0, T0, 0x71))             # swl
a.emit(I(0x2E, A0, T0, 0x74))             # swr
# likely branches
a.ori(T0, ZERO, 1)
a.emit(I(0x14, T0, ZERO, 1))              # beql t0,zero,+1 (not taken -> annul)
a.addiu(T0, T0, 100)
a.sw(T0, 0x80, A0)
a.emit(I(0x15, T0, ZERO, 1))              # bnel t0,zero,+1 (taken)
a.addiu(T0, T0, 5)
a.sw(T0, 0x84, A0)
# jal
a.j(3, 'func'); a.nop()
a.sw(V0, 0x88, A0)

# --- PI DMA: ROM 0x1000 -> RAM 0x1000 ---
a.lui(T0, 0xA460)
a.ori(T1, ZERO, 0x1000); a.sw(T1, 0, T0)
a.lui(T1, 0x1000); a.ori(T1, T1, 0x1000); a.sw(T1, 4, T0)
a.ori(T1, ZERO, 15); a.sw(T1, 0xC, T0)
a.lui(T2, 0xA000)
a.lw(T1, 0x1000, T2); a.sw(T1, 0x90, A0)

# --- SI / controller ---
a.li(T0, 0xFF010401); a.sw(T0, 0x100, A0)
a.li(T0, 0xFFFFFFFF); a.sw(T0, 0x104, A0)
a.li(T0, 0xFE000000); a.sw(T0, 0x108, A0)
a.lui(T3, 0xA480)
a.li(T4, RB + 0x100); a.sw(T4, 0, T3)
a.li(T5, 0x1FC007C0); a.sw(T5, 0x10, T3)      # WR64B
a.li(T4, RB + 0x200); a.sw(T4, 0, T3)
a.sw(T5, 0x4, T3)                             # RD64B
a.lw(T1, 0x204, A0); a.sw(T1, 0xA0, A0)


# ================= RSP test =================
def lwc2(op, vt, base, e, off): return (0x32 << 26) | (base << 21) | (vt << 16) | (op << 11) | (e << 7) | (off & 0x7F)
def swc2(op, vt, base, e, off): return (0x3A << 26) | (base << 21) | (vt << 16) | (op << 11) | (e << 7) | (off & 0x7F)
def vop(fn, vd, vs, vt, e=0): return (0x12 << 26) | (1 << 25) | (e << 21) | (vt << 16) | (vs << 11) | (vd << 6) | fn

def lis(rt, v):
    a.li(rt, v)

# data into DMEM @0x200 / 0x210
v1 = [0x4000, 2, 3, 4, 5, 6, 7, 0xFFF8]
v2 = [0x4000, 10, 20, 30, 40, 50, 60, 70]
a.lui(T2, 0xA400)
for i in range(4):
    a.li(T0, (v1[2*i] << 16) | v1[2*i+1]); a.sw(T0, 0x200 + 4*i, T2)
    a.li(T0, (v2[2*i] << 16) | v2[2*i+1]); a.sw(T0, 0x210 + 4*i, T2)
ucode = [
    lwc2(4, 1, 0, 0, 0x20),          # lqv v1[0], 0x200(r0)
    lwc2(4, 2, 0, 0, 0x21),          # lqv v2[0], 0x210(r0)
    vop(0x10, 3, 1, 2),              # vadd v3,v1,v2
    vop(0x00, 4, 1, 2),              # vmulf v4,v1,v2
    vop(0x30, 5, 0, 1, 0),           # vrcp v5[0], v1[0]
    swc2(4, 3, 0, 0, 0x22),          # sqv v3[0], 0x220
    swc2(4, 4, 0, 0, 0x23),          # sqv v4[0], 0x230
    swc2(4, 5, 0, 0, 0x24),          # sqv v5[0], 0x240
    I(0x09, 0, 2, 0x1234),           # addiu r2,r0,0x1234
    I(0x2B, 0, 2, 0x250),            # sw r2,0x250(r0)
    R(0, 0, 0, 0, 0x0D),             # break
    0,
]
a.lui(T3, 0xA400)
for i, w in enumerate(ucode):
    a.li(T0, w); a.sw(T0, 0x1000 + 4*i, T3)
a.lui(T4, 0xA408); a.sw(ZERO, 0, T4)                # SP_PC = 0
a.lui(T4, 0xA404); a.ori(T5, ZERO, 5); a.sw(T5, 0x10, T4)   # clear halt + clear broke
a.label('rsp_wait')
a.lw(T1, 0x10, T4)
a.emit(I(0x0C, T1, T1, 1))                          # andi t1,t1,1
a.br(4, T1, ZERO, 'rsp_wait')                       # beq t1,zero
a.nop()
for i in range(4):
    a.lw(T1, 0x220 + 4*i, T3); a.sw(T1, 0x300 + 4*i, A0)
    a.lw(T1, 0x230 + 4*i, T3); a.sw(T1, 0x310 + 4*i, A0)
    a.lw(T1, 0x240 + 4*i, T3); a.sw(T1, 0x320 + 4*i, A0)
a.lw(T1, 0x250, T3); a.sw(T1, 0x330, A0)

# ================= RDP test =================
FB = 0x200000
def px5551(y, x): return (((y * 4 + x + 1) << 11) | 1)
# texture data @ RAM 0xA000
a.lui(T1, 0xA000); a.ori(T1, T1, 0xA000)
for y in range(4):
    for k in range(2):
        a.li(T0, (px5551(y, 2*k) << 16) | px5551(y, 2*k+1)); a.sw(T0, (y*2 + k) * 4, T1)

cmds = []
cmds.append(((0x3F << 24) | (0 << 21) | (2 << 19) | 319, FB))                  # set color image
cmds.append(((0x2D << 24), (1280 << 12) | 960))                                # scissor
cmds.append(((0x2F << 24) | (3 << 20), 0))                                     # other modes: fill
cmds.append(((0x37 << 24), 0x07C107C1))                                        # fill color (green)
cmds.append(((0x36 << 24) | (400 << 12) | 400, 0))                             # fill rect 0,0..100,100
cmds.append(((0x2F << 24) | (0 << 20), 0))                                     # other modes: 1-cycle
cmds.append(((0x3C << 24) | (15 << 20) | (31 << 15) | (7 << 12) | (7 << 9) | (15 << 5) | 31,
             (15 << 28) | (15 << 24) | (7 << 21) | (7 << 18) | (4 << 15) | (7 << 12) | (4 << 9) | (4 << 6) | (7 << 3) | 4))
# shaded triangle: major edge left (x=50), minor right, apex region
tri = [ (0x0C << 24) | (1 << 23) | 600, (200 << 16) | 200,
        0x960000, 0xFFFF0000, 0x320000, 0, 0x960000, 0,
        (255 << 16) | 0, (0 << 16) | 255, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0 ]
for i in range(0, len(tri), 2): cmds.append((tri[i], tri[i+1]))
# texture: load 4x4 RGBA16 and draw in copy mode
cmds.append(((0x3D << 24) | (0 << 21) | (2 << 19) | 3, 0xA000))                # set texture image
cmds.append(((0x35 << 24) | (0 << 21) | (2 << 19) | (1 << 9) | 0, 0))          # set tile 0
cmds.append(((0x33 << 24), (15 << 12) | 0x800))                                # load block
cmds.append(((0x32 << 24), (12 << 12) | 12))                                   # tile size
cmds.append(((0x2F << 24) | (2 << 20), 0))                                     # other modes: copy
cmds.append(((0x24 << 24) | (52 << 12) | 52, (0 << 24) | (40 << 12) | 40))     # texrect
cmds.append((0, (0x1000 << 16) | 0x400))
cmds.append(((0x29 << 24), 0))                                                 # sync full
words = []
for hi, lo in cmds: words += [hi, lo]
a.lui(T1, 0xA000); a.ori(T1, T1, 0x9000)
for i, w in enumerate(words):
    a.li(T0, w); a.sw(T0, i * 4, T1)
a.lui(T3, 0xA410)
a.li(T4, 0x9000); a.sw(T4, 0, T3)
a.li(T4, 0x9000 + len(words) * 4); a.sw(T4, 4, T3)

# ================= TLB test =================
def mtc0(rt, rd): a.emit((0x10 << 26) | (4 << 21) | (rt << 16) | (rd << 11))
mtc0(ZERO, 0)                                 # Index = 0
a.li(T0, 0x70000000); mtc0(T0, 10)            # EntryHi: VPN2 0x70000000, ASID 0
a.li(T0, 0x81F); mtc0(T0, 2)                  # EntryLo0: PFN 0x20, C=3, D, V, G
a.li(T0, 0x85F); mtc0(T0, 3)                  # EntryLo1: PFN 0x21
mtc0(ZERO, 5)                                 # PageMask = 4 KB
a.emit(0x42000002)                            # TLBWI
a.li(T1, 0x70000000); a.li(T0, 0xCAFEBABE); a.sw(T0, 0, T1)      # store through mapped page
a.li(T1, 0xA0020000); a.lw(T2, 0, T1); a.sw(T2, 0x340, A0)       # read back via KSEG1 (phys 0x20000)
a.li(T1, 0xA0021000); a.li(T0, 0x12345678); a.sw(T0, 0, T1)      # write phys 0x21000
a.li(T1, 0x70001000); a.lw(T2, 0, T1); a.sw(T2, 0x344, A0)       # read through mapped odd page
# fetch through the TLB: copy "ori v0,zero,0x55 ; jr ra ; nop" to phys 0x22000 and call it via 0x70002000

a.li(T0, 0xD00DF00D); a.sw(T0, 0xF0, A0)
a.label('end')
a.j(2, 'end'); a.nop()
a.resolve()

ram = bytearray(0x4000)
for addr, w in a.w.items():
    assert addr < 0x4000, hex(addr)
    ram[addr:addr+4] = struct.pack('>I', w)

hdr = bytearray(0x1000)
hdr[0:4] = bytes([0x80, 0x37, 0x12, 0x40])
struct.pack_into('>I', hdr, 4, 0x0000000F)
struct.pack_into('>I', hdr, 8, 0x80000000)
hdr[0x20:0x20+20] = b'N64S60 TEST ROM'.ljust(20)
rom = bytes(hdr) + bytes(ram)
rom = rom.ljust(0x10000, b'\0')

def v64(b):
    o = bytearray(b)
    for i in range(0, len(o), 2): o[i], o[i+1] = o[i+1], o[i]
    return bytes(o)
def n64(b):
    o = bytearray(b)
    for i in range(0, len(o), 4): o[i:i+4] = o[i:i+4][::-1]
    return bytes(o)

out = sys.argv[1] if len(sys.argv) > 1 else '.'
open(out + '/testrom.z64', 'wb').write(rom)
open(out + '/testrom.v64', 'wb').write(v64(rom))
open(out + '/testrom.n64', 'wb').write(n64(rom))
print('ok, ram image bytes used: 0x%X' % (max(a.w) + 4))
