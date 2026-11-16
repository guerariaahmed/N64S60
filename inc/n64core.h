/*
 * n64core.h - portable, dependency-free N64 core for N64S60.
 *
 * No libc / libm / STL / exceptions / globals needed, so it builds
 * under the S60 3rd Edition GCCE or RVCT toolchain AND on a PC (test).
 *
 * What it emulates:  VR4300 CPU (64-bit integer ISA), CP0 (Count/Compare/
 *   Status/Cause/EPC, exceptions, interrupts), CP1 FPU (S/D/W/L formats),
 *   RDRAM (4 MB), SP DMEM/IMEM + SP DMA, MI, VI, PI DMA (cart->RDRAM),
 *   SI + PIF joybus (controller 1), HLE boot (no PIF/IPL3 ROM needed).
 * What it does NOT emulate: RSP microcode, RDP, audio, TLB, Controller Pak.
 *   => only software-rendered / CPU-framebuffer ROMs (homebrew, demos,
 *      test ROMs) show a picture. Commercial games will boot but stay black.
 */
#ifndef N64CORE_H
#define N64CORE_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        s8;
typedef short              s16;
typedef int                s32;
typedef long long          s64;

#define N64_RDRAM_SIZE   0x400000u            /* 4 MB */
#define N64_RDRAM_BYTES  N64_RDRAM_SIZE

/* Controller button bits (standard N64 joybus layout) */
#define N64_BTN_A       0x8000
#define N64_BTN_B       0x4000
#define N64_BTN_Z       0x2000
#define N64_BTN_START   0x1000
#define N64_BTN_DUP     0x0800
#define N64_BTN_DDOWN   0x0400
#define N64_BTN_DLEFT   0x0200
#define N64_BTN_DRIGHT  0x0100
#define N64_BTN_L       0x0020
#define N64_BTN_R       0x0010
#define N64_BTN_CUP     0x0008
#define N64_BTN_CDOWN   0x0004
#define N64_BTN_CLEFT   0x0002
#define N64_BTN_CRIGHT  0x0001


/* one TLB entry (R4300 style: 32 entries, even/odd page pair) */
struct N64TlbEntry
    {
    u32 mask;       /* PageMask (bits 24:13) */
    u32 hi;         /* EntryHi: VPN2 | ASID */
    u32 lo0, lo1;   /* EntryLo0/1: PFN<<6 | C<<3 | D<<2 | V<<1 | G */
    };

/* pre-decoded RSP instruction (GPU cached mode) */
struct N64RspDec
    {
    u32 inst;
    u8  op, rs, rt, rd;
    };

/* cached-CPU basic block (pre-fetched instructions of one physical RAM page) */
struct N64Block
    {
    u32 pc;         /* virtual start address */
    u32 epoch;      /* TLB epoch it was built in */
    u32 gen;        /* generation of its RAM page when built */
    u16 page;       /* physical 4 KB page index */
    u16 len;        /* number of instructions (0 = not cacheable) */
    u32 inst[32];
    };

/* ---- RDP state (see n64rdp.cpp) ---- */
struct N64Tile
    {
    u8  fmt, siz, pal;
    u8  cmS, cmT;            /* bit0 mirror, bit1 clamp */
    u8  maskS, maskT, shiftS, shiftT;
    u16 line, tmem;          /* in 64-bit words */
    u16 sl, tl, sh, th;      /* u10.2 */
    };

struct N64Rdp
    {
    u8  ciFmt, ciSiz; u16 ciWidth; u32 ciAddr;
    u8  tiFmt, tiSiz; u16 tiWidth; u32 tiAddr;
    u32 ziAddr;
    u32 resQ;                /* resolution scale, 256 = 1.0 */
    u32 dbg;                 /* debug: 1 no z-test, 2 no alpha-test, 4 point sampling, 8 exact (not wide) edges */
    u32 fast;                /* GPU cached mode */
    u8  pipeDirty, usesTex, usesShade, pad0;
    u32 fillColor, fogColor, blendColor, primColor, envColor;
    u8  primLodFrac; u16 primZ, primDz;
    u32 combHi, combLo;
    u32 omHi, omLo;
    u16 scXH, scYH, scXL, scYL;
    N64Tile tiles[8];
    u8  tmem[4096];
    };

/* debug counters shown on screen */
struct N64Stats
    {
    u32 rspRuns, rspInstr, rdpCmds, rdpTris, rdpRects, unkCpu, unkRsp, unkRdp;
    u32 viWrites, dlistKicks, tlbFaults, excs, padPolls;
    };

/* Host implements this: raw (file) bytes of the ROM image, any offset/len.
   Return number of bytes actually read (<= aLen). */
class MN64RomSource
    {
public:
    virtual s32 ReadRom(u32 aOffset, u8* aDst, u32 aLen) = 0;
    virtual void Trace(const char* /*aMsg*/) {}      /* optional progress breadcrumbs */
    };

class N64Core
    {
public:
    /* aRdram: caller-owned buffer of N64_RDRAM_BYTES bytes (word aligned). */
    void Init(u32* aRdram);

    /* Detects z64/v64/n64 byte order, HLE-boots. Returns 0 on success. */
    s32  LoadRom(MN64RomSource* aSrc, u32 aRomSize);
    void Reset();                         /* re-boot currently loaded ROM */
    bool RomLoaded() const { return iSrc != 0; }

    /* Run one video frame worth of emulation (iInstrPerFrame instrs). */
    void RunFrame();                      /* run until one video frame is done */
    bool RunSlice(u32 aInstr);            /* run a few instructions; true when a frame finished */

    void SetInput(u16 aButtons, s8 aStickX, s8 aStickY);
    void SetInstrPerFrame(u32 aN);
    void SetCpuMode(u32 aMode) { iCpuMode = aMode ? 1 : 0; }   /* 0 = interpreter, 1 = cached blocks */
    u32  CpuMode() const { return iCpuMode; }
    void SetGpuMode(u32 aMode) { iGpuMode = aMode ? 1 : 0; iRdp.fast = iGpuMode; if (iGpuMode) iRspDecDirty = true; }
    u32  GpuMode() const { return iGpuMode; }
    void SetGfxDebug(u32 aMask) { iGfxDbg = aMask; iRdp.dbg = aMask; }
    u32  GfxDebug() const { return iGfxDbg; }
    void PressButton(u16 aMask) { iLatched |= aMask; }          /* a "tap" that is held until the game polls it */
    void SetResScale(u32 aQ);                                  /* 256 = 1.0, 230 = 0.9, 192 = 0.75, 128 = 0.5 */
    u32  ResScale() const { return iResQ; }
    u32  InstrPerFrame() const { return iInstrPerFrame; }

    /* Scale current VI framebuffer into an RGB565 buffer (aPitch in pixels). */
    void Render565(u16* aDst, s32 aW, s32 aH, s32 aPitch);
    bool VideoOn() const { return (iVi[0] & 3) >= 2; }

    u32  Pc() const        { return iPC; }
    u32  Frames() const    { return iFrames; }
    u32  CicType() const   { return iCic; }
    const char* RomName() const { return iName; }

    const N64Stats& Stats() const { return iStats; }
    u32  RspPc() const { return iSpPc; }
    u32  SpStatusReg() const { return iSpStatus; }

    /* test/debug helpers */
    u32  PeekRam32(u32 aPhys) const { return iRam[(aPhys & (N64_RDRAM_SIZE-1)) >> 2]; }
    u64  Gpr(u32 n) const { return iR[n & 31]; }

private:
    /* ---- CPU ---- */
    void Step();
    void ExecInst(u32 aInst, u32 aCur);
    u32  RunCached(u32 aBudget);
    u32  ExecBlock(const N64Block* aBlk);
    N64Block* FindBlock(u32 aPc);
    void MarkWrite(u32 aPhys) { u32 pg = aPhys >> 12; if (iCodePage[pg]) { iCodePage[pg] = 0; iPageGen[pg]++; } }
    void InvalRange(u32 aAddr, u32 aLen);
    void Exception(u32 aCode, u32 aEpc, bool aBd, u32 aVec = 0x180);
    inline bool Tr(u32 va, bool wr, u32& p);
    bool XlateTlb(u32 va, bool wr, u32& p);
    void TlbFault(u32 va, bool wr, u32 kind);
    void FlushTlbCache();
    void TlbWrite(u32 aIndex);
    inline void Branch(bool aTaken, u32 aTarget, bool aLikely);
    void Cop0(u32 aInst);
    void Cop1(u32 aInst, u32 aCur);
    void FpS(u32 fn, u32 ft, u32 fs, u32 fd);
    void FpD(u32 fn, u32 ft, u32 fs, u32 fd);
    void FpW(u32 fn, u32 fs, u32 fd);
    void FpL(u32 fn, u32 fs, u32 fd);
    void Special(u32 aInst, u32 aCur);
    void RegImm(u32 aInst, u32 aCur);
    void UpdateMiIrq();
    void RaiseMi(u32 aBit);
    void ClearMi(u32 aBit);

    /* ---- memory ---- */
    u32  Rd32(u32 va);
    u32  Rd16(u32 va);
    u32  Rd8(u32 va);
    u64  Rd64(u32 va);
    void Wr32(u32 va, u32 v);
    void Wr16(u32 va, u32 v);
    void Wr8(u32 va, u32 v);
    void Wr64(u32 va, u64 v);
    u32  RdIo(u32 p);
    void WrIo(u32 p, u32 v);
    u32  Fetch(u32 va);

    /* ---- devices ---- */
    s32  RomRead(u32 aOff, u8* aDst, u32 aLen);
    void RamWriteBytes(u32 aAddr, const u8* aSrc, u32 aLen);
    void DmaRomToRam(u32 aRomOff, u32 aRamAddr, u32 aLen);
    void SpDma(bool aToSp, u32 aLenReg);
    void SpStatusWrite(u32 v);
    void PiDma(u32 aLenReg);
    void SiDma(bool aRead);
    void ProcessPif();
    void EndOfFrame();

    /* ---- RSP (n64rsp.cpp) ---- */
    void RunRsp(u32 aBudget);
    void RspDecode();
    void RspVecFast(u32 aInst);
    void RspVec(u32 aInst);
    void RspLwc2(u32 aInst);
    void RspSwc2(u32 aInst);
    u32  RspRdB(u32 a) const { a &= 0xFFF; return (iSp[a >> 2] >> ((3 - (a & 3)) * 8)) & 0xFF; }
    void RspWrB(u32 a, u32 v) { a &= 0xFFF; u32 sh = (3 - (a & 3)) * 8; iSp[a >> 2] = (iSp[a >> 2] & ~(0xFFu << sh)) | ((v & 0xFFu) << sh); }
    u32  SpRegRd(u32 aIdx);
    void SpRegWr(u32 aIdx, u32 v);
    u32  DpcRd(u32 aIdx);
    void DpcWr(u32 aIdx, u32 v);

    /* ---- RDP (n64rdp.cpp) ---- */
    u32  RdpRun(u32 aStart, u32 aEnd, bool aDmem);
    void RdpReset();
    void Trace(const char* aMsg) { if (iSrc) iSrc->Trace(aMsg); }

    /* ---- FPU register access honouring Status.FR ---- */
    u64  FGet64(u32 r) const;
    void FSet64(u32 r, u64 v);

private:
    /* CPU state */
    u64  iR[32];
    u64  iHI, iLO;
    u32  iPC, iNPC;
    bool iBranchPrev;
    bool iDelaySlot;
    bool iLLbit;
    u32  iCp0[32];
    u32  iCount, iCountStep;
    u64  iF[32];
    u32  iFcr31;

    /* cached CPU mode */
    u32  iCpuMode;
    u32  iResQ;
    u32  iBlkEpoch;
    bool iQuiet;
    u32  iPageGen[1024];
    u8   iCodePage[1024];
    N64Block iBlk[1024];

    /* TLB */
    N64TlbEntry iTlb[32];
    u32  iTlbTag[64];
    u32  iTlbPhys[64];
    u8   iTlbW[64];
    u32  iCurPc;
    bool iFault;

    /* memory */
    u32* iRam;
    u32  iSp[2048];           /* DMEM (4K) + IMEM (4K) as words */
    u8   iPif[64];

    /* devices */
    u32  iSpMemAddr, iSpDramAddr, iSpStatus, iSpSema, iSpPc;
    u32  iDpc[8];
    u32  iMiMode, iMiIntr, iMiMask;
    u32  iVi[14];
    u32  iViCurrent;
    u32  iAi[6];
    u32  iAiLen;
    u32  iPi[13];
    u32  iRi[8];
    u32  iSi[7];

    /* RSP */
    u32  iGpuMode;
    u32  iGfxDbg;
    bool iRspDecDirty;
    N64RspDec iRspDec[1024];
    u32  iRspR[32];
    u32  iRspNpc;
    u16  iVr[32][8];
    s64  iAcc[8];
    u16  iVco, iVcc;
    u8   iVce;
    u16  iDivIn;
    s32  iDivOut;
    bool iDivDp;
    u32  iDpcFlags;

    /* RDP */
    N64Rdp iRdp;

    /* EEPROM (4 Kbit) behind the PIF */
    u8   iEeprom[512];

    N64Stats iStats;

    /* input */
    u16  iButtons;
    s8   iStickX, iStickY;
    bool iPifActive;
    u16  iPrevHeld, iLatched;
    s8   iLatX, iLatY;
    bool iLatStick;

    /* rom */
    MN64RomSource* iSrc;
    u32  iRomSize;
    u32  iRomFmt;             /* 0=z64 (BE), 1=v64 (16bit swapped), 2=n64 (LE) */
    u32  iCic;
    char iName[24];

    u8   iRomBuf[2048];
    u8   iDmaBuf[2048];

    /* timing */
    u32  iInstrPerFrame;
    u32  iFrames;
    u32  iFrameChunk;
    };

#endif
