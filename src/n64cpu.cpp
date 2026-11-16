/*
 * n64cpu.cpp - VR4300 interpreter (integer ISA, CP0, CP1 FPU).
 * Simplifications: no TLB, no trap/overflow exceptions, FR=1 register
 * model only, FPU exceptions never raised, delay-slot interrupts handled
 * the usual way (EPC = branch, BD set).
 */
#include "n64core.h"

enum
    {
    C0_RANDOM = 1,
    C0_COUNT = 9,
    C0_COMPARE = 11,
    C0_STATUS = 12,
    C0_CAUSE = 13,
    C0_EPC = 14,
    C0_ERROREPC = 30
    };

static inline u64 SE32(u32 v) { return (u64)(s64)(s32)v; }

/* ------------------------------------------------------------------ */
/* float / double bit access                                           */
/* ------------------------------------------------------------------ */
static inline float U2F(u32 u) { union { u32 u; float f; } x; x.u = u; return x.f; }
static inline u32   F2U(float f) { union { u32 u; float f; } x; x.f = f; return x.u; }
static inline double U2D(u64 u) { union { u64 u; double d; } x; x.u = u; return x.d; }
static inline u64    D2U(double d) { union { u64 u; double d; } x; x.d = d; return x.u; }
static inline float  U2FQuietNaN() { return U2F(0x7FC00000u); }
static inline double U2DQuietNaN() { return U2D(0x7FF8000000000000ULL); }
static inline float  U2FInf(float a, float b) { return U2F(((F2U(a) ^ F2U(b)) & 0x80000000u) | 0x7F800000u); }
static inline double U2DInf(double a, double b) { return U2D(((D2U(a) ^ D2U(b)) & 0x8000000000000000ULL) | 0x7FF0000000000000ULL); }
static inline float SafeDivS(float a, float b)
    {
    if (b == 0.0f)
        {
        if (a == 0.0f || a != a) return U2FQuietNaN();
        return U2FInf(a, b);
        }
    return a / b;
    }
static inline double SafeDivD(double a, double b)
    {
    if (b == 0.0)
        {
        if (a == 0.0 || a != a) return U2DQuietNaN();
        return U2DInf(a, b);
        }
    return a / b;
    }

/* sqrt without libm: exponent-halving guess + Newton iterations */
static double SqrtD(double x)
    {
    if (!(x > 0.0))
        {
        if (x == 0.0) return x;
        return U2D(0x7FF8000000000000ULL);     /* NaN (also for NaN input) */
        }
    double g = U2D((D2U(x) >> 1) + 0x1FF8000000000000ULL);
    for (int i = 0; i < 6; i++) g = 0.5 * (g + x / g);
    return g;
    }

/* mode: 0 nearest-even, 1 trunc, 2 ceil, 3 floor */
static s64 RoundToInt(double x, u32 mode)
    {
    if (x != x) return 0x7FFFFFFFFFFFFFFFLL;
    if (x >= 9.2233720368547758e18)  return 0x7FFFFFFFFFFFFFFFLL;
    if (x <= -9.2233720368547758e18) return (s64)0x8000000000000000ULL;
    s64 t = (s64)x;
    double d = x - (double)t;
    switch (mode & 3)
        {
        case 1: return t;
        case 2: return (d > 0.0) ? t + 1 : t;
        case 3: return (d < 0.0) ? t - 1 : t;
        default:
            {
            double ad = (d < 0.0) ? -d : d;
            if (ad > 0.5) return (d > 0.0) ? t + 1 : t - 1;
            if (ad < 0.5) return t;
            if (t & 1) return (d > 0.0) ? t + 1 : t - 1;
            return t;
            }
        }
    }

static s32 ClampW(s64 v)
    {
    if (v > 0x7FFFFFFFLL) return 0x7FFFFFFF;
    if (v < -0x80000000LL) return (s32)0x80000000u;
    return (s32)v;
    }

static void MulU64(u64 a, u64 b, u64& hi, u64& lo)
    {
    u64 a0 = a & 0xFFFFFFFFULL, a1 = a >> 32;
    u64 b0 = b & 0xFFFFFFFFULL, b1 = b >> 32;
    u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    u64 mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
    lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
    hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    }

/* ------------------------------------------------------------------ */
/* exceptions / branches                                               */
/* ------------------------------------------------------------------ */
void N64Core::Exception(u32 aCode, u32 aEpc, bool aBd, u32 aVec)
    {
    u32 st = iCp0[C0_STATUS];
    u32 ca = iCp0[C0_CAUSE];
    if (aCode) iStats.excs++;
    if (!(st & 2u))
        {
        iCp0[C0_EPC] = aBd ? aEpc - 4 : aEpc;
        ca = (ca & ~0x8000007Cu) | (aBd ? 0x80000000u : 0u) | (aCode << 2);
        }
    else
        {
        ca = (ca & ~0x7Cu) | (aCode << 2);
        }
    iCp0[C0_CAUSE] = ca;
    iCp0[C0_STATUS] = st | 2u;                       /* EXL */
    u32 base = (st & 0x00400000u) ? 0xBFC00200u : 0x80000000u;
    iPC = base + aVec;
    iNPC = iPC + 4;
    iBranchPrev = false;
    }

inline void N64Core::Branch(bool aTaken, u32 aTarget, bool aLikely)
    {
    if (aTaken)
        {
        iNPC = aTarget;
        iBranchPrev = true;
        }
    else if (aLikely)
        {
        iPC = iNPC;                  /* annul delay slot */
        iNPC = iPC + 4;
        }
    else
        {
        iBranchPrev = true;
        }
    }

/* ------------------------------------------------------------------ */
/* SPECIAL / REGIMM                                                    */
/* ------------------------------------------------------------------ */
void N64Core::Special(u32 inst, u32 cur)
    {
    u32 rs = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31, sa = (inst >> 6) & 31;
    u64 a = iR[rs], b = iR[rt];
    switch (inst & 63)
        {
        case 0x00: iR[rd] = SE32((u32)b << sa); break;                      /* SLL */
        case 0x02: iR[rd] = SE32((u32)b >> sa); break;                      /* SRL */
        case 0x03: iR[rd] = SE32((u32)((s32)b >> sa)); break;               /* SRA */
        case 0x04: iR[rd] = SE32((u32)b << (a & 31)); break;                /* SLLV */
        case 0x06: iR[rd] = SE32((u32)b >> (a & 31)); break;                /* SRLV */
        case 0x07: iR[rd] = SE32((u32)((s32)b >> (a & 31))); break;         /* SRAV */
        case 0x08: Branch(true, (u32)a, false); break;                      /* JR */
        case 0x09: iR[rd] = SE32(cur + 8); Branch(true, (u32)a, false); break; /* JALR */
        case 0x0C: Exception(8, cur, iDelaySlot); break;                    /* SYSCALL */
        case 0x0D: Exception(9, cur, iDelaySlot); break;                    /* BREAK */
        case 0x0F: break;                                                   /* SYNC */
        case 0x10: iR[rd] = iHI; break;
        case 0x11: iHI = a; break;
        case 0x12: iR[rd] = iLO; break;
        case 0x13: iLO = a; break;
        case 0x14: iR[rd] = b << (a & 63); break;                           /* DSLLV */
        case 0x16: iR[rd] = b >> (a & 63); break;                           /* DSRLV */
        case 0x17: iR[rd] = (u64)((s64)b >> (a & 63)); break;               /* DSRAV */
        case 0x18:                                                          /* MULT */
            {
            s64 p = (s64)(s32)a * (s64)(s32)b;
            iLO = SE32((u32)p);
            iHI = SE32((u32)((u64)p >> 32));
            break;
            }
        case 0x19:                                                          /* MULTU */
            {
            u64 p = (u64)(u32)a * (u64)(u32)b;
            iLO = SE32((u32)p);
            iHI = SE32((u32)(p >> 32));
            break;
            }
        case 0x1A:                                                          /* DIV */
            {
            s32 x = (s32)a, y = (s32)b;
            if (y == 0) { iLO = (x < 0) ? 1ULL : 0xFFFFFFFFFFFFFFFFULL; iHI = SE32((u32)x); }
            else if (x == (s32)0x80000000 && y == -1) { iLO = SE32(0x80000000u); iHI = 0; }
            else { iLO = SE32((u32)(x / y)); iHI = SE32((u32)(x % y)); }
            break;
            }
        case 0x1B:                                                          /* DIVU */
            {
            u32 x = (u32)a, y = (u32)b;
            if (y == 0) { iLO = 0xFFFFFFFFFFFFFFFFULL; iHI = SE32(x); }
            else { iLO = SE32(x / y); iHI = SE32(x % y); }
            break;
            }
        case 0x1C:                                                          /* DMULT */
            {
            u64 hi, lo;
            MulU64(a, b, hi, lo);
            if ((s64)a < 0) hi -= b;
            if ((s64)b < 0) hi -= a;
            iLO = lo; iHI = hi;
            break;
            }
        case 0x1D:                                                          /* DMULTU */
            {
            u64 hi, lo;
            MulU64(a, b, hi, lo);
            iLO = lo; iHI = hi;
            break;
            }
        case 0x1E:                                                          /* DDIV */
            {
            s64 x = (s64)a, y = (s64)b;
            if (y == 0) { iLO = (x < 0) ? 1ULL : 0xFFFFFFFFFFFFFFFFULL; iHI = a; }
            else if (a == 0x8000000000000000ULL && y == -1) { iLO = a; iHI = 0; }
            else { iLO = (u64)(x / y); iHI = (u64)(x % y); }
            break;
            }
        case 0x1F:                                                          /* DDIVU */
            {
            if (b == 0) { iLO = 0xFFFFFFFFFFFFFFFFULL; iHI = a; }
            else { iLO = a / b; iHI = a % b; }
            break;
            }
        case 0x20: case 0x21: iR[rd] = SE32((u32)a + (u32)b); break;        /* ADD(U) */
        case 0x22: case 0x23: iR[rd] = SE32((u32)a - (u32)b); break;        /* SUB(U) */
        case 0x24: iR[rd] = a & b; break;
        case 0x25: iR[rd] = a | b; break;
        case 0x26: iR[rd] = a ^ b; break;
        case 0x27: iR[rd] = ~(a | b); break;
        case 0x2A: iR[rd] = ((s64)a < (s64)b) ? 1 : 0; break;               /* SLT */
        case 0x2B: iR[rd] = (a < b) ? 1 : 0; break;                         /* SLTU */
        case 0x2C: case 0x2D: iR[rd] = a + b; break;                        /* DADD(U) */
        case 0x2E: case 0x2F: iR[rd] = a - b; break;                        /* DSUB(U) */
        case 0x38: iR[rd] = b << sa; break;                                 /* DSLL */
        case 0x3A: iR[rd] = b >> sa; break;                                 /* DSRL */
        case 0x3B: iR[rd] = (u64)((s64)b >> sa); break;                     /* DSRA */
        case 0x3C: iR[rd] = b << (sa + 32); break;                          /* DSLL32 */
        case 0x3E: iR[rd] = b >> (sa + 32); break;                          /* DSRL32 */
        case 0x3F: iR[rd] = (u64)((s64)b >> (sa + 32)); break;              /* DSRA32 */
        default: break;                                                     /* traps etc. (ignored) */
        }
    }

void N64Core::RegImm(u32 inst, u32 cur)
    {
    u32 rs = (inst >> 21) & 31, op = (inst >> 16) & 31;
    s32 off = (s32)(s16)inst;
    u32 target = cur + 4 + (u32)(off << 2);
    s64 v = (s64)iR[rs];
    bool taken = false, likely = false, link = false;
    switch (op)
        {
        case 0x00: taken = v < 0; break;                                    /* BLTZ */
        case 0x01: taken = v >= 0; break;                                   /* BGEZ */
        case 0x02: taken = v < 0; likely = true; break;                     /* BLTZL */
        case 0x03: taken = v >= 0; likely = true; break;                    /* BGEZL */
        case 0x10: taken = v < 0; link = true; break;                       /* BLTZAL */
        case 0x11: taken = v >= 0; link = true; break;                      /* BGEZAL */
        case 0x12: taken = v < 0; likely = true; link = true; break;
        case 0x13: taken = v >= 0; likely = true; link = true; break;
        default: return;
        }
    if (link) iR[31] = SE32(cur + 8);
    Branch(taken, target, likely);
    }

/* ------------------------------------------------------------------ */
/* COP0                                                                */
/* ------------------------------------------------------------------ */
void N64Core::TlbWrite(u32 aIndex)
    {
    N64TlbEntry& e = iTlb[aIndex & 31];
    e.mask = iCp0[5] & 0x01FFE000u;
    e.hi   = iCp0[10] & 0xFFFFE0FFu;
    e.lo0  = iCp0[2] & 0x3FFFFFFFu;
    e.lo1  = iCp0[3] & 0x3FFFFFFFu;
    FlushTlbCache();
    }

void N64Core::Cop0(u32 inst)
    {
    u32 sub = (inst >> 21) & 31, rt = (inst >> 16) & 31, rd = (inst >> 11) & 31;
    switch (sub)
        {
        case 0x00: case 0x01:                                              /* MFC0 / DMFC0 */
            {
            u32 v;
            if (rd == C0_COUNT) v = iCount;
            else if (rd == C0_RANDOM)
                {
                u32 wired = iCp0[6] & 31u;
                v = wired + ((iCount >> 1) % (32u - wired));
                }
            else v = iCp0[rd];
            iR[rt] = SE32(v);
            break;
            }
        case 0x04: case 0x05:                                              /* MTC0 / DMTC0 */
            {
            u32 v = (u32)iR[rt];
            switch (rd)
                {
                case C0_COUNT:   iCount = v; break;
                case C0_COMPARE: iCp0[C0_COMPARE] = v; iCp0[C0_CAUSE] &= ~0x8000u; break;
                case C0_CAUSE:   iCp0[C0_CAUSE] = (iCp0[C0_CAUSE] & ~0x300u) | (v & 0x300u); break;
                case 0:          iCp0[0] = v & 0x3Fu; break;                       /* Index */
                case 2: case 3:  iCp0[rd] = v & 0x3FFFFFFFu; break;                /* EntryLo0/1 */
                case 4:          iCp0[4] = (iCp0[4] & 0x007FFFFFu) | (v & 0xFF800000u); break;
                case 5:          iCp0[5] = v & 0x01FFE000u; break;                 /* PageMask */
                case 6:          iCp0[6] = v & 0x3Fu; break;                       /* Wired */
                case 10:         iCp0[10] = v & 0xFFFFE0FFu; FlushTlbCache(); break; /* EntryHi */
                case 15:         break;                                            /* PRId read-only */
                default:         iCp0[rd] = v; break;
                }
            break;
            }
        default:
            if (sub & 0x10)
                {
                switch (inst & 63)
                    {
                    case 0x01:                                              /* TLBR */
                        {
                        const N64TlbEntry& e = iTlb[iCp0[0] & 31u];
                        iCp0[5] = e.mask; iCp0[10] = e.hi; iCp0[2] = e.lo0; iCp0[3] = e.lo1;
                        FlushTlbCache();
                        break;
                        }
                    case 0x02: TlbWrite(iCp0[0] & 31u); break;              /* TLBWI */
                    case 0x06:                                              /* TLBWR */
                        {
                        u32 wired = iCp0[6] & 31u;
                        TlbWrite(wired + ((iCount >> 1) % (32u - wired)));
                        break;
                        }
                    case 0x08:                                              /* TLBP */
                        {
                        u32 found = 0x80000000u;
                        for (u32 i = 0; i < 32; i++)
                            {
                            const N64TlbEntry& e = iTlb[i];
                            u32 mask = e.mask | 0x1FFFu;
                            if ((iCp0[10] ^ e.hi) & ~mask) continue;
                            bool glob = (e.lo0 & e.lo1 & 1u) != 0;
                            if (!glob && ((e.hi ^ iCp0[10]) & 0xFFu)) continue;
                            found = i;
                            break;
                            }
                        iCp0[0] = found;
                        break;
                        }
                    case 0x18:                                              /* ERET */
                        {
                        u32 tgt;
                        if (iCp0[C0_STATUS] & 4u) { tgt = iCp0[C0_ERROREPC]; iCp0[C0_STATUS] &= ~4u; }
                        else                      { tgt = iCp0[C0_EPC];      iCp0[C0_STATUS] &= ~2u; }
                        iPC = tgt;
                        iNPC = tgt + 4;
                        iBranchPrev = false;
                        iLLbit = false;
                        break;
                        }
                    default: break;
                    }
                }
            break;
        }
    }

/* ------------------------------------------------------------------ */
/* COP1 (FPU)                                                          */
/* ------------------------------------------------------------------ */
static inline float  FS_(const u64* f, u32 r) { return U2F((u32)f[r]); }
#define FD_(f, r)  U2D(FGet64(r))

static void SetS(u64* f, u32 r, float v) { f[r] = (f[r] & 0xFFFFFFFF00000000ULL) | F2U(v); }
static void SetW(u64* f, u32 r, u32 v)   { f[r] = (f[r] & 0xFFFFFFFF00000000ULL) | v; }
#define SetD(f, r, v)  FSet64((r), D2U(v))


/* With Status.FR=0 (what libultra threads run with) a double lives in an
   even/odd register pair; with FR=1 every register is a full 64 bits. */
u64 N64Core::FGet64(u32 r) const
    {
    if (iCp0[C0_STATUS] & 0x04000000u) return iF[r];
    r &= ~1u;
    return ((u64)(u32)iF[r + 1] << 32) | (u64)(u32)iF[r];
    }

void N64Core::FSet64(u32 r, u64 v)
    {
    if (iCp0[C0_STATUS] & 0x04000000u) { iF[r] = v; return; }
    r &= ~1u;
    iF[r] = (u32)v;
    iF[r + 1] = (u32)(v >> 32);
    }

void N64Core::FpS(u32 fn, u32 ft, u32 fs, u32 fd)
    {
    float a = FS_(iF, fs), b = FS_(iF, ft);
    switch (fn)
        {
        case 0: SetS(iF, fd, a + b); return;
        case 1: SetS(iF, fd, a - b); return;
        case 2: SetS(iF, fd, a * b); return;
        case 3: SetS(iF, fd, SafeDivS(a, b)); return;
        case 4: SetS(iF, fd, (float)SqrtD((double)a)); return;
        case 5: SetS(iF, fd, (a < 0.0f) ? -a : a); return;
        case 6: SetW(iF, fd, (u32)iF[fs]); return;
        case 7: SetS(iF, fd, -a); return;
        case 8: case 9: case 10: case 11:
            FSet64(fd, (u64)RoundToInt((double)a, fn - 8)); return;
        case 12: case 13: case 14: case 15:
            SetW(iF, fd, (u32)ClampW(RoundToInt((double)a, fn - 12))); return;
        case 33: SetD(iF, fd, (double)a); return;
        case 36: SetW(iF, fd, (u32)ClampW(RoundToInt((double)a, iFcr31 & 3))); return;
        case 37: FSet64(fd, (u64)RoundToInt((double)a, iFcr31 & 3)); return;
        }
    if (fn >= 48)
        {
        bool un = (a != a) || (b != b);
        bool lt = !un && (a < b);
        bool eq = !un && (a == b);
        bool r = ((fn & 1) && un) || ((fn & 2) && eq) || ((fn & 4) && lt);
        if (r) iFcr31 |= 0x800000u; else iFcr31 &= ~0x800000u;
        }
    }

void N64Core::FpD(u32 fn, u32 ft, u32 fs, u32 fd)
    {
    double a = FD_(iF, fs), b = FD_(iF, ft);
    switch (fn)
        {
        case 0: SetD(iF, fd, a + b); return;
        case 1: SetD(iF, fd, a - b); return;
        case 2: SetD(iF, fd, a * b); return;
        case 3: SetD(iF, fd, SafeDivD(a, b)); return;
        case 4: SetD(iF, fd, SqrtD(a)); return;
        case 5: SetD(iF, fd, (a < 0.0) ? -a : a); return;
        case 6: FSet64(fd, FGet64(fs)); return;
        case 7: SetD(iF, fd, -a); return;
        case 8: case 9: case 10: case 11:
            FSet64(fd, (u64)RoundToInt(a, fn - 8)); return;
        case 12: case 13: case 14: case 15:
            SetW(iF, fd, (u32)ClampW(RoundToInt(a, fn - 12))); return;
        case 32: SetS(iF, fd, (float)a); return;
        case 36: SetW(iF, fd, (u32)ClampW(RoundToInt(a, iFcr31 & 3))); return;
        case 37: FSet64(fd, (u64)RoundToInt(a, iFcr31 & 3)); return;
        }
    if (fn >= 48)
        {
        bool un = (a != a) || (b != b);
        bool lt = !un && (a < b);
        bool eq = !un && (a == b);
        bool r = ((fn & 1) && un) || ((fn & 2) && eq) || ((fn & 4) && lt);
        if (r) iFcr31 |= 0x800000u; else iFcr31 &= ~0x800000u;
        }
    }

void N64Core::FpW(u32 fn, u32 fs, u32 fd)
    {
    s32 w = (s32)(u32)iF[fs];
    if (fn == 32) SetS(iF, fd, (float)w);
    else if (fn == 33) SetD(iF, fd, (double)w);
    }

void N64Core::FpL(u32 fn, u32 fs, u32 fd)
    {
    s64 l = (s64)FGet64(fs);
    if (fn == 32) SetS(iF, fd, (float)l);
    else if (fn == 33) SetD(iF, fd, (double)l);
    }

void N64Core::Cop1(u32 inst, u32 cur)
    {
    u32 fmt = (inst >> 21) & 31, ft = (inst >> 16) & 31, fs = (inst >> 11) & 31, fd = (inst >> 6) & 31;
    switch (fmt)
        {
        case 0: iR[ft] = SE32((u32)iF[fs]); break;                          /* MFC1 */
        case 1: iR[ft] = FGet64(fs); break;                                 /* DMFC1 */
        case 2:                                                             /* CFC1 */
            if (fs == 31) iR[ft] = SE32(iFcr31);
            else if (fs == 0) iR[ft] = 0xA00;
            else iR[ft] = 0;
            break;
        case 4: SetW(iF, fs, (u32)iR[ft]); break;                           /* MTC1 */
        case 5: FSet64(fs, iR[ft]); break;                                  /* DMTC1 */
        case 6: if (fs == 31) iFcr31 = (u32)iR[ft] & 0x0183FFFFu; break;    /* CTC1 */
        case 8:                                                             /* BC1x */
            {
            bool c = (iFcr31 & 0x800000u) != 0;
            bool want = (ft & 1) != 0;
            bool likely = (ft & 2) != 0;
            u32 target = cur + 4 + (u32)((s32)(s16)inst << 2);
            Branch(c == want, target, likely);
            break;
            }
        case 16: FpS(inst & 63, ft, fs, fd); break;
        case 17: FpD(inst & 63, ft, fs, fd); break;
        case 20: FpW(inst & 63, fs, fd); break;
        case 21: FpL(inst & 63, fs, fd); break;
        default: break;
        }
    }

/* ------------------------------------------------------------------ */
/* main interpreter step                                               */
/* ------------------------------------------------------------------ */
void N64Core::Step()
    {
    iFault = false;
    /* pending interrupt? (IE=1, EXL=0, ERL=0, IM & IP) */
    if ((iCp0[C0_CAUSE] & iCp0[C0_STATUS] & 0xFF00u) && ((iCp0[C0_STATUS] & 7u) == 1u))
        {
        Exception(0, iPC, iBranchPrev);
        return;
        }

    /* Count / Compare */
    {
    u32 old = iCount;
    iCount += iCountStep;
    if ((u32)(iCp0[C0_COMPARE] - old - 1u) < iCountStep) iCp0[C0_CAUSE] |= 0x8000u;
    }

    u32 cur = iPC;
    iCurPc = cur;
    iDelaySlot = iBranchPrev;
    u32 inst = Fetch(cur);
    if (iFault) return;                              /* TLB exception on fetch */
    iPC = iNPC;
    iNPC = iPC + 4;
    iBranchPrev = false;
    ExecInst(inst, cur);
    }

/* executes one already-fetched instruction (PC/NPC are already advanced) */
void N64Core::ExecInst(u32 inst, u32 cur)
    {
    u32 op = inst >> 26;
    u32 rs = (inst >> 21) & 31, rt = (inst >> 16) & 31;
    s32 imm = (s32)(s16)inst;
    u32 uimm = inst & 0xFFFFu;
    u64 savedRt = iR[rt];
    u64 savedF0 = 0, savedF1 = 0;
    if (op == 0x31 || op == 0x35) { savedF0 = iF[rt & ~1u]; savedF1 = iF[(rt & ~1u) + 1]; }

    switch (op)
        {
        case 0x00: Special(inst, cur); break;
        case 0x01: RegImm(inst, cur); break;
        case 0x02: Branch(true, ((cur + 4) & 0xF0000000u) | ((inst & 0x3FFFFFFu) << 2), false); break;
        case 0x03:
            iR[31] = SE32(cur + 8);
            Branch(true, ((cur + 4) & 0xF0000000u) | ((inst & 0x3FFFFFFu) << 2), false);
            break;
        case 0x04: Branch(iR[rs] == iR[rt], cur + 4 + (u32)(imm << 2), false); break;   /* BEQ */
        case 0x05: Branch(iR[rs] != iR[rt], cur + 4 + (u32)(imm << 2), false); break;   /* BNE */
        case 0x06: Branch((s64)iR[rs] <= 0, cur + 4 + (u32)(imm << 2), false); break;   /* BLEZ */
        case 0x07: Branch((s64)iR[rs] > 0,  cur + 4 + (u32)(imm << 2), false); break;   /* BGTZ */
        case 0x08: case 0x09: iR[rt] = SE32((u32)iR[rs] + (u32)imm); break;              /* ADDI(U) */
        case 0x0A: iR[rt] = ((s64)iR[rs] < (s64)imm) ? 1 : 0; break;                     /* SLTI */
        case 0x0B: iR[rt] = (iR[rs] < (u64)(s64)imm) ? 1 : 0; break;                     /* SLTIU */
        case 0x0C: iR[rt] = iR[rs] & uimm; break;
        case 0x0D: iR[rt] = iR[rs] | uimm; break;
        case 0x0E: iR[rt] = iR[rs] ^ uimm; break;
        case 0x0F: iR[rt] = SE32(uimm << 16); break;                                     /* LUI */
        case 0x10: Cop0(inst); break;
        case 0x11: Cop1(inst, cur); break;
        case 0x14: Branch(iR[rs] == iR[rt], cur + 4 + (u32)(imm << 2), true); break;     /* BEQL */
        case 0x15: Branch(iR[rs] != iR[rt], cur + 4 + (u32)(imm << 2), true); break;     /* BNEL */
        case 0x16: Branch((s64)iR[rs] <= 0, cur + 4 + (u32)(imm << 2), true); break;     /* BLEZL */
        case 0x17: Branch((s64)iR[rs] > 0,  cur + 4 + (u32)(imm << 2), true); break;     /* BGTZL */
        case 0x18: case 0x19: iR[rt] = iR[rs] + (u64)(s64)imm; break;                    /* DADDI(U) */

        case 0x1A:                                                                       /* LDL */
            {
            u32 a = (u32)iR[rs] + (u32)imm;
            u32 k = a & 7, base = a & ~7u;
            u64 v = iR[rt];
            for (u32 i = 0; i < 8 - k; i++)
                {
                u32 sh = 8 * (7 - i);
                v = (v & ~(0xFFULL << sh)) | ((u64)Rd8(base + k + i) << sh);
                }
            iR[rt] = v;
            break;
            }
        case 0x1B:                                                                       /* LDR */
            {
            u32 a = (u32)iR[rs] + (u32)imm;
            u32 k = a & 7, base = a & ~7u;
            u64 v = iR[rt];
            for (u32 i = 0; i <= k; i++)
                {
                u32 sh = 8 * (k - i);
                v = (v & ~(0xFFULL << sh)) | ((u64)Rd8(base + i) << sh);
                }
            iR[rt] = v;
            break;
            }
        case 0x20: { u32 v = Rd8((u32)iR[rs] + (u32)imm); iR[rt] = (u64)(s64)(s8)v; break; }   /* LB */
        case 0x21: { u32 v = Rd16((u32)iR[rs] + (u32)imm); iR[rt] = (u64)(s64)(s16)v; break; } /* LH */
        case 0x22:                                                                       /* LWL */
            {
            u32 a = (u32)iR[rs] + (u32)imm;
            u32 k = a & 3;
            u32 w = Rd32(a & ~3u);
            u32 keep = k ? ((1u << (8 * k)) - 1u) : 0u;
            iR[rt] = SE32((w << (8 * k)) | ((u32)iR[rt] & keep));
            break;
            }
        case 0x23: iR[rt] = SE32(Rd32((u32)iR[rs] + (u32)imm)); break;                   /* LW */
        case 0x24: iR[rt] = Rd8((u32)iR[rs] + (u32)imm); break;                          /* LBU */
        case 0x25: iR[rt] = Rd16((u32)iR[rs] + (u32)imm); break;                         /* LHU */
        case 0x26:                                                                       /* LWR */
            {
            u32 a = (u32)iR[rs] + (u32)imm;
            u32 k = a & 3;
            u32 w = Rd32(a & ~3u);
            u32 sh = 24 - 8 * k;
            u32 keepMask = (sh == 0) ? 0u : ~(0xFFFFFFFFu >> sh);
            u32 m = (w >> sh) | ((u32)iR[rt] & keepMask);
            if (k == 3) iR[rt] = SE32(m);
            else iR[rt] = (iR[rt] & 0xFFFFFFFF00000000ULL) | m;
            break;
            }
        case 0x27: iR[rt] = Rd32((u32)iR[rs] + (u32)imm); break;                         /* LWU */
        case 0x28: Wr8((u32)iR[rs] + (u32)imm, (u32)iR[rt]); break;                      /* SB */
        case 0x29: Wr16((u32)iR[rs] + (u32)imm, (u32)iR[rt]); break;                     /* SH */
        case 0x2A:                                                                       /* SWL */
            {
            u32 a = (u32)iR[rs] + (u32)imm;
            u32 k = a & 3;
            u32 w = Rd32(a & ~3u);
            u32 keep = (k == 0) ? 0u : ~(0xFFFFFFFFu >> (8 * k));
            Wr32(a & ~3u, (w & keep) | ((u32)iR[rt] >> (8 * k)));
            break;
            }
        case 0x2B: Wr32((u32)iR[rs] + (u32)imm, (u32)iR[rt]); break;                     /* SW */
        case 0x2C:                                                                       /* SDL */
            {
            u32 a = (u32)iR[rs] + (u32)imm;
            u32 k = a & 7, base = a & ~7u;
            for (u32 i = 0; i < 8 - k; i++)
                Wr8(base + k + i, (u32)(iR[rt] >> (8 * (7 - i))));
            break;
            }
        case 0x2D:                                                                       /* SDR */
            {
            u32 a = (u32)iR[rs] + (u32)imm;
            u32 k = a & 7, base = a & ~7u;
            for (u32 i = 0; i <= k; i++)
                Wr8(base + i, (u32)(iR[rt] >> (8 * (k - i))));
            break;
            }
        case 0x2E:                                                                       /* SWR */
            {
            u32 a = (u32)iR[rs] + (u32)imm;
            u32 k = a & 3;
            u32 w = Rd32(a & ~3u);
            u32 sh = 24 - 8 * k;
            u32 keep = (sh == 0) ? 0u : ((1u << sh) - 1u);
            Wr32(a & ~3u, (w & keep) | ((u32)iR[rt] << sh));
            break;
            }
        case 0x2F: break;                                                                /* CACHE */
        case 0x30: iR[rt] = SE32(Rd32((u32)iR[rs] + (u32)imm)); iLLbit = true; break;    /* LL */
        case 0x31: SetW(iF, rt, Rd32((u32)iR[rs] + (u32)imm)); break;                    /* LWC1 */
        case 0x34: iR[rt] = Rd64((u32)iR[rs] + (u32)imm); iLLbit = true; break;          /* LLD */
        case 0x35: FSet64(rt, Rd64((u32)iR[rs] + (u32)imm)); break;                         /* LDC1 */
        case 0x37: iR[rt] = Rd64((u32)iR[rs] + (u32)imm); break;                         /* LD */
        case 0x38:                                                                       /* SC */
            if (iLLbit) { Wr32((u32)iR[rs] + (u32)imm, (u32)iR[rt]); iR[rt] = 1; }
            else iR[rt] = 0;
            iLLbit = false;
            break;
        case 0x39: Wr32((u32)iR[rs] + (u32)imm, (u32)iF[rt]); break;                     /* SWC1 */
        case 0x3C:                                                                       /* SCD */
            if (iLLbit) { Wr64((u32)iR[rs] + (u32)imm, iR[rt]); iR[rt] = 1; }
            else iR[rt] = 0;
            iLLbit = false;
            break;
        case 0x3D: Wr64((u32)iR[rs] + (u32)imm, FGet64(rt)); break;                          /* SDC1 */
        case 0x3F: Wr64((u32)iR[rs] + (u32)imm, iR[rt]); break;                          /* SD */
        default: iStats.unkCpu++; break;                                                 /* unknown: nop */
        }

    if (iFault)                                       /* data TLB exception: undo partial load */
        {
        iR[rt] = savedRt;
        if (op == 0x31 || op == 0x35) { iF[rt & ~1u] = savedF0; iF[(rt & ~1u) + 1] = savedF1; }
        }
    iR[0] = 0;
    }


/* ================================================================== */
/* cached CPU mode: pre-fetched basic blocks                           */
/* ================================================================== */
static inline bool IsBranchOp(u32 inst)
    {
    u32 op = inst >> 26;
    switch (op)
        {
        case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
        case 0x14: case 0x15: case 0x16: case 0x17:
            return true;
        case 0x00: { u32 f = inst & 63; return f == 8 || f == 9; }          /* JR, JALR */
        case 0x11: return ((inst >> 21) & 31) == 8;                          /* BC1x */
        }
    return false;
    }

static inline bool IsBlockEnd(u32 inst)         /* ends a block, no delay slot */
    {
    u32 op = inst >> 26;
    if (op == 0x10) return true;                                             /* COP0 / TLB / ERET */
    if (op == 0x00) { u32 f = inst & 63; return f == 0x0C || f == 0x0D; }    /* SYSCALL, BREAK */
    return false;
    }

N64Block* N64Core::FindBlock(u32 pc)
    {
    N64Block* b = &iBlk[(pc >> 2) & 1023u];
    if (b->pc == pc && b->epoch == iBlkEpoch && b->gen == iPageGen[b->page])
        return b->len ? b : 0;

    b->pc = pc; b->epoch = iBlkEpoch; b->len = 0; b->page = 0; b->gen = iPageGen[0];

    u32 p;
    bool ok = true;
    if ((pc & 0xC0000000u) == 0x80000000u) p = pc & 0x1FFFFFFFu;
    else
        {
        iQuiet = true; iFault = false;
        ok = XlateTlb(pc, false, p);
        iQuiet = false;
        if (iFault) ok = false;
        iFault = false;
        }
    if (!ok) return 0;
    p &= 0x1FFFFFFCu;
    if (p >= N64_RDRAM_SIZE) return 0;

    u32 page = p >> 12;
    u32 idx = (p & 0xFFFu) >> 2;
    u32 maxn = 1024u - idx;
    if (maxn > 32) maxn = 32;
    const u32* src = iRam + (p >> 2);
    u32 n = 0;
    while (n < maxn)
        {
        u32 inst = src[n];
        b->inst[n++] = inst;
        if (IsBranchOp(inst))
            {
            if (n < maxn) { b->inst[n] = src[n]; n++; }
            else n--;                                  /* delay slot not available: stop before the branch */
            break;
            }
        if (IsBlockEnd(inst)) break;
        }
    if (n == 0) return 0;
    iCodePage[page] = 1;
    b->len = (u16)n;
    b->page = (u16)page;
    b->gen = iPageGen[page];
    return b;
    }

u32 N64Core::ExecBlock(const N64Block* b)
    {
    u32 n = b->len;
    u32 expect = b->pc;
    u32 k;
    for (k = 0; k < n; k++)
        {
        if (iPC != expect) break;                       /* flow left the block (exception / annulled slot) */
        u32 inst = b->inst[k];
        u32 cur = expect;
        iCurPc = cur;
        iDelaySlot = iBranchPrev;
        iPC = iNPC;
        iNPC = iPC + 4;
        iBranchPrev = false;
        iFault = false;
        expect += 4;

        u32 rs = (inst >> 21) & 31, rt = (inst >> 16) & 31;
        switch (inst >> 26)
            {
            case 0x00:
                {
                u32 f = inst & 63, rd = (inst >> 11) & 31;
                if (f == 0x21)      { iR[rd] = SE32((u32)iR[rs] + (u32)iR[rt]); iR[0] = 0; }
                else if (f == 0x00) { iR[rd] = SE32((u32)iR[rt] << ((inst >> 6) & 31)); iR[0] = 0; }
                else if (f == 0x25) { iR[rd] = iR[rs] | iR[rt]; iR[0] = 0; }
                else if (f == 0x24) { iR[rd] = iR[rs] & iR[rt]; iR[0] = 0; }
                else ExecInst(inst, cur);
                break;
                }
            case 0x09: iR[rt] = SE32((u32)iR[rs] + (u32)(s32)(s16)inst); iR[0] = 0; break;     /* ADDIU */
            case 0x0C: iR[rt] = iR[rs] & (inst & 0xFFFFu); iR[0] = 0; break;                   /* ANDI */
            case 0x0D: iR[rt] = iR[rs] | (inst & 0xFFFFu); iR[0] = 0; break;                   /* ORI */
            case 0x0F: iR[rt] = SE32((inst & 0xFFFFu) << 16); iR[0] = 0; break;                /* LUI */
            case 0x23:                                                                          /* LW */
                {
                u32 va = (u32)iR[rs] + (u32)(s32)(s16)inst;
                u32 pa = va & 0x1FFFFFFCu;
                if ((va & 0xC0000000u) == 0x80000000u && pa < N64_RDRAM_SIZE) { iR[rt] = SE32(iRam[pa >> 2]); iR[0] = 0; }
                else ExecInst(inst, cur);
                break;
                }
            case 0x2B:                                                                          /* SW */
                {
                u32 va = (u32)iR[rs] + (u32)(s32)(s16)inst;
                u32 pa = va & 0x1FFFFFFCu;
                if ((va & 0xC0000000u) == 0x80000000u && pa < N64_RDRAM_SIZE) { iRam[pa >> 2] = (u32)iR[rt]; MarkWrite(pa); }
                else ExecInst(inst, cur);
                break;
                }
            default: ExecInst(inst, cur); break;
            }
        if (iFault) { k++; break; }
        }
    return k;
    }

u32 N64Core::RunCached(u32 aBudget)
    {
    u32 done = 0;
    while (done < aBudget)
        {
        if (iBranchPrev || ((iCp0[C0_CAUSE] & iCp0[C0_STATUS] & 0xFF00u) && ((iCp0[C0_STATUS] & 7u) == 1u)))
            { Step(); done++; continue; }

        N64Block* b = FindBlock(iPC);
        if (!b) { Step(); done++; continue; }

        u32 span = (u32)b->len * iCountStep;
        if ((u32)(iCp0[C0_COMPARE] - iCount - 1u) < span)      /* timer would fire inside: go slow */
            { Step(); done++; continue; }

        u32 ran = ExecBlock(b);
        iCount += ran * iCountStep;
        done += ran;
        }
    return done;
    }
