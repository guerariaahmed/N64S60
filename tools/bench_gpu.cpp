/* GPU speed: RSP (vector-heavy loop) and RDP (big shaded+textured triangles), interpreter vs cached */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define private public
#include "n64core.h"
#undef private
static u32 VecOp(u32 fn,u32 e,u32 vt,u32 vs,u32 vd){return (0x12u<<26)|(1u<<25)|(e<<21)|(vt<<16)|(vs<<11)|(vd<<6)|fn;}
int main()
    {
    for (int mode = 0; mode < 2; mode++)
        {
        u32* ram = (u32*)calloc(N64_RDRAM_BYTES/4,4); N64Core* c = new N64Core; c->Init(ram);
        c->SetGpuMode(mode);
        /* ---- RSP loop ---- */
        u32 p[] = {
            (0x0Fu<<26)|(1<<16)|0x0003,               /* lui r1,3 */
            (0x0Du<<26)|(1<<21)|(1<<16)|0x0D40,       /* ori r1,r1,0x0D40 (200000) */
            VecOp(6,0,3,2,1), VecOp(15,0,5,4,1), VecOp(16,0,2,1,6), VecOp(5,3,4,2,7), VecOp(0x28,0,7,6,8),
            VecOp(13,0,3,6,9), VecOp(0,2,3,2,10), VecOp(0x11,0,10,9,11),
            (0x09u<<26)|(1<<21)|(1<<16)|0xFFFF,       /* addiu r1,r1,-1 */
            (0x05u<<26)|(1<<21)|0xFFF7,               /* bne r1,r0,loop (-9) */
            0, 0x0Du };
        for (u32 i = 0; i < sizeof(p)/4; i++) c->iSp[1024+i] = p[i];
        for (u32 i = 0; i < 32; i++) for (u32 k = 0; k < 8; k++) c->iVr[i][k] = (u16)(i*977+k*131);
        c->iSpStatus = 0; c->iSpPc = 0; c->iRspNpc = 4; c->iRspDecDirty = true;
        clock_t t0 = clock();
        while (!(c->iSpStatus & 1)) c->RunRsp(100000);
        double s = (double)(clock()-t0)/CLOCKS_PER_SEC;
        printf("%-12s RSP %.1f M instr/s", mode ? "cached" : "interpreter", (double)c->iStats.rspInstr/s/1e6);

        /* ---- RDP: 400 big triangles (shade + texture + z), 16-bit fb ---- */
        c->RdpReset();
        c->iRdp.ciAddr = 0x200000; c->iRdp.ciSiz = 2; c->iRdp.ciWidth = 320; c->iRdp.ziAddr = 0x300000;
        c->iRdp.scXL = 1280; c->iRdp.scYL = 960; c->iRdp.omHi = 0; c->iRdp.omLo = 0x30;
        c->iRdp.combHi = (15<<20)|(31<<15)|(7<<12)|(7<<9)|(15<<5)|31;
        c->iRdp.combLo = (15u<<28)|(15<<24)|(7<<21)|(7<<18)|(4<<15)|(7<<12)|(4<<9)|(4<<6)|(7<<3)|4;   /* shade only */
        c->iRdp.pipeDirty = 1;
        u32 q[44] = {0};
        q[0] = (0x0Du<<24)|(1<<23)|900; q[1] = (0<<16)|0;                 /* shade+z, lft */
        q[2] = 300u<<16; q[3] = 0; q[4] = 10u<<16; q[5] = 0; q[6] = 300u<<16; q[7] = 0;
        q[8] = (200u<<16)|100; q[9] = (50u<<16)|255;
        q[10] = 0x00010002; q[11] = 0x00030004;
        q[24] = 0x3FFFFFF0; q[25] = 0x100;
        t0 = clock();
        for (int n = 0; n < 150; n++) { c->iRdp.fast = mode; c->RdpRun(0,0,false); }
        /* run via command buffer */
        for (u32 i = 0; i < 28; i++) ram[0x9000/4+i] = q[i];
        t0 = clock();
        for (int n = 0; n < 150; n++) c->RdpRun(0x9000, 0x9000 + 28*4, false);
        s = (double)(clock()-t0)/CLOCKS_PER_SEC;
        printf("   RDP %.1f M px/s\n", 150.0 * 290 * 225 / s / 1e6);
        }
    return 0;
    }
