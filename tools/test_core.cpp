/*
 * PC self-test for the portable N64 core (not part of the Symbian build).
 *   python3 make_testrom.py .
 *   g++ -O2 -I../inc test_core.cpp ../src/n64core.cpp ../src/n64cpu.cpp -o test_core
 *   ./test_core testrom.z64 [testrom.v64 testrom.n64 ...]
 * Optional:  ./test_core some.z64 --dump out.ppm   (saves the VI framebuffer)
 */
#include "n64core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

class FileSrc : public MN64RomSource
    {
public:
    FILE* f;
    s32 ReadRom(u32 off, u8* dst, u32 len)
        {
        if (fseek(f, (long)off, SEEK_SET) != 0) return 0;
        return (s32)fread(dst, 1, len, f);
        }
    };

static int fails = 0;
static void Check(const char* name, u32 got, u32 want)
    {
    if (got == want) printf("  ok   %-22s = %08X\n", name, got);
    else { printf("  FAIL %-22s = %08X (want %08X)\n", name, got, want); fails++; }
    }

int main(int argc, char** argv)
    {
    const char* dump = 0;
    int cached = 0, gpu = 0; unsigned resq = 256;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--cached")) cached = 1; if (!strcmp(argv[i], "--gpu")) gpu = 1; if (!strncmp(argv[i], "--res=", 6)) resq = (unsigned)atoi(argv[i] + 6); }
    for (int i = 1; i < argc - 1; i++) if (!strcmp(argv[i], "--dump")) dump = argv[i + 1];

    for (int ai = 1; ai < argc; ai++)
        {
        if (!strcmp(argv[ai], "--dump")) { ai++; continue; }
        if (argv[ai][0] == '-' && argv[ai][1] == '-') continue;
        FileSrc src;
        src.f = fopen(argv[ai], "rb");
        if (!src.f) { printf("cannot open %s\n", argv[ai]); return 2; }
        fseek(src.f, 0, SEEK_END);
        long size = ftell(src.f);

        u32* ram = (u32*)malloc(N64_RDRAM_BYTES);
        N64Core* core = new N64Core;
        core->Init(ram);
        core->SetCpuMode((u32)cached);
        core->SetGpuMode((u32)gpu);
        core->SetResScale(resq);
        s32 r = core->LoadRom(&src, (u32)size);
        printf("== %s: LoadRom=%d name='%s' cic=%u\n", argv[ai], r, core->RomName(), core->CicType());
        if (r != 0) { fails++; continue; }

        core->SetInput(0x8000, 10, -5);
        clock_t t0 = clock();
        const int FRAMES = 30;
        for (int f = 0; f < FRAMES; f++) core->RunFrame();
        double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("  ran %d frames (%u instr each) in %.3fs  => %.1f M instr/s (PC host)\n",
               FRAMES, core->InstrPerFrame(), sec,
               sec > 0 ? (double)FRAMES * core->InstrPerFrame() / sec / 1e6 : 0.0);

        const u32 RB = 0x8000;
        Check("done marker", core->PeekRam32(RB + 0xF0), 0xD00DF00Du);
        Check("mul.s 1.5*2.0", core->PeekRam32(RB + 0x00), 0x40400000u);
        Check("cvt.d.s hi", core->PeekRam32(RB + 0x08), 0x40080000u);
        Check("cvt.d.s lo", core->PeekRam32(RB + 0x0C), 0u);
        Check("trunc.w 3.7", core->PeekRam32(RB + 0x10), 3u);
        Check("round.w 3.7", core->PeekRam32(RB + 0x14), 4u);
        Check("floor.w -1.5", core->PeekRam32(RB + 0x18), 0xFFFFFFFEu);
        Check("ceil.w -1.5", core->PeekRam32(RB + 0x1C), 0xFFFFFFFFu);
        Check("sqrt.s 9.0", core->PeekRam32(RB + 0x20), 0x40400000u);
        Check("c.lt.s + bc1t", core->PeekRam32(RB + 0x24), 1u);
        Check("div.s 1/4", core->PeekRam32(RB + 0x28), 0x3E800000u);
        Check("cvt.s.w 7", core->PeekRam32(RB + 0x2C), 0x40E00000u);
        Check("mult lo", core->PeekRam32(RB + 0x40), 0x540BE400u);
        Check("mult hi", core->PeekRam32(RB + 0x44), 2u);
        Check("div quotient", core->PeekRam32(RB + 0x48), 14u);
        Check("div remainder", core->PeekRam32(RB + 0x4C), 2u);
        Check("dsll32 sd hi", core->PeekRam32(RB + 0x50), 1u);
        Check("dsll32 sd lo", core->PeekRam32(RB + 0x54), 0u);
        Check("lwl+lwr", core->PeekRam32(RB + 0x68), 0x22334455u);
        Check("swl word0", core->PeekRam32(RB + 0x70), 0x00010203u);
        Check("swr word1", core->PeekRam32(RB + 0x74), 0x04000000u);
        Check("beql annul", core->PeekRam32(RB + 0x80), 1u);
        Check("bnel taken", core->PeekRam32(RB + 0x84), 6u);
        Check("jal/jr", core->PeekRam32(RB + 0x88), 0x77u);
        Check("PI DMA", core->PeekRam32(RB + 0x90), 0x08000100u);
        Check("SI joybus poll", core->PeekRam32(RB + 0xA0), 0x80000AFBu);
        Check("tlb store/load (even page)", core->PeekRam32(RB + 0x340), 0xCAFEBABEu);
        Check("tlb load (odd page)", core->PeekRam32(RB + 0x344), 0x12345678u);
        Check("rsp vadd w0", core->PeekRam32(RB + 0x300), 0x7FFF000Cu);
        Check("rsp vadd w1", core->PeekRam32(RB + 0x304), 0x00170022u);
        Check("rsp vadd w2", core->PeekRam32(RB + 0x308), 0x002D0038u);
        Check("rsp vadd w3", core->PeekRam32(RB + 0x30C), 0x0043003Eu);
        Check("rsp vmulf w0", core->PeekRam32(RB + 0x310), 0x20000000u);
        Check("rsp vmulf w1", core->PeekRam32(RB + 0x314), 0u);
        Check("rsp vrcp w0", core->PeekRam32(RB + 0x320), 0xFFFF0000u);
        Check("rsp scalar sw", core->PeekRam32(RB + 0x330), 0x1234u);
        {
        #define FBPX(x, y) ({ u32 a_ = 0x200000u + ((y) * 320u + (x)) * 2u; u32 w_ = core->PeekRam32(a_ & ~3u); (a_ & 2) ? (w_ & 0xFFFFu) : (w_ >> 16); })
        Check("rdp fill (20,20)", FBPX(20, 20), 0x07C1u);
        Check("rdp fill (150,150) untouched", FBPX(150, 150), 0u);
        Check("rdp shaded tri (60,60)", FBPX(60, 60), 0xF801u);
        Check("rdp tri outside (140,140)", FBPX(140, 140), 0u);
        Check("rdp tri inside (61,61)", FBPX(61, 61), 0xF801u);
        Check("rdp tri inside (80,70)", FBPX(80, 70), 0xF801u);
        Check("rdp texrect (10,10)", FBPX(10, 10), 0x0801u);
        Check("rdp texrect (12,11)", FBPX(12, 11), 0x3801u);
        Check("rdp texrect (13,13)", FBPX(13, 13), 0x8001u);
        Check("rdp texrect edge (14,13) = fill", FBPX(14, 13), 0x07C1u);
        }
        printf("  stats: rspRuns=%u rspInstr=%u rdpCmds=%u tris=%u rects=%u unkCpu=%u unkRsp=%u unkRdp=%u\n",
               core->Stats().rspRuns, core->Stats().rspInstr, core->Stats().rdpCmds, core->Stats().rdpTris,
               core->Stats().rdpRects, core->Stats().unkCpu, core->Stats().unkRsp, core->Stats().unkRdp);
        u32 irq = core->PeekRam32(0x104);
        printf("  VI interrupts handled: %u\n", irq);
        if (irq < 10) { printf("  FAIL interrupt count\n"); fails++; }

        u16* fb = (u16*)malloc(320 * 240 * 2);
        core->Render565(fb, 320, 240, 320);
        Check("framebuffer px(0,0)", fb[0], 0xF800u);
        Check("framebuffer px(319,239)", fb[239 * 320 + 319], 0xF800u);
        u16* small = (u16*)malloc(240 * 160 * 2);
        core->Render565(small, 240, 160, 240);
        Check("scaled px(100,100)", small[100 * 240 + 100], 0xF800u);

        if (dump)
            {
            FILE* o = fopen(dump, "wb");
            fprintf(o, "P6\n320 240\n255\n");
            for (int i = 0; i < 320 * 240; i++)
                {
                u16 p = fb[i];
                unsigned char rgb[3] = { (unsigned char)((p >> 11) << 3), (unsigned char)(((p >> 5) & 63) << 2), (unsigned char)((p & 31) << 3) };
                fwrite(rgb, 1, 3, o);
                }
            fclose(o);
            }
        free(fb); free(small); free(ram); delete core;
        fclose(src.f);
        }
    printf(fails ? "\n*** %d FAILURES ***\n" : "\nALL TESTS PASSED\n", fails);
    return fails ? 1 : 0;
    }
