/* Controller polling through the PIF: the game writes the command block ONCE, then only re-reads the
   result buffer (this is what Mario 64 / GoldenEye do).  The result must follow the live pad state. */
#include <stdio.h>
#include <stdlib.h>
#define private public
#include "n64core.h"
#undef private
static int fails = 0;
static void Check(const char* n, u32 g, u32 w) { if (g == w) printf("  ok   %-34s = %08X\n", n, g); else { printf("  FAIL %-34s = %08X (want %08X)\n", n, g, w); fails++; } }
int main()
    {
    u32* ram = (u32*)calloc(N64_RDRAM_BYTES / 4, 4);
    N64Core* c = new N64Core; c->Init(ram);
    /* command block: FF 01 04 01 FF FF FF FF  FE */
    ram[0x200 / 4 + 0] = 0xFF010401; ram[0x200 / 4 + 1] = 0xFFFFFFFF; ram[0x200 / 4 + 2] = 0xFE000000;
    c->iSi[0] = 0x200; c->SiDma(false);                    /* write command block once */
    c->iSi[0] = 0x300; c->SiDma(true);
    Check("first read, nothing pressed", ram[0x304 / 4], 0x00000000);
    c->SetInput(0x1000, 0, 0);                            /* hold START, NO new command written */
    c->iSi[0] = 0x300; c->SiDma(true);
    Check("re-read sees START (no rewrite)", ram[0x304 / 4], 0x10000000);
    c->SetInput(0, 0, 0);
    c->iSi[0] = 0x300; c->SiDma(true);
    Check("re-read after release", ram[0x304 / 4], 0x00000000);
    c->SetInput(0x8000, 0, 0); c->SetInput(0, 0, 0);       /* quick tap between two polls */
    c->iSi[0] = 0x300; c->SiDma(true);
    Check("quick tap latched until polled", ram[0x304 / 4], 0x80000000);
    c->iSi[0] = 0x300; c->SiDma(true);
    Check("tap delivered only once", ram[0x304 / 4], 0x00000000);
    c->PressButton(0x1000);
    c->iSi[0] = 0x300; c->SiDma(true);
    Check("menu 'Tap START'", ram[0x304 / 4], 0x10000000);
    c->SetInput(0, 50, -20);
    c->iSi[0] = 0x300; c->SiDma(true);
    Check("analog stick x=50 y=-20", ram[0x304 / 4], 0x0000 << 16 | (50 << 8) | 0xEC);
    printf("controller polls counted: %u\n", c->iStats.padPolls);
    printf(fails ? "PIF TEST FAILED\n" : "PIF TEST OK\n");
    return fails ? 1 : 0;
    }
