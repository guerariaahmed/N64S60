/*
 * Differential fuzz test: random RSP microcode and random RDP command streams
 * are run through the GPU interpreter and the GPU cached mode; every bit of
 * resulting state (vector regs, accumulators, DMEM, RDRAM, TMEM...) must match.
 *
 *   g++ -O2 -I../inc fuzz_gpu.cpp ../src/n64core.cpp ../src/n64cpu.cpp ../src/n64rsp.cpp ../src/n64rdp.cpp -o fuzz_gpu
 *   ./fuzz_gpu [rsp_iterations] [rdp_iterations]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define private public
#include "n64core.h"
#undef private

static u32 g_seed = 12345;
static u32 rnd() { g_seed = g_seed * 1664525u + 1013904223u; return (g_seed >> 8) | (g_seed << 24); }
static u32 rr(u32 n) { return rnd() % n; }

static void RandomState(N64Core* a, N64Core* b)
    {
    u32 i, k;
    for (i = 0; i < 32; i++)
        {
        u32 r0 = rnd() | 1u;
        a->iRspR[i] = b->iRspR[i] = (i == 0) ? 0 : r0;
        for (k = 0; k < 8; k++) { u16 v = (u16)rnd(); a->iVr[i][k] = b->iVr[i][k] = v; }
        }
    for (i = 0; i < 8; i++)
        {
        s64 acc = (s64)((((u64)rnd() << 32) | rnd()) << 16) >> 16;
        a->iAcc[i] = b->iAcc[i] = acc;
        }
    a->iVco = b->iVco = (u16)rnd(); a->iVcc = b->iVcc = (u16)rnd(); a->iVce = b->iVce = (u8)rnd();
    a->iDivIn = b->iDivIn = (u16)rnd(); a->iDivOut = b->iDivOut = (s32)rnd(); a->iDivDp = b->iDivDp = (rnd() & 1) != 0;
    for (i = 0; i < 1024; i++) { u32 w = rnd(); a->iSp[i] = b->iSp[i] = w; }
    }

static int CompareRsp(N64Core* a, N64Core* b, int it)
    {
    u32 i, k;
    for (i = 0; i < 32; i++)
        {
        if (a->iRspR[i] != b->iRspR[i]) { printf("rsp it %d: scalar r%u differs\n", it, i); return 1; }
        for (k = 0; k < 8; k++)
            if (a->iVr[i][k] != b->iVr[i][k]) { printf("rsp it %d: v%u[%u] %04x != %04x\n", it, i, k, a->iVr[i][k], b->iVr[i][k]); return 1; }
        }
    for (i = 0; i < 8; i++) if (a->iAcc[i] != b->iAcc[i]) { printf("rsp it %d: acc[%u] differs\n", it, i); return 1; }
    if (a->iVco != b->iVco || a->iVcc != b->iVcc || a->iVce != b->iVce) { printf("rsp it %d: flags differ\n", it); return 1; }
    if (a->iDivIn != b->iDivIn || a->iDivOut != b->iDivOut || a->iDivDp != b->iDivDp) { printf("rsp it %d: div state differs\n", it); return 1; }
    for (i = 0; i < 2048; i++) if (a->iSp[i] != b->iSp[i]) { printf("rsp it %d: %s word %u differs\n", it, i < 1024 ? "DMEM" : "IMEM", i & 1023); return 1; }
    if (a->iSpPc != b->iSpPc || a->iSpStatus != b->iSpStatus) { printf("rsp it %d: pc/status differ\n", it); return 1; }
    return 0;
    }

static u32 VecOp(u32 fn, u32 e, u32 vt, u32 vs, u32 vd)
    { return (0x12u << 26) | (1u << 25) | (e << 21) | (vt << 16) | (vs << 11) | (vd << 6) | fn; }

static int FuzzRsp(int iters)
    {
    static const u8 fns[] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,19,20,21,29,32,33,34,35,36,37,38,39,
                              40,41,42,43,44,45,48,49,50,51,52,53,54,55 };
    static const u8 ldops[] = { 0,1,2,3,4,5,6,7,8,9,11 };
    u32* ramA = (u32*)calloc(N64_RDRAM_BYTES / 4, 4);
    u32* ramB = (u32*)calloc(N64_RDRAM_BYTES / 4, 4);
    N64Core* a = new N64Core; N64Core* b = new N64Core;
    a->Init(ramA); b->Init(ramB);
    a->SetGpuMode(0); b->SetGpuMode(1);
    for (int it = 0; it < iters; it++)
        {
        RandomState(a, b);
        u32 n = 40 + rr(100), i;
        u32 prog[400];
        for (i = 0; i < n; i++)
            {
            u32 t = rr(100), w;
            if (t < 45) w = VecOp(fns[rr(sizeof(fns))], rr(16), rr(32), rr(32), rr(32));
            else if (t < 62) w = (0x32u << 26) | (rr(32) << 16) | ((u32)ldops[rr(sizeof(ldops))] << 11) | (rr(16) << 7) | rr(128);
            else if (t < 78) w = (0x3Au << 26) | (rr(32) << 16) | (rr(12) << 11) | (rr(16) << 7) | rr(128);
            else
                {
                u32 rs = rr(32), rt = rr(32), rd = rr(32);
                switch (rr(10))
                    {
                    case 0: w = (0x09u << 26) | (rs << 21) | (rt << 16) | rr(0x10000); break;
                    case 1: w = (0x0Du << 26) | (rs << 21) | (rt << 16) | rr(0x10000); break;
                    case 2: w = (0x0Fu << 26) | (rt << 16) | rr(0x10000); break;
                    case 3: w = (rs << 21) | (rt << 16) | (rd << 11) | (rr(32) << 6) | 0; break;
                    case 4: w = (rs << 21) | (rt << 16) | (rd << 11) | 0x26; break;
                    case 5: w = (0x12u << 26) | (4u << 21) | (rt << 16) | (rd << 11) | (rr(16) << 7); break;   /* MTC2 */
                    case 6: w = (0x12u << 26) | (0u << 21) | (rt << 16) | (rd << 11) | (rr(16) << 7); break;   /* MFC2 */
                    case 7: w = (0x12u << 26) | (2u << 21) | (rt << 16) | (rd << 11); break;                   /* CFC2 */
                    case 8: w = (0x12u << 26) | (6u << 21) | (rt << 16) | (rd << 11); break;                   /* CTC2 */
                    default: w = (0x23u << 26) | (rs << 21) | (rt << 16) | rr(0x1000); break;                  /* LW */
                    }
                }
            prog[i] = w;
            }
        if (n > 60 && rr(3) == 0)                                  /* overlay: DMA new code into IMEM while running */
            {
            u32 k = rr(n - 40);
            u32 dst = k + 8;                                       /* instruction index that gets replaced */
            prog[k + 0] = (0x0Du << 26) | (0 << 21) | (1 << 16) | (0x1000 + dst * 4);   /* ori r1,r0,imem addr */
            prog[k + 1] = (0x10u << 26) | (4u << 21) | (1 << 16) | (0 << 11);           /* mtc0 r1,SP_MEM_ADDR */
            prog[k + 2] = (0x0Du << 26) | (0 << 21) | (2 << 16) | 0x2000;               /* ori r2,r0,0x2000 */
            prog[k + 3] = (0x10u << 26) | (4u << 21) | (2 << 16) | (1 << 11);           /* mtc0 r2,SP_DRAM_ADDR */
            prog[k + 4] = (0x0Du << 26) | (0 << 21) | (3 << 16) | 63;                   /* ori r3,r0,63 (64 bytes) */
            prog[k + 5] = (0x10u << 26) | (4u << 21) | (3 << 16) | (2 << 11);           /* mtc0 r3,SP_RD_LEN */
            for (u32 j = 0; j < 16; j++)
                {
                u32 w = VecOp(fns[rr(sizeof(fns))], rr(16), rr(32), rr(32), rr(32));
                ramA[0x2000 / 4 + j] = ramB[0x2000 / 4 + j] = w;
                }
            }
        prog[n++] = 0x0000000Du;                                   /* BREAK */
        for (i = 0; i < 1024; i++) { u32 w = (i < n) ? prog[i] : 0; a->iSp[1024 + i] = b->iSp[1024 + i] = w; }
        a->iSpPc = b->iSpPc = 0; a->iRspNpc = b->iRspNpc = 4;
        a->iSpStatus = b->iSpStatus = 0;
        a->iRspDecDirty = b->iRspDecDirty = true;
        a->RunRsp(100000); b->RunRsp(100000);
        if (CompareRsp(a, b, it)) return 1;
        }
    printf("RSP fuzz: %d random microcode programs identical in both modes (%u RSP instructions executed)\n", iters, (unsigned)a->iStats.rspInstr);
    return 0;
    }

/* ---------------- RDP ---------------- */
static u32 cmd[8192];
static u32 ncmd;
static void C(u32 h, u32 l) { cmd[ncmd++] = h; cmd[ncmd++] = l; }
static u32 S16(s32 v) { return (u32)v & 0xFFFFu; }

static void RandomTriangle()
    {
    u32 type = 8 + rr(8);
    bool shade = (type & 4) != 0, tex = (type & 2) != 0, z = (type & 1) != 0;
    s32 yh = (s32)rr(900), yl = yh + 4 + (s32)rr(500);
    if (yl > 959) yl = 959;
    if (yh >= yl) yh = yl - 4;
    s32 ym = yh + (s32)rr((u32)(yl - yh + 1));
    u32 lft = rr(2), tile = rr(6);
    C((type << 24) | (lft << 23) | (tile << 16) | ((u32)yl & 0x3FFF), (((u32)ym & 0x3FFF) << 16) | ((u32)yh & 0x3FFF));
    s32 xa = (s32)(rr(320 * 4)) << 14, xb = (s32)(rr(320 * 4)) << 14;
    C((u32)xb, (u32)((s32)rr(0x40000) - 0x20000));
    C((u32)xa, (u32)((s32)rr(0x40000) - 0x20000));
    C((u32)xb, (u32)((s32)rr(0x40000) - 0x20000));
    if (shade)
        {
        u32 r = rr(256), g = rr(256), bl = rr(256), al = rr(256);
        C((r << 16) | g, (bl << 16) | al);
        C((S16((s32)rr(40) - 20) << 16) | S16((s32)rr(40) - 20), (S16((s32)rr(40) - 20) << 16) | S16((s32)rr(40) - 20));
        C((rr(0x10000) << 16) | rr(0x10000), (rr(0x10000) << 16) | rr(0x10000));
        C((rr(0x10000) << 16) | rr(0x10000), (rr(0x10000) << 16) | rr(0x10000));
        C((S16((s32)rr(40) - 20) << 16) | S16((s32)rr(40) - 20), (S16((s32)rr(40) - 20) << 16) | S16((s32)rr(40) - 20));
        C((S16((s32)rr(40) - 20) << 16) | S16((s32)rr(40) - 20), (S16((s32)rr(40) - 20) << 16) | S16((s32)rr(40) - 20));
        C((rr(0x10000) << 16) | rr(0x10000), (rr(0x10000) << 16) | rr(0x10000));
        C((rr(0x10000) << 16) | rr(0x10000), (rr(0x10000) << 16) | rr(0x10000));
        }
    if (tex)
        {
        C((rr(0x400) << 16) | rr(0x400), (0x7000 + rr(0x1000)) << 16);
        C((S16((s32)rr(60) - 30) << 16) | S16((s32)rr(60) - 30), S16((s32)rr(10) - 5) << 16);
        C((rr(0x10000) << 16) | rr(0x10000), rr(0x10000) << 16);
        C((rr(0x10000) << 16) | rr(0x10000), rr(0x10000) << 16);
        C((S16((s32)rr(60) - 30) << 16) | S16((s32)rr(60) - 30), S16((s32)rr(10) - 5) << 16);
        C((S16((s32)rr(60) - 30) << 16) | S16((s32)rr(60) - 30), S16((s32)rr(10) - 5) << 16);
        C((rr(0x10000) << 16) | rr(0x10000), rr(0x10000) << 16);
        C((rr(0x10000) << 16) | rr(0x10000), rr(0x10000) << 16);
        }
    if (z)
        {
        C(rr(0x7FFFFFFF), (u32)((s32)rr(0x20000) - 0x10000));
        C((u32)((s32)rr(0x20000) - 0x10000), (u32)((s32)rr(0x20000) - 0x10000));
        }
    }

static void BuildRdpProgram()
    {
    ncmd = 0;
    u32 siz = (rr(3) == 0) ? 3 : 2;
    C((0x3Fu << 24) | (0 << 21) | (siz << 19) | 319, 0x200000);
    C(0x3Eu << 24, 0x300000);
    C(0x2Du << 24, (1280u << 12) | 960);
    C((0x3Du << 24) | (rr(5) << 21) | (rr(4) << 19) | (rr(100) + 8), 0xA000);
    for (u32 t = 0; t < 4; t++)
        C((0x35u << 24) | (rr(5) << 21) | (rr(4) << 19) | ((1 + rr(8)) << 9) | rr(0x1F0),
          (t << 24) | (rr(16) << 20) | (rr(4) << 18) | (rr(6) << 14) | (rr(12) << 10) | (rr(4) << 8) | (rr(6) << 4) | rr(12));
    for (u32 t = 0; t < 4; t++)
        {
        C((0x32u << 24) | (rr(8) << 12) | rr(8), (t << 24) | ((16 + rr(100)) << 12) | (16 + rr(100)));
        if (rr(2)) C((0x33u << 24) | (rr(4) << 12) | rr(4), (t << 24) | (rr(500) << 12) | rr(0x800));
        else C((0x34u << 24) | (rr(16) << 12) | rr(16), (t << 24) | ((20 + rr(120)) << 12) | (20 + rr(120)));
        }
    if (rr(2)) C((0x30u << 24), (255u << 14));
    C((0x3Au << 24) | rr(256), rnd());
    C(0x3Bu << 24, rnd());
    C(0x39u << 24, rnd());
    C(0x38u << 24, rnd());
    C(0x37u << 24, rnd());
    for (u32 g = 0; g < 6; g++)
        {
        u32 cyc = (rr(6) == 0) ? 2 + rr(2) : rr(2);
        C((0x2Fu << 24) | (cyc << 20) | (rr(2) << 19) | (rr(2) << 15) | (rr(2) << 14) | (rr(2) << 13),
          (rnd() & 0xFFFF0000u) | (rnd() & 0x7FFF));
        C((0x3Cu << 24) | (rnd() & 0xFFFFFF), rnd());
        u32 nt = 1 + rr(5);
        for (u32 i = 0; i < nt; i++) RandomTriangle();
        if (rr(2)) C((0x36u << 24) | (rr(1000) << 12) | rr(900), (rr(500) << 12) | rr(400));
        if (rr(2))
            {
            C((0x24u << 24) | ((200 + rr(600)) << 12) | (200 + rr(600)), (rr(4) << 24) | (rr(200) << 12) | rr(200));
            C((rr(0x400) << 16) | rr(0x400), (S16((s32)rr(0x2000) - 0x1000) << 16) | S16((s32)rr(0x2000) - 0x1000));
            }
        }
    C(0x29u << 24, 0);
    }

static int FuzzRdp(int iters)
    {
    u32* ramA = (u32*)calloc(N64_RDRAM_BYTES / 4, 4);
    u32* ramB = (u32*)calloc(N64_RDRAM_BYTES / 4, 4);
    N64Core* a = new N64Core; N64Core* b = new N64Core;
    a->Init(ramA); b->Init(ramB);
    unsigned long long tris = 0, changed = 0, rects = 0;
    for (int it = 0; it < iters; it++)
        {
        u32 i;
        for (i = 0; i < N64_RDRAM_BYTES / 4; i++) ramA[i] = ramB[i] = 0;
        for (i = 0; i < 0x4000 / 4; i++) { u32 w = rnd(); ramA[0xA000 / 4 + i] = ramB[0xA000 / 4 + i] = w; }
        for (i = 0; i < 320 * 240 * 4 / 4; i++) { u32 w = rnd(); ramA[0x200000 / 4 + i] = ramB[0x200000 / 4 + i] = w; }
        for (i = 0; i < 320 * 240 * 2 / 4; i++) { u32 w = (rnd() & 0x3FFFC000u) | 0x00FF0000u; ramA[0x300000 / 4 + i] = ramB[0x300000 / 4 + i] = w; }
        BuildRdpProgram();
        for (i = 0; i < ncmd; i++) { ramA[0x9000 / 4 + i] = ramB[0x9000 / 4 + i] = cmd[i]; }
        u32 res = rr(4);
        u32 qs = res == 0 ? 256 : (res == 1 ? 230 : (res == 2 ? 192 : 128));
        u32 dbg = (rr(3) == 0) ? rr(16) : 0;
        a->SetGpuMode(0); b->SetGpuMode(1);
        a->iResQ = b->iResQ = qs; a->iGfxDbg = b->iGfxDbg = dbg;
        a->RdpReset(); b->RdpReset();
        a->iStats.rdpTris = a->iStats.rdpRects = 0;
        static u32 before[320 * 240];
        for (i = 0; i < 320 * 240; i++) before[i] = ramA[0x200000 / 4 + i];
        a->RdpRun(0x9000, 0x9000 + ncmd * 4, false);
        b->RdpRun(0x9000, 0x9000 + ncmd * 4, false);
        for (i = 0; i < N64_RDRAM_BYTES / 4; i++)
            if (ramA[i] != ramB[i])
                { printf("rdp it %d: RDRAM word 0x%X differs (%08x vs %08x) [res %u dbg %u]\n", it, i * 4, ramA[i], ramB[i], qs, dbg); return 1; }
        if (memcmp(a->iRdp.tmem, b->iRdp.tmem, 4096)) { printf("rdp it %d: TMEM differs\n", it); return 1; }
        tris += a->iStats.rdpTris; rects += a->iStats.rdpRects;
        for (i = 0; i < 320 * 240; i++) if (before[i] != ramA[0x200000 / 4 + i]) changed++;
        }
    printf("RDP fuzz: %d random command streams identical in both modes (%llu triangles, %llu rects, %llu framebuffer words changed)\n", iters, tris, rects, changed);
    return 0;
    }

int main(int argc, char** argv)
    {
    int rsp = argc > 1 ? atoi(argv[1]) : 3000;
    int rdp = argc > 2 ? atoi(argv[2]) : 300;
    if (FuzzRsp(rsp)) return 1;
    if (FuzzRdp(rdp)) return 1;
    printf("ALL GPU FUZZ TESTS PASSED\n");
    return 0;
    }
