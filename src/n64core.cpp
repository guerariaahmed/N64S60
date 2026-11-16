/*
 * n64core.cpp - see n64core.h.  CPU/FPU interpreter lives in n64cpu.cpp,
 * this file holds memory map, devices, boot and video output.
 */
#include "n64core.h"

#define RDRAM_SIZE N64_RDRAM_SIZE

/* ------------------------------------------------------------------ */
/* tiny helpers (no libc)                                              */
/* ------------------------------------------------------------------ */
static void ZeroWords(u32* p, u32 n)
    {
    while (n--) *p++ = 0;
    }

static u32 BE32(const u8* p)
    {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    }

/* ------------------------------------------------------------------ */
/* init / boot                                                         */
/* ------------------------------------------------------------------ */
void N64Core::Init(u32* aRdram)
    {
    iRam = aRdram;
    iSrc = 0;
    iRomSize = 0;
    iRomFmt = 0;
    iCic = 6102;
    iName[0] = 0;
    iButtons = 0;
    iStickX = 0;
    iStickY = 0;
    iPrevHeld = 0; iLatched = 0; iLatX = 0; iLatY = 0; iLatStick = false; iPifActive = false;
    iGpuMode = 0; iGfxDbg = 0; iRspDecDirty = true;
    iInstrPerFrame = 300000;
    iFrames = 0;
    iFrameChunk = 0;
    iCpuMode = 0;
    iResQ = 256;
    iBlkEpoch = 1;
    iQuiet = false;
    iCountStep = 5;
    iCount = 0;
    iPC = iNPC = 0;
    iHI = iLO = 0;
    iBranchPrev = iDelaySlot = iLLbit = false;
    iFcr31 = 0;
    iSpMemAddr = iSpDramAddr = iSpStatus = iSpSema = iSpPc = 0;
    iMiMode = iMiIntr = iMiMask = 0;
    iViCurrent = 0;
    iAiLen = 0;
    u32 i;
    for (i = 0; i < 32; i++) { iR[i] = 0; iF[i] = 0; iCp0[i] = 0; }
    for (i = 0; i < 14; i++) iVi[i] = 0;
    for (i = 0; i < 8; i++)  { iDpc[i] = 0; iRi[i] = 0; }
    for (i = 0; i < 6; i++)  iAi[i] = 0;
    for (i = 0; i < 13; i++) iPi[i] = 0;
    for (i = 0; i < 7; i++)  iSi[i] = 0;
    for (i = 0; i < 64; i++) iPif[i] = 0;
    for (i = 0; i < 32; i++) { iTlb[i].mask = iTlb[i].hi = iTlb[i].lo0 = iTlb[i].lo1 = 0; }
    FlushTlbCache();
    iCurPc = 0; iFault = false;
    ZeroWords(iSp, 2048);
    ZeroWords(iRam, RDRAM_SIZE / 4);
    for (i = 0; i < 512; i++) iEeprom[i] = 0xFF;
    for (i = 0; i < 1024; i++) { iPageGen[i] = 0; iCodePage[i] = 0; iBlk[i].pc = 0; iBlk[i].epoch = 0; iBlk[i].gen = 0; iBlk[i].page = 0; iBlk[i].len = 0; }
    iDpcFlags = 0;
    iRspNpc = 4;
    }

/* Presses are latched until the game has read them at least once, so a short
   tap is not lost when the game polls the pad only every few seconds. */
void N64Core::SetInput(u16 aButtons, s8 aStickX, s8 aStickY)
    {
    iLatched |= (u16)(aButtons & ~iPrevHeld);          /* newly pressed */
    iPrevHeld = aButtons;
    iButtons = aButtons;
    iStickX = aStickX;
    iStickY = aStickY;
    if (aStickX != 0 || aStickY != 0) { iLatX = aStickX; iLatY = aStickY; iLatStick = true; }
    }

void N64Core::InvalRange(u32 aAddr, u32 aLen)
    {
    if (aLen == 0 || aAddr >= RDRAM_SIZE) return;
    u32 last = aAddr + aLen - 1;
    if (last >= RDRAM_SIZE) last = RDRAM_SIZE - 1;
    for (u32 pg = aAddr >> 12; pg <= (last >> 12); pg++)
        if (iCodePage[pg]) { iCodePage[pg] = 0; iPageGen[pg]++; }
    }

void N64Core::SetResScale(u32 aQ)
    {
    if (aQ < 64) aQ = 64;
    if (aQ > 256) aQ = 256;
    iResQ = aQ;
    iRdp.resQ = aQ;
    }

void N64Core::SetInstrPerFrame(u32 aN)
    {
    if (aN < 4096) aN = 4096;
    if (aN > 4000000) aN = 4000000;
    iInstrPerFrame = aN;
    /* real CPU: Count ticks at 46.875 MHz => 781250 ticks per 1/60 s */
    u32 s = 781250u / aN;
    iCountStep = s ? s : 1;
    }

s32 N64Core::LoadRom(MN64RomSource* aSrc, u32 aRomSize)
    {
    u8 h[4];
    if (!aSrc || aRomSize < 0x2000) return -1;
    if (aSrc->ReadRom(0, h, 4) != 4) return -2;

    if (h[0] == 0x80 && h[1] == 0x37 && h[2] == 0x12 && h[3] == 0x40) iRomFmt = 0;
    else if (h[0] == 0x37 && h[1] == 0x80 && h[2] == 0x40 && h[3] == 0x12) iRomFmt = 1;
    else if (h[0] == 0x40 && h[1] == 0x12 && h[2] == 0x37 && h[3] == 0x80) iRomFmt = 2;
    else return -3;

    iSrc = aSrc;
    iRomSize = aRomSize;
    SetInstrPerFrame(iInstrPerFrame);
    Reset();
    return 0;
    }

/* Reads ROM bytes in canonical big-endian (z64) order. */
s32 N64Core::RomRead(u32 aOff, u8* aDst, u32 aLen)
    {
    u8* t = iRomBuf;
    s32 total = 0;
    if (!iSrc) { while (aLen--) *aDst++ = 0; return 0; }
    while (aLen)
        {
        u32 a = aOff & ~3u;
        u32 skip = aOff - a;
        u32 want = skip + aLen;
        u32 n = 2048;
        if (want < n) n = (want + 3) & ~3u;

        s32 got = 0;
        if (a < iRomSize) got = iSrc->ReadRom(a, t, n);
        if (got < 0) got = 0;
        for (u32 z = (u32)got; z < n; z++) t[z] = 0;

        if (iRomFmt == 1)
            {
            for (u32 i = 0; i < n; i += 2) { u8 x = t[i]; t[i] = t[i+1]; t[i+1] = x; }
            }
        else if (iRomFmt == 2)
            {
            for (u32 i = 0; i < n; i += 4)
                {
                u8 a0 = t[i], a1 = t[i+1];
                t[i] = t[i+3]; t[i+1] = t[i+2]; t[i+2] = a1; t[i+3] = a0;
                }
            }

        u32 cnt = n - skip;
        if (cnt > aLen) cnt = aLen;
        for (u32 i = 0; i < cnt; i++) aDst[i] = t[skip + i];
        aOff += cnt;
        aDst += cnt;
        aLen -= cnt;
        total += (s32)cnt;
        }
    return total;
    }

void N64Core::RamWriteBytes(u32 aAddr, const u8* b, u32 n)
    {
    aAddr &= 0x00FFFFFFu;
    InvalRange(aAddr, n);
    while (n)
        {
        if (aAddr >= RDRAM_SIZE) break;
        if (!(aAddr & 3) && n >= 4)
            {
            iRam[aAddr >> 2] = BE32(b);
            aAddr += 4; b += 4; n -= 4;
            }
        else
            {
            u32 sh = (3 - (aAddr & 3)) * 8;
            u32 w = iRam[aAddr >> 2];
            iRam[aAddr >> 2] = (w & ~(0xFFu << sh)) | ((u32)*b << sh);
            aAddr++; b++; n--;
            }
        }
    }

void N64Core::DmaRomToRam(u32 aRomOff, u32 aRamAddr, u32 aLen)
    {
    u8* buf = iDmaBuf;
    while (aLen)
        {
        u32 n = aLen > 2048 ? 2048 : aLen;
        RomRead(aRomOff, buf, n);
        RamWriteBytes(aRamAddr, buf, n);
        aRomOff += n; aRamAddr += n; aLen -= n;
        }
    }

static u32 Crc32Update(u32 crc, const u8* p, u32 n)
    {
    while (n--)
        {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    return crc;
    }

void N64Core::Reset()
    {
    u32 i;
    if (!iSrc) return;
    Trace("core: reset begin");

    for (i = 0; i < 32; i++) { iR[i] = 0; iF[i] = 0; iCp0[i] = 0; }
    iHI = iLO = 0;
    iBranchPrev = false;
    iDelaySlot = false;
    iLLbit = false;
    iCount = 0;
    iFcr31 = 0;
    iFrames = 0;
    iFrameChunk = 0;
    ZeroWords(iRam, RDRAM_SIZE / 4);
    ZeroWords(iSp, 2048);
    for (i = 0; i < 64; i++) iPif[i] = 0;

    iSpMemAddr = iSpDramAddr = iSpSema = iSpPc = 0;
    iSpStatus = 1;                      /* halted */
    iRspNpc = 4;
    iRspDecDirty = true;
    iPrevHeld = 0; iLatched = 0; iLatStick = false; iPifActive = false;
    for (i = 0; i < 32; i++) { iRspR[i] = 0; for (u32 k = 0; k < 8; k++) iVr[i][k] = 0; }
    for (i = 0; i < 8; i++) iAcc[i] = 0;
    iVco = iVcc = 0; iVce = 0; iDivIn = 0; iDivOut = 0; iDivDp = false;
    iDpcFlags = 0;
    for (i = 0; i < 32; i++) { iTlb[i].mask = iTlb[i].hi = iTlb[i].lo0 = iTlb[i].lo1 = 0; }
    FlushTlbCache();
    for (i = 0; i < 1024; i++) { iCodePage[i] = 0; iPageGen[i]++; }
    iFault = false;
    RdpReset();
    {
    u32* sp = (u32*)&iStats;
    for (i = 0; i < sizeof(N64Stats) / 4; i++) sp[i] = 0;
    }
    for (i = 0; i < 8; i++) iDpc[i] = 0;
    iMiMode = 0; iMiIntr = 0; iMiMask = 0;
    for (i = 0; i < 14; i++) iVi[i] = 0;
    iViCurrent = 0;
    for (i = 0; i < 6; i++) iAi[i] = 0;
    iAiLen = 0;
    for (i = 0; i < 13; i++) iPi[i] = 0;
    for (i = 0; i < 8; i++) iRi[i] = 0;
    iRi[0] = 0xE; iRi[1] = 0x40; iRi[3] = 0x14; iRi[4] = 0x63634;
    for (i = 0; i < 7; i++) iSi[i] = 0;

    Trace("core: state cleared");
    /* header */
    u8 hdr[0x40];
    RomRead(0, hdr, 0x40);
    u32 entry = BE32(hdr + 8);
    for (i = 0; i < 20; i++)
        {
        u8 c = hdr[0x20 + i];
        iName[i] = (c >= 32 && c < 127) ? (char)c : ' ';
        }
    iName[20] = 0;
    for (int k = 19; k >= 0 && iName[k] == ' '; k--) iName[k] = 0;

    Trace("core: header read");
    /* CIC detection (best effort) from boot code CRC32 */
    {
    u32 crc = 0xFFFFFFFFu;
    u8 buf[256];
    u32 off = 0x40;
    while (off < 0x1000)
        {
        u32 n = 0x1000 - off;
        if (n > 256) n = 256;
        RomRead(off, buf, n);
        crc = Crc32Update(crc, buf, n);
        off += n;
        }
    crc = ~crc;
    iCic = 6102;
    u32 seed = 0x3F;
    if (crc == 0x6170A4A1u)      { iCic = 6101; seed = 0x3F; }
    else if (crc == 0x90BB6CB5u) { iCic = 6102; seed = 0x3F; }
    else if (crc == 0x0B050EE0u) { iCic = 6103; seed = 0x78; entry -= 0x100000u; }
    else if (crc == 0x98BC2C86u) { iCic = 6105; seed = 0x91; }
    else if (crc == 0xACC8580Au) { iCic = 6106; seed = 0x85; entry -= 0x200000u; }
    iR[22] = seed;
    }

    Trace("core: cic done");
    /* IPL3 puts the first 4 KB of the ROM into SP DMEM */
    {
    u8 buf[256];
    for (u32 off = 0; off < 0x1000; off += 256)
        {
        RomRead(off, buf, 256);
        for (u32 k = 0; k < 64; k++) iSp[(off >> 2) + k] = BE32(buf + k * 4);
        }
    }

    Trace("core: dmem loaded");
    /* IPL3 copies 1 MB from ROM+0x1000 to the entry point */
    {
    u32 ramAddr = entry & 0x1FFFFFFFu;
    u32 len = 0x100000u;
    if (iRomSize > 0x1000 && len > iRomSize - 0x1000) len = iRomSize - 0x1000;
    if (ramAddr < RDRAM_SIZE) DmaRomToRam(0x1000, ramAddr, len);
    }

    Trace("core: boot copy done");
    /* libultra boot parameters */
    iRam[0x300 >> 2] = 1;                 /* osTvType = NTSC */
    iRam[0x304 >> 2] = 0;                 /* osRomType = cart */
    iRam[0x308 >> 2] = 0xB0000000u;       /* osRomBase */
    iRam[0x30C >> 2] = 0;                 /* osResetType */
    iRam[0x318 >> 2] = RDRAM_SIZE;        /* osMemSize */

    iCp0[12] = 0x34000000u;               /* Status: CU0|CU1|FR */
    iCp0[15] = 0x00000B22u;               /* PRId */
    iCp0[16] = 0x0006E463u;               /* Config */
    iCp0[11] = 0;                         /* Compare */
    iCp0[13] = 0;

    iR[11] = 0xFFFFFFFFA4000040ULL;
    iR[20] = 1;
    iR[29] = 0xFFFFFFFFA4001FF0ULL;
    iR[31] = 0xFFFFFFFFA4001550ULL;

    iPC = entry;
    iNPC = entry + 4;
    Trace("core: reset end");
    }

/* ------------------------------------------------------------------ */
/* interrupts (MI)                                                     */
/* ------------------------------------------------------------------ */
void N64Core::UpdateMiIrq()
    {
    if (iMiIntr & iMiMask) iCp0[13] |= 0x400u;
    else                   iCp0[13] &= ~0x400u;
    }

void N64Core::RaiseMi(u32 aBit)
    {
    iMiIntr |= aBit;
    UpdateMiIrq();
    }

void N64Core::ClearMi(u32 aBit)
    {
    iMiIntr &= ~aBit;
    UpdateMiIrq();
    }

/* ------------------------------------------------------------------ */
/* memory access                                                       */
/* ------------------------------------------------------------------ */
inline bool N64Core::Tr(u32 va, bool wr, u32& p)
    {
    if ((va & 0xC0000000u) == 0x80000000u) { p = va & 0x1FFFFFFFu; return true; }   /* KSEG0/KSEG1 */
    return XlateTlb(va, wr, p);
    }

void N64Core::FlushTlbCache()
    {
    iBlkEpoch++;
    for (u32 i = 0; i < 64; i++) { iTlbTag[i] = 0; iTlbPhys[i] = 0; iTlbW[i] = 0; }
    }

bool N64Core::XlateTlb(u32 va, bool wr, u32& p)
    {
    u32 idx = (va >> 12) & 63;
    u32 tag = (va >> 12) + 1;
    if (iTlbTag[idx] == tag && (!wr || iTlbW[idx]))
        {
        p = iTlbPhys[idx] | (va & 0xFFFu);
        return true;
        }
    u32 asid = iCp0[10] & 0xFFu;
    for (u32 i = 0; i < 32; i++)
        {
        const N64TlbEntry& e = iTlb[i];
        u32 mask = e.mask | 0x1FFFu;
        if ((va ^ e.hi) & ~mask) continue;
        bool glob = (e.lo0 & e.lo1 & 1u) != 0;
        if (!glob && ((e.hi ^ asid) & 0xFFu)) continue;
        u32 odd = (mask + 1) >> 1;
        u32 lo = (va & odd) ? e.lo1 : e.lo0;
        if (!(lo & 2u)) { TlbFault(va, wr, 1); return false; }
        if (wr && !(lo & 4u)) { TlbFault(va, wr, 2); return false; }
        u32 pa = ((lo >> 6) << 12) | (va & (odd - 1));
        iTlbTag[idx] = tag;
        iTlbPhys[idx] = pa & ~0xFFFu;
        iTlbW[idx] = (lo & 4u) ? 1 : 0;
        p = pa;
        return true;
        }
    TlbFault(va, wr, 0);
    return false;
    }

/* kind: 0 = refill (no match), 1 = invalid, 2 = modified (write to clean page) */
void N64Core::TlbFault(u32 va, bool wr, u32 kind)
    {
    iFault = true;
    if (iQuiet) return;
    iStats.tlbFaults++;
    iCp0[8] = va;                                                          /* BadVAddr */
    iCp0[4] = (iCp0[4] & 0xFF800000u) | (((va >> 13) & 0x7FFFFu) << 4);    /* Context */
    iCp0[10] = (va & 0xFFFFE000u) | (iCp0[10] & 0xFFu);                    /* EntryHi */
    bool exl = (iCp0[12] & 2u) != 0;
    u32 code = (kind == 2) ? 1u : (wr ? 3u : 2u);
    u32 vec = (kind == 0 && !exl) ? 0x000u : 0x180u;
    Exception(code, iCurPc, iDelaySlot, vec);
    }

u32 N64Core::Fetch(u32 va)
    {
    u32 p;
    if (!Tr(va, false, p)) return 0;
    p &= 0x1FFFFFFCu;
    if (p < RDRAM_SIZE) return iRam[p >> 2];
    if (p >= 0x04000000u && p < 0x04002000u) return iSp[(p - 0x04000000u) >> 2];
    if (p >= 0x10000000u && p < 0x1FC00000u)
        {
        u8 b[4];
        RomRead(p - 0x10000000u, b, 4);
        return BE32(b);
        }
    return 0;
    }

u32 N64Core::Rd32(u32 va)
    {
    u32 p;
    if (!Tr(va, false, p)) return 0;
    p &= 0x1FFFFFFCu;
    if (p < RDRAM_SIZE) return iRam[p >> 2];
    return RdIo(p);
    }

u32 N64Core::Rd16(u32 va)
    {
    u32 w = Rd32(va);
    return (w >> (((va & 2) ^ 2) * 8)) & 0xFFFFu;
    }

u32 N64Core::Rd8(u32 va)
    {
    u32 w = Rd32(va);
    return (w >> ((3 - (va & 3)) * 8)) & 0xFFu;
    }

u64 N64Core::Rd64(u32 va)
    {
    u64 hi = Rd32(va);
    u64 lo = Rd32(va + 4);
    return (hi << 32) | lo;
    }

void N64Core::Wr32(u32 va, u32 v)
    {
    u32 p;
    if (!Tr(va, true, p)) return;
    p &= 0x1FFFFFFCu;
    if (p < RDRAM_SIZE) { iRam[p >> 2] = v; MarkWrite(p); return; }
    WrIo(p, v);
    }

void N64Core::Wr16(u32 va, u32 v)
    {
    u32 p;
    if (!Tr(va, true, p)) return;
    p &= 0x1FFFFFFFu;
    u32 sh = ((va & 2) ^ 2) * 8;
    if (p < RDRAM_SIZE)
        {
        u32 i = (p & ~3u) >> 2;
        iRam[i] = (iRam[i] & ~(0xFFFFu << sh)) | ((v & 0xFFFFu) << sh);
        MarkWrite(p);
        }
    else if (p >= 0x04000000u && p < 0x04002000u)
        {
        u32 i = (p - 0x04000000u) >> 2;
        iSp[i] = (iSp[i] & ~(0xFFFFu << sh)) | ((v & 0xFFFFu) << sh);
        if (i >= 1024) iRspDecDirty = true;
        }
    }

void N64Core::Wr8(u32 va, u32 v)
    {
    u32 p;
    if (!Tr(va, true, p)) return;
    p &= 0x1FFFFFFFu;
    u32 sh = (3 - (va & 3)) * 8;
    if (p < RDRAM_SIZE)
        {
        u32 i = p >> 2;
        iRam[i] = (iRam[i] & ~(0xFFu << sh)) | ((v & 0xFFu) << sh);
        MarkWrite(p);
        }
    else if (p >= 0x04000000u && p < 0x04002000u)
        {
        u32 i = (p - 0x04000000u) >> 2;
        iSp[i] = (iSp[i] & ~(0xFFu << sh)) | ((v & 0xFFu) << sh);
        if (i >= 1024) iRspDecDirty = true;
        }
    else if (p >= 0x1FC007C0u && p < 0x1FC00800u)
        {
        iPif[p - 0x1FC007C0u] = (u8)v;
        }
    }

void N64Core::Wr64(u32 va, u64 v)
    {
    Wr32(va, (u32)(v >> 32));
    Wr32(va + 4, (u32)v);
    }

/* ------------------------------------------------------------------ */
/* I/O register reads                                                  */
/* ------------------------------------------------------------------ */
u32 N64Core::RdIo(u32 p)
    {
    if (p >= 0x04000000u && p < 0x04002000u) return iSp[(p - 0x04000000u) >> 2];

    if (p >= 0x04040000u && p < 0x04040020u) return SpRegRd((p & 0x1C) >> 2);
    if (p == 0x04080000u) return iSpPc;

    if (p >= 0x04100000u && p < 0x04100020u) return DpcRd((p & 0x1F) >> 2);

    if (p >= 0x04300000u && p < 0x04300010u)
        {
        switch (p & 0xC)
            {
            case 0x0: return iMiMode;
            case 0x4: return 0x02020102u;
            case 0x8: return iMiIntr;
            case 0xC: return iMiMask;
            }
        return 0;
        }

    if (p >= 0x04400000u && p < 0x04400038u)
        {
        u32 idx = (p - 0x04400000u) >> 2;
        if (idx == 4) return iViCurrent;
        return iVi[idx];
        }

    if (p >= 0x04500000u && p < 0x04500018u)
        {
        u32 idx = (p - 0x04500000u) >> 2;
        if (idx == 1) return iAiLen;
        if (idx == 3) return 0;
        return iAi[idx];
        }

    if (p >= 0x04600000u && p < 0x04600034u)
        {
        u32 idx = (p - 0x04600000u) >> 2;
        if (idx == 4) return 0;          /* PI status: idle */
        return iPi[idx];
        }

    if (p >= 0x04700000u && p < 0x04700020u) return iRi[(p & 0x1F) >> 2];

    if (p >= 0x04800000u && p < 0x0480001Cu)
        {
        u32 idx = (p - 0x04800000u) >> 2;
        if (idx == 6) return (iMiIntr & 2) ? 0x1000u : 0u;
        return iSi[idx];
        }

    if (p >= 0x10000000u && p < 0x1FC00000u)
        {
        u8 b[4];
        RomRead(p - 0x10000000u, b, 4);
        return BE32(b);
        }

    if (p >= 0x1FC007C0u && p < 0x1FC00800u)
        {
        u32 o = p - 0x1FC007C0u;
        return BE32(iPif + o);
        }
    return 0;
    }

/* ------------------------------------------------------------------ */
/* I/O register writes                                                 */
/* ------------------------------------------------------------------ */
void N64Core::SpDma(bool aToSp, u32 v)
    {
    u32 len   = ((v & 0xFFFu) | 7u) + 1u;
    u32 count = ((v >> 12) & 0xFFu) + 1u;
    u32 skip  = (v >> 20) & 0xFFFu;
    u32 mem   = iSpMemAddr & 0x1FF8u;
    u32 ram   = iSpDramAddr & 0x7FFFF8u;

    for (u32 row = 0; row < count; row++)
        {
        for (u32 i = 0; i < len; i += 4)
            {
            u32 mi = ((mem + i) & 0x1FFFu) >> 2;
            u32 ri = ram + i;
            if (ri < RDRAM_SIZE)
                {
                if (aToSp) { iSp[mi] = iRam[ri >> 2]; if (mi >= 1024) iRspDecDirty = true; }
                else       { iRam[ri >> 2] = iSp[mi]; MarkWrite(ri); }
                }
            }
        mem = (mem + len) & 0x1FFFu;
        ram += len + skip;
        }
    iSpMemAddr = mem;
    iSpDramAddr = ram;
    }

void N64Core::SpStatusWrite(u32 v)
    {
    if (v & 0x0001u) iSpStatus &= ~1u;                  /* clear halt -> RSP runs */
    if (v & 0x0002u) iSpStatus |= 1u;                   /* set halt */
    if (v & 0x0004u) iSpStatus &= ~2u;                  /* clear broke */
    if (v & 0x0008u) ClearMi(1);                        /* clear intr */
    if (v & 0x0010u) RaiseMi(1);                        /* set intr */
    if (v & 0x0020u) iSpStatus &= ~0x20u;               /* clear sstep */
    if (v & 0x0040u) iSpStatus |= 0x20u;                /* set sstep */
    if (v & 0x0080u) iSpStatus &= ~0x40u;               /* clear intr on break */
    if (v & 0x0100u) iSpStatus |= 0x40u;                /* set intr on break */
    u32 b;
    for (b = 0; b < 8; b++)                             /* signals 0..7 */
        {
        if (v & (1u << (9 + 2 * b)))  iSpStatus &= ~(1u << (7 + b));
        if (v & (1u << (10 + 2 * b))) iSpStatus |=  (1u << (7 + b));
        }
    }

u32 N64Core::SpRegRd(u32 aIdx)
    {
    switch (aIdx)
        {
        case 0: return iSpMemAddr;
        case 1: return iSpDramAddr;
        case 2: case 3: return 0x00000FF8u;
        case 4: return iSpStatus;
        case 7: { u32 s = iSpSema; iSpSema = 1; return s; }
        }
    return 0;
    }

void N64Core::SpRegWr(u32 aIdx, u32 v)
    {
    switch (aIdx)
        {
        case 0: iSpMemAddr = v & 0x1FF8u; break;
        case 1: iSpDramAddr = v & 0xFFFFF8u; break;
        case 2: SpDma(true, v); break;
        case 3: SpDma(false, v); break;
        case 4:
            {
            bool wasHalted = (iSpStatus & 1u) != 0;
            SpStatusWrite(v);
            if (wasHalted && !(iSpStatus & 1u)) iStats.rspRuns++;
            break;
            }
        case 7: iSpSema = 0; break;
        }
    }

u32 N64Core::DpcRd(u32 aIdx)
    {
    switch (aIdx)
        {
        case 0: return iDpc[0];
        case 1: return iDpc[1];
        case 2: return iDpc[2];
        case 3: return (iDpcFlags & 7u) | 0x80u;        /* cbuf ready */
        }
    return 0;
    }

void N64Core::DpcWr(u32 aIdx, u32 v)
    {
    switch (aIdx)
        {
        case 0: iDpc[0] = v & 0xFFFFF8u; iDpc[2] = iDpc[0]; break;
        case 1:
            iDpc[1] = v & 0xFFFFF8u;
            iStats.dlistKicks++;
            iDpc[2] = RdpRun(iDpc[2], iDpc[1], (iDpcFlags & 1u) != 0);
            break;
        case 3:
            if (v & 0x01u) iDpcFlags &= ~1u;
            if (v & 0x02u) iDpcFlags |= 1u;
            if (v & 0x04u) iDpcFlags &= ~2u;
            if (v & 0x08u) iDpcFlags |= 2u;
            if (v & 0x10u) iDpcFlags &= ~4u;
            if (v & 0x20u) iDpcFlags |= 4u;
            break;
        }
    }

void N64Core::PiDma(u32 aLenReg)
    {
    u32 len = (aLenReg & 0xFFFFFFu) + 1u;
    u32 cart = iPi[1];
    u32 ram = iPi[0] & 0x7FFFFFu;
    if (len & 1) len++;
    if (cart >= 0x10000000u && cart < 0x1FC00000u)
        DmaRomToRam(cart - 0x10000000u, ram, len);
    RaiseMi(0x10);
    }

void N64Core::SiDma(bool aRead)
    {
    u32 ram = iSi[0] & 0x7FFFFFu;
    u32 i;
    if (aRead)
        {
        /* The real PIF keeps executing the last command block (controller polling) by itself;
           libultra relies on that and only re-reads the result buffer, so refresh it here. */
        if (iPifActive) ProcessPif();
        RamWriteBytes(ram, iPif, 64);
        }
    else
        {
        for (i = 0; i < 16; i++)
            {
            u32 a = ram + i * 4;
            u32 w = (a < RDRAM_SIZE) ? iRam[a >> 2] : 0;
            iPif[i*4]   = (u8)(w >> 24);
            iPif[i*4+1] = (u8)(w >> 16);
            iPif[i*4+2] = (u8)(w >> 8);
            iPif[i*4+3] = (u8)w;
            }
        ProcessPif();
        iPifActive = true;
        }
    RaiseMi(2);
    }

/* Minimal joybus: controller on port 1, nothing on ports 2-4. */
void N64Core::ProcessPif()
    {
    u8* p = iPif;
    u32 i = 0;
    u32 ch = 0;
    while (i < 63)
        {
        u32 t = p[i++];
        if (t == 0xFE) break;
        if (t == 0x00) { ch++; continue; }
        if (t == 0xFF || t == 0xFD) continue;
        if (i >= 63) break;
        u32 r = p[i++];
        u32 txl = t & 0x3F;
        u32 rxl = r & 0x3F;
        if (i + txl + rxl > 63) break;
        u32 cmd = p[i];
        if (ch == 0)
            {
            u8* rx = p + i + txl;
            if (cmd == 0x00 || cmd == 0xFF)
                {
                if (rxl >= 3) { rx[0] = 0x05; rx[1] = 0x00; rx[2] = 0x00; }   /* standard pad, no pak */
                }
            else if (cmd == 0x01)
                {
                if (rxl >= 4)
                    {
                    u16 b = (u16)(iButtons | iLatched);
                    s8 sx = iStickX, sy = iStickY;
                    if (sx == 0 && sy == 0 && iLatStick) { sx = iLatX; sy = iLatY; }
                    rx[0] = (u8)(b >> 8);
                    rx[1] = (u8)b;
                    rx[2] = (u8)sx;
                    rx[3] = (u8)sy;
                    iLatched = 0;                          /* every latched press has now been delivered once */
                    if (iStickX == 0 && iStickY == 0) iLatStick = false;
                    iStats.padPolls++;
                    }
                }
            }
        else if (ch == 4)
            {
            u8* rx = p + i + txl;
            if (cmd == 0x00 || cmd == 0xFF)
                {
                if (rxl >= 3) { rx[0] = 0x00; rx[1] = 0x80; rx[2] = 0x00; }   /* 4 Kbit EEPROM */
                }
            else if (cmd == 0x04 && txl >= 2 && rxl >= 8)
                {
                u32 blk = p[i + 1] & 0x3F;
                for (u32 k = 0; k < 8; k++) rx[k] = iEeprom[blk * 8 + k];
                }
            else if (cmd == 0x05 && txl >= 10 && rxl >= 1)
                {
                u32 blk = p[i + 1] & 0x3F;
                for (u32 k = 0; k < 8; k++) iEeprom[blk * 8 + k] = p[i + 2 + k];
                rx[0] = 0x00;
                }
            }
        else
            {
            p[i - 1] = (u8)(r | 0x80);          /* no device present */
            }
        i += txl + rxl;
        ch++;
        }
    p[63] = 0;
    }

void N64Core::WrIo(u32 p, u32 v)
    {
    if (p >= 0x04000000u && p < 0x04002000u) { u32 wi = (p - 0x04000000u) >> 2; iSp[wi] = v; if (wi >= 1024) iRspDecDirty = true; return; }

    if (p >= 0x04040000u && p < 0x04040020u) { SpRegWr((p & 0x1C) >> 2, v); return; }
    if (p == 0x04080000u) { iSpPc = v & 0xFFCu; iRspNpc = (iSpPc + 4) & 0xFFCu; return; }

    if (p >= 0x04100000u && p < 0x04100020u) { DpcWr((p & 0x1F) >> 2, v); return; }

    if (p >= 0x04300000u && p < 0x04300010u)
        {
        if ((p & 0xC) == 0x0)
            {
            iMiMode = v;
            if (v & 0x800u) ClearMi(0x20);      /* clear DP interrupt */
            }
        else if ((p & 0xC) == 0xC)
            {
            if (v & 0x001u) iMiMask &= ~0x01u;
            if (v & 0x002u) iMiMask |=  0x01u;
            if (v & 0x004u) iMiMask &= ~0x02u;
            if (v & 0x008u) iMiMask |=  0x02u;
            if (v & 0x010u) iMiMask &= ~0x04u;
            if (v & 0x020u) iMiMask |=  0x04u;
            if (v & 0x040u) iMiMask &= ~0x08u;
            if (v & 0x080u) iMiMask |=  0x08u;
            if (v & 0x100u) iMiMask &= ~0x10u;
            if (v & 0x200u) iMiMask |=  0x10u;
            if (v & 0x400u) iMiMask &= ~0x20u;
            if (v & 0x800u) iMiMask |=  0x20u;
            UpdateMiIrq();
            }
        return;
        }

    if (p >= 0x04400000u && p < 0x04400038u)
        {
        u32 idx = (p - 0x04400000u) >> 2;
        if (idx == 4) { ClearMi(8); return; }    /* write V_CURRENT clears VI int */
        iVi[idx] = v;
        iStats.viWrites++;
        return;
        }

    if (p >= 0x04500000u && p < 0x04500018u)
        {
        u32 idx = (p - 0x04500000u) >> 2;
        if (idx == 1) iAiLen = v & 0x3FFF8u;
        else if (idx == 3) ClearMi(4);           /* write status clears AI int */
        else iAi[idx] = v;
        return;
        }

    if (p >= 0x04600000u && p < 0x04600034u)
        {
        u32 idx = (p - 0x04600000u) >> 2;
        switch (idx)
            {
            case 0: iPi[0] = v & 0xFFFFFFu; break;
            case 1: iPi[1] = v; break;
            case 2: RaiseMi(0x10); break;        /* RD_LEN: ram->cart ignored */
            case 3: PiDma(v); break;             /* WR_LEN: cart->ram */
            case 4: if (v & 2u) ClearMi(0x10); break;
            default: iPi[idx] = v; break;
            }
        return;
        }

    if (p >= 0x04700000u && p < 0x04700020u) { iRi[(p & 0x1F) >> 2] = v; return; }

    if (p >= 0x04800000u && p < 0x0480001Cu)
        {
        switch (p & 0x1C)
            {
            case 0x00: iSi[0] = v; break;
            case 0x04: SiDma(true); break;
            case 0x10: SiDma(false); break;
            case 0x18: ClearMi(2); break;
            }
        return;
        }

    if (p >= 0x1FC007C0u && p < 0x1FC00800u)
        {
        u32 o = p - 0x1FC007C0u;
        iPif[o]   = (u8)(v >> 24);
        iPif[o+1] = (u8)(v >> 16);
        iPif[o+2] = (u8)(v >> 8);
        iPif[o+3] = (u8)v;
        return;
        }
    }

/* ------------------------------------------------------------------ */
/* frame loop                                                          */
/* ------------------------------------------------------------------ */
void N64Core::EndOfFrame()
    {
    iViCurrent = 0;
    RaiseMi(8);                              /* VI interrupt */
    if (iAiLen)
        {
        iAiLen = 0;                          /* pretend audio finished */
        RaiseMi(4);
        }
    iFrames++;
    }

bool N64Core::RunSlice(u32 aInstr)
    {
    if (!iSrc) return false;
    const u32 CHUNK = 256;
    u32 chunks = iInstrPerFrame / CHUNK;
    if (!chunks) chunks = 1;
    while (aInstr >= CHUNK)
        {
        if (iCpuMode) RunCached(CHUNK);
        else for (u32 i = 0; i < CHUNK; i++) Step();
        if (!(iSpStatus & 1u)) RunRsp(2048);
        aInstr -= CHUNK;
        iFrameChunk++;
        iViCurrent = (iFrameChunk * 524u) / chunks;
        if (iFrameChunk >= chunks)
            {
            iFrameChunk = 0;
            EndOfFrame();
            return true;
            }
        }
    return false;
    }

void N64Core::RunFrame()
    {
    while (!RunSlice(1u << 20)) { }
    }

/* ------------------------------------------------------------------ */
/* video out: VI framebuffer -> RGB565, nearest-neighbour scaled       */
/* ------------------------------------------------------------------ */
void N64Core::Render565(u16* aDst, s32 aW, s32 aH, s32 aPitch)
    {
    if (aW <= 0 || aH <= 0) return;
    u32 type = iVi[0] & 3;
    s32 x, y;
    if (type < 2)
        {
        for (y = 0; y < aH; y++)
            for (x = 0; x < aW; x++) aDst[y * aPitch + x] = 0;
        return;
        }

    /* picture size from H_START/V_START/X_SCALE/Y_SCALE, source stride from VI_WIDTH */
    s32 stride = (s32)(iVi[2] & 0xFFF);
    if (stride < 16 || stride > 1024) stride = 320;
    u32 hs = (iVi[9] >> 16) & 0x3FF, he = iVi[9] & 0x3FF;
    u32 vs = (iVi[10] >> 16) & 0x3FF, ve = iVi[10] & 0x3FF;
    u32 xsc = iVi[12] & 0xFFF, ysc = iVi[13] & 0xFFF;
    s32 sw = 0, sh = 0;
    if (he > hs && xsc) sw = (s32)(((he - hs) * xsc) >> 10);
    if (ve > vs && ysc) sh = (s32)((((ve - vs) >> 1) * ysc) >> 10);
    if (sw < 16 || sw > 700) sw = (stride > 400) ? 640 : stride;
    if (sh < 16 || sh > 600) sh = (sw > 400) ? 480 : 240;
    if (sw > stride) sw = stride;
    u32 bpp = (type == 3) ? 4 : 2;
    u32 base = iVi[1] & 0x00FFFFFFu;
    while (sh > 8 && base + (u32)stride * (u32)sh * bpp > RDRAM_SIZE) sh -= 8;

    u32 xstep = ((u32)sw << 16) / (u32)aW;
    u32 ystep = ((u32)sh << 16) / (u32)aH;
    u32 fy = 0;
    for (y = 0; y < aH; y++)
        {
        u32 sy = fy >> 16;
        fy += ystep;
        u32 row = base + sy * (u32)stride * bpp;
        u32 fx = 0;
        u16* d = aDst + y * aPitch;
        if (bpp == 2)
            {
            for (x = 0; x < aW; x++)
                {
                u32 a = row + (fx >> 16) * 2;
                fx += xstep;
                u32 px = 0;
                if (a < RDRAM_SIZE)
                    {
                    u32 w = iRam[a >> 2];
                    px = (a & 2) ? (w & 0xFFFFu) : (w >> 16);
                    }
                d[x] = (u16)((px & 0xFFC0u) | ((px >> 1) & 0x1Fu));
                }
            }
        else
            {
            for (x = 0; x < aW; x++)
                {
                u32 a = row + (fx >> 16) * 4;
                fx += xstep;
                u32 w = (a < RDRAM_SIZE) ? iRam[a >> 2] : 0;
                d[x] = (u16)(((w >> 16) & 0xF800u) | ((w >> 13) & 0x07E0u) | ((w >> 11) & 0x1Fu));
                }
            }
        }
    }
