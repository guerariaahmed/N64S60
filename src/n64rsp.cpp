/*
 * n64rsp.cpp - Reality Signal Processor: scalar MIPS subset + vector unit.
 * Executes the game's microcode (graphics/audio) straight from IMEM,
 * so any ucode (Fast3D, F3DEX, ...) works without per-game HLE.
 *
 * Known approximations: VMULQ/VMACQ/VRNDP/VRNDN are no-ops, LFV/SFV are
 * no-ops, VCL/VCH/VCR are written from memory of the hardware behaviour.
 */
#include "n64core.h"

/* ------------------------------------------------------------------ */
/* reciprocal / inverse sqrt ROMs, generated once                       */
/* ------------------------------------------------------------------ */
static u16  gRcp[512];
static u16  gRsq[512];
static bool gTablesReady = false;

static u64 ISqrt64(u64 v)
    {
    u64 lo = 0, hi = (u64)1 << 32;
    while (lo < hi)
        {
        u64 mid = (lo + hi + 1) >> 1;
        if (mid * mid <= v) lo = mid; else hi = mid - 1;
        }
    return lo;
    }

/* Reciprocal ROM: (0x10000|gRcp[i]) = 2^17 / y,  y = (512+i)/512 in [1,2).
   Inverse-sqrt ROM, idx = (m8<<1)|p : (0x10000|gRsq[idx]) = 2^17/sqrt(y)  when p=1 (even exponent)
   or 2^17/sqrt(2y) when p=0 (odd exponent).  Result = ((0x10000|rom)<<14) >> shift. */
static void BuildTables()
    {
    if (gTablesReady) return;
    for (u32 i = 0; i < 512; i++)
        {
        u64 v = ((u64)1 << 27) / (2 * (512 + (u64)i) + 1);          /* mid-point of the mantissa bucket */
        u64 w = (v > 0x10000) ? v - 0x10000 : 0;
        gRcp[i] = (u16)(w > 0xFFFF ? 0xFFFF : w);
        }
    gRcp[0] = 0xFFFF;                                                /* exact powers of two */
    for (u32 idx = 0; idx < 512; idx++)
        {
        u32 p = idx & 1, m8 = idx >> 1;
        u64 a = 513 + 2 * (u64)m8;
        u64 num = p ? ((u64)1 << 43) : ((u64)1 << 42);
        u64 v = ISqrt64(num / a);
        u64 w = (v > 0x10000) ? v - 0x10000 : 0;
        gRsq[idx] = (u16)(w > 0xFFFF ? 0xFFFF : w);
        }
    gTablesReady = true;
    }

static u32 Clz32(u32 v)
    {
    u32 n = 0;
    if (v == 0) return 32;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
    }

static s32 RcpCalc(s32 input, bool rsq)
    {
    s32 mask = input >> 31;
    s32 data = input ^ mask;
    if (input > -32768) data -= mask;
    if (data == 0) return 0x7FFFFFFF;
    if (input == -32768) return (s32)0xFFFF0000u;
    u32 shift = Clz32((u32)data);
    u32 index = (u32)((((u64)(u32)data << shift) & 0x7FC00000ULL) >> 22);
    s32 result;
    if (!rsq)
        {
        result = gRcp[index];
        result = (s32)((0x10000u | (u32)result) << 14);
        result = result >> (31 - shift);
        }
    else
        {
        result = gRsq[(index & 0x1FE) | (shift & 1)];
        result = (s32)((0x10000u | (u32)result) << 14);
        result = result >> ((31 - shift) >> 1);
        }
    return result ^ mask;
    }

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static inline s64 Sext48(s64 v) { return (s64)((u64)v << 16) >> 16; }

static inline u16 SatS(s64 acc)
    {
    s64 v = acc >> 16;
    if (v > 32767) return 0x7FFF;
    if (v < -32768) return 0x8000;
    return (u16)v;
    }
static inline u16 SatU(s64 acc)
    {
    s64 v = acc >> 16;
    if (v < 0) return 0;
    if (v > 0x7FFF) return 0xFFFF;
    return (u16)v;
    }
static inline u16 SatL(s64 acc)
    {
    s64 v = acc >> 16;
    if (v < -32768) return 0;
    if (v > 32767) return 0xFFFF;
    return (u16)acc;
    }
static inline u16 Clamp16(s32 v)
    {
    if (v > 32767) return 0x7FFF;
    if (v < -32768) return 0x8000;
    return (u16)v;
    }

/* broadcast lane index for element specifier e */
static inline u32 Bcast(u32 e, u32 i)
    {
    if (e < 2) return i;
    if (e < 4) return (e == 2) ? (i & ~1u) : (i | 1u);
    if (e < 8) return (i & ~3u) + (e - 4);
    return e & 7;
    }

#define VB(r, b)       ((u32)((iVr[(r)][(b) >> 1] >> (((b) & 1) ? 0 : 8)) & 0xFF))
static inline void VSetB(u16 (*vr)[8], u32 r, u32 b, u32 v)
    {
    u16 l = vr[r][b >> 1];
    if (b & 1) l = (u16)((l & 0xFF00) | (v & 0xFF));
    else       l = (u16)((l & 0x00FF) | ((v & 0xFF) << 8));
    vr[r][b >> 1] = l;
    }

/* ------------------------------------------------------------------ */
/* vector load / store                                                 */
/* ------------------------------------------------------------------ */
void N64Core::RspLwc2(u32 inst)
    {
    u32 op = (inst >> 11) & 31, vt = (inst >> 16) & 31, base = (inst >> 21) & 31, e = (inst >> 7) & 15;
    s32 off = (s32)(inst & 0x7F);
    if (off & 0x40) off -= 128;
    u32 k;
    static const u8 scale[16] = { 0, 1, 2, 3, 4, 4, 3, 3, 4, 4, 4, 4, 0, 0, 0, 0 };
    /* scale = log2(bytes) : 1,2,4,8,16,16,8,8,16,16,16,16 */
    u32 addr = (iRspR[base] + (u32)(off * (s32)(1 << scale[op & 15]))) & 0xFFF;

    switch (op)
        {
        case 0:  VSetB(iVr, vt, e, RspRdB(addr)); break;                                /* LBV */
        case 1:  for (k = 0; k < 2 && e + k < 16; k++) VSetB(iVr, vt, e + k, RspRdB(addr + k)); break; /* LSV */
        case 2:  for (k = 0; k < 4 && e + k < 16; k++) VSetB(iVr, vt, e + k, RspRdB(addr + k)); break; /* LLV */
        case 3:  for (k = 0; k < 8 && e + k < 16; k++) VSetB(iVr, vt, e + k, RspRdB(addr + k)); break; /* LDV */
        case 4:                                                                          /* LQV */
            {
            u32 n = 16 - (addr & 15);
            for (k = 0; k < n && e + k < 16; k++) VSetB(iVr, vt, e + k, RspRdB(addr + k));
            break;
            }
        case 5:                                                                          /* LRV */
            {
            u32 idx = addr & 15, b = addr & ~15u;
            for (k = 0; k < idx; k++) { u32 d = 16 - idx + k; if (d + e < 16) VSetB(iVr, vt, d + e, RspRdB(b + k)); }
            break;
            }
        case 6:                                                                          /* LPV */
            {
            s32 index = (s32)(addr & 7) - (s32)e;
            u32 b = addr & ~7u;
            for (k = 0; k < 8; k++) iVr[vt][k] = (u16)(RspRdB(b + (((u32)index + k) & 15)) << 8);
            break;
            }
        case 7:                                                                          /* LUV */
            {
            s32 index = (s32)(addr & 7) - (s32)e;
            u32 b = addr & ~7u;
            for (k = 0; k < 8; k++) iVr[vt][k] = (u16)(RspRdB(b + (((u32)index + k) & 15)) << 7);
            break;
            }
        case 8:                                                                          /* LHV */
            {
            s32 index = (s32)(addr & 7) - (s32)e;
            u32 b = addr & ~7u;
            for (k = 0; k < 8; k++) iVr[vt][k] = (u16)(RspRdB(b + (((u32)index + k * 2) & 15)) << 7);
            break;
            }
        case 11:                                                                         /* LTV */
            {
            u32 begin = addr & ~7u, end = begin + 16;
            u32 a = begin + ((e + (addr & 8)) & 15);
            u32 vbase = vt & ~7u, voff = e >> 1;
            for (k = 0; k < 8; k++)
                {
                VSetB(iVr, vbase + voff, k * 2, RspRdB(a)); a++; if (a == end) a = begin;
                VSetB(iVr, vbase + voff, k * 2 + 1, RspRdB(a)); a++; if (a == end) a = begin;
                voff = (voff + 1) & 7;
                }
            break;
            }
        case 9:                                                                          /* LFV */
            {
            u32 idx = addr & 7, b0 = addr & ~7u;
            u16 tmp[8];
            for (k = 0; k < 4; k++)
                {
                tmp[k]     = (u16)(RspRdB(b0 + ((idx + 16 - e + k * 4) & 15)) << 7);
                tmp[k + 4] = (u16)(RspRdB(b0 + ((idx + 16 - e + k * 4 + 8) & 15)) << 7);
                }
            u32 end = e + 8;
            if (end > 16) end = 16;
            for (k = e; k < end; k++)
                VSetB(iVr, vt, k, (k & 1) ? (tmp[k >> 1] & 0xFF) : (tmp[k >> 1] >> 8));
            break;
            }
        default: iStats.unkRsp++; break;                                                 /* LWV, others */
        }
    }

void N64Core::RspSwc2(u32 inst)
    {
    u32 op = (inst >> 11) & 31, vt = (inst >> 16) & 31, base = (inst >> 21) & 31, e = (inst >> 7) & 15;
    s32 off = (s32)(inst & 0x7F);
    if (off & 0x40) off -= 128;
    static const u8 scale[16] = { 0, 1, 2, 3, 4, 4, 3, 3, 4, 4, 4, 4, 0, 0, 0, 0 };
    u32 addr = (iRspR[base] + (u32)(off * (s32)(1 << scale[op & 15]))) & 0xFFF;
    u32 k;

    switch (op)
        {
        case 0: RspWrB(addr, VB(vt, e & 15)); break;                                    /* SBV */
        case 1: for (k = 0; k < 2; k++) RspWrB(addr + k, VB(vt, (e + k) & 15)); break;  /* SSV */
        case 2: for (k = 0; k < 4; k++) RspWrB(addr + k, VB(vt, (e + k) & 15)); break;  /* SLV */
        case 3: for (k = 0; k < 8; k++) RspWrB(addr + k, VB(vt, (e + k) & 15)); break;  /* SDV */
        case 4:                                                                          /* SQV */
            {
            u32 n = 16 - (addr & 15);
            for (k = 0; k < n; k++) RspWrB(addr + k, VB(vt, (e + k) & 15));
            break;
            }
        case 5:                                                                          /* SRV */
            {
            u32 cnt = addr & 15, b = addr & ~15u, basei = 16 - cnt;
            for (k = 0; k < cnt; k++) RspWrB(b + k, VB(vt, (e + k + basei) & 15));
            break;
            }
        case 6:                                                                          /* SPV */
            for (k = 0; k < 8; k++)
                {
                u32 o = e + k;
                if ((o & 15) < 8) RspWrB(addr + k, VB(vt, (o & 7) << 1));
                else              RspWrB(addr + k, iVr[vt][o & 7] >> 7);
                }
            break;
        case 7:                                                                          /* SUV */
            for (k = 0; k < 8; k++)
                {
                u32 o = e + k;
                if ((o & 15) < 8) RspWrB(addr + k, iVr[vt][o & 7] >> 7);
                else              RspWrB(addr + k, VB(vt, (o & 7) << 1));
                }
            break;
        case 8:                                                                          /* SHV */
            {
            u32 idx = addr & 7, b = addr & ~7u;
            for (k = 0; k < 8; k++)
                {
                u32 byte = k << 1;
                u32 v = (VB(vt, (byte + e) & 15) << 1) | (VB(vt, (byte + e + 1) & 15) >> 7);
                RspWrB(b + ((idx + byte) & 15), v);
                }
            break;
            }
        case 10:                                                                         /* SWV */
            {
            u32 idx = addr & 7, b = addr & ~7u;
            for (k = 0; k < 16; k++) RspWrB(b + ((idx + k) & 15), VB(vt, (e + k) & 15));
            break;
            }
        case 11:                                                                         /* STV */
            {
            u32 start = vt & ~7u;
            u32 element = 16 - (e & ~1u);
            u32 b = addr & ~7u;
            s32 idx = (s32)(addr & 7) - (s32)(e & ~1u);
            for (k = 0; k < 8; k++)
                {
                u32 lane = ((element >> 1) & 7);
                u16 w = iVr[start + k][lane];
                RspWrB(b + ((u32)idx & 15), w >> 8); idx++;
                RspWrB(b + ((u32)idx & 15), w & 0xFF); idx++;
                element += 2;
                }
            break;
            }
        default: iStats.unkRsp++; break;                                                 /* SFV */
        }
    }

/* ------------------------------------------------------------------ */
/* vector unit (COP2 CO operations)                                    */
/* ------------------------------------------------------------------ */
void N64Core::RspVec(u32 inst)
    {
    u32 e = (inst >> 21) & 15, vt = (inst >> 16) & 31, vs = (inst >> 11) & 31, vd = (inst >> 6) & 31;
    u32 fn = inst & 63;
    u16 a[8], b[8], res[8];
    u32 i;

    for (i = 0; i < 8; i++) { a[i] = iVr[vs][i]; b[i] = iVr[vt][Bcast(e, i)]; }

    switch (fn)
        {
        case 0x00: case 0x01:                                         /* VMULF, VMULU */
            for (i = 0; i < 8; i++)
                {
                s64 p = (s64)(s16)a[i] * (s64)(s16)b[i] * 2 + 0x8000;
                iAcc[i] = Sext48(p);
                res[i] = (fn == 0) ? SatS(iAcc[i]) : SatU(iAcc[i]);
                }
            break;
        case 0x02: case 0x0A:                                         /* VRNDP, VRNDN */
            for (i = 0; i < 8; i++)
                {
                s64 prod = (s64)(s16)b[i];
                if (vs & 1) prod <<= 16;
                s64 acc = iAcc[i];
                if (fn == 0x02) { if (acc >= 0) acc += prod; }
                else            { if (acc < 0)  acc += prod; }
                iAcc[i] = Sext48(acc);
                res[i] = SatS(iAcc[i]);
                }
            break;
        case 0x03:                                                    /* VMULQ */
            for (i = 0; i < 8; i++)
                {
                s32 p = (s32)(s16)a[i] * (s32)(s16)b[i];
                if (p < 0) p += 31;
                iAcc[i] = Sext48(((s64)p) << 16);
                res[i] = (u16)((u32)(u16)Clamp16(p >> 1) & ~15u);
                }
            break;
        case 0x0B:                                                    /* VMACQ */
            for (i = 0; i < 8; i++)
                {
                s32 p = (s32)(iAcc[i] >> 16);
                if (p < 0 && !(p & 0x20)) p += 32;
                else if (p >= 0 && !(p & 0x20)) p -= 32;
                iAcc[i] = Sext48((iAcc[i] & 0xFFFF) | (((s64)p) << 16));
                res[i] = (u16)((u32)(u16)Clamp16(p >> 1) & ~15u);
                }
            break;
        case 0x04:                                                    /* VMUDL */
            for (i = 0; i < 8; i++)
                {
                iAcc[i] = (s64)(((u32)a[i] * (u32)b[i]) >> 16);
                res[i] = SatL(iAcc[i]);
                }
            break;
        case 0x05:                                                    /* VMUDM */
            for (i = 0; i < 8; i++)
                {
                iAcc[i] = Sext48((s64)(s16)a[i] * (s64)(u16)b[i]);
                res[i] = SatS(iAcc[i]);
                }
            break;
        case 0x06:                                                    /* VMUDN */
            for (i = 0; i < 8; i++)
                {
                iAcc[i] = Sext48((s64)(u16)a[i] * (s64)(s16)b[i]);
                res[i] = SatL(iAcc[i]);
                }
            break;
        case 0x07:                                                    /* VMUDH */
            for (i = 0; i < 8; i++)
                {
                iAcc[i] = Sext48(((s64)(s16)a[i] * (s64)(s16)b[i]) << 16);
                res[i] = SatS(iAcc[i]);
                }
            break;
        case 0x08: case 0x09:                                         /* VMACF, VMACU */
            for (i = 0; i < 8; i++)
                {
                iAcc[i] = Sext48(iAcc[i] + (s64)(s16)a[i] * (s64)(s16)b[i] * 2);
                res[i] = (fn == 8) ? SatS(iAcc[i]) : SatU(iAcc[i]);
                }
            break;
        case 0x0C:                                                    /* VMADL */
            for (i = 0; i < 8; i++)
                {
                iAcc[i] = Sext48(iAcc[i] + (s64)(((u32)a[i] * (u32)b[i]) >> 16));
                res[i] = SatL(iAcc[i]);
                }
            break;
        case 0x0D:                                                    /* VMADM */
            for (i = 0; i < 8; i++)
                {
                iAcc[i] = Sext48(iAcc[i] + (s64)(s16)a[i] * (s64)(u16)b[i]);
                res[i] = SatS(iAcc[i]);
                }
            break;
        case 0x0E:                                                    /* VMADN */
            for (i = 0; i < 8; i++)
                {
                iAcc[i] = Sext48(iAcc[i] + (s64)(u16)a[i] * (s64)(s16)b[i]);
                res[i] = SatL(iAcc[i]);
                }
            break;
        case 0x0F:                                                    /* VMADH */
            for (i = 0; i < 8; i++)
                {
                iAcc[i] = Sext48(iAcc[i] + (((s64)(s16)a[i] * (s64)(s16)b[i]) << 16));
                res[i] = SatS(iAcc[i]);
                }
            break;

        case 0x10:                                                    /* VADD */
            for (i = 0; i < 8; i++)
                {
                s32 sum = (s32)(s16)a[i] + (s32)(s16)b[i] + ((iVco >> i) & 1);
                iAcc[i] = (iAcc[i] & ~(s64)0xFFFF) | (u16)sum;
                res[i] = Clamp16(sum);
                }
            iVco = 0;
            break;
        case 0x11:                                                    /* VSUB */
            for (i = 0; i < 8; i++)
                {
                s32 d = (s32)(s16)a[i] - (s32)(s16)b[i] - ((iVco >> i) & 1);
                iAcc[i] = (iAcc[i] & ~(s64)0xFFFF) | (u16)d;
                res[i] = Clamp16(d);
                }
            iVco = 0;
            break;
        case 0x13:                                                    /* VABS */
            for (i = 0; i < 8; i++)
                {
                s32 s = (s16)a[i];
                s32 t = (s16)b[i];
                s32 r = (s < 0) ? -t : (s == 0 ? 0 : t);
                iAcc[i] = (iAcc[i] & ~(s64)0xFFFF) | (u16)r;
                res[i] = Clamp16(r);
                }
            break;
        case 0x14:                                                    /* VADDC */
            {
            u16 co = 0;
            for (i = 0; i < 8; i++)
                {
                u32 sum = (u32)a[i] + (u32)b[i];
                iAcc[i] = (iAcc[i] & ~(s64)0xFFFF) | (u16)sum;
                res[i] = (u16)sum;
                if (sum >> 16) co |= (u16)(1u << i);
                }
            iVco = co;
            break;
            }
        case 0x15:                                                    /* VSUBC */
            {
            u16 co = 0;
            for (i = 0; i < 8; i++)
                {
                s32 d = (s32)(u32)a[i] - (s32)(u32)b[i];
                iAcc[i] = (iAcc[i] & ~(s64)0xFFFF) | (u16)d;
                res[i] = (u16)d;
                if (d < 0) co |= (u16)(1u << i);
                if (d != 0) co |= (u16)(1u << (i + 8));
                }
            iVco = co;
            break;
            }
        case 0x1D:                                                    /* VSAR */
            for (i = 0; i < 8; i++)
                {
                if (e == 8)       res[i] = (u16)(iAcc[i] >> 32);
                else if (e == 9)  res[i] = (u16)(iAcc[i] >> 16);
                else if (e == 10) res[i] = (u16)iAcc[i];
                else              res[i] = 0;
                }
            break;

        case 0x20: case 0x21: case 0x22: case 0x23:                   /* VLT VEQ VNE VGE */
            {
            u16 cc = 0;
            for (i = 0; i < 8; i++)
                {
                s32 s = (s16)a[i], t = (s16)b[i];
                bool carry = ((iVco >> i) & 1) != 0;
                bool ne = ((iVco >> (i + 8)) & 1) != 0;
                bool f;
                if (fn == 0x20)      f = (s < t) || (s == t && carry && !ne);
                else if (fn == 0x21) f = (s == t) && !ne;
                else if (fn == 0x22) f = (s != t) || ne;
                else                 f = (s > t) || (s == t && !(carry && !ne));
                res[i] = f ? a[i] : b[i];
                iAcc[i] = (iAcc[i] & ~(s64)0xFFFF) | res[i];
                if (f) cc |= (u16)(1u << i);
                }
            iVcc = cc;
            iVco = 0;
            break;
            }
        case 0x24:                                                    /* VCL */
            {
            u16 cc = iVcc;
            for (i = 0; i < 8; i++)
                {
                u32 s = a[i], t = b[i];
                bool carry = ((iVco >> i) & 1) != 0;
                bool ne = ((iVco >> (i + 8)) & 1) != 0;
                bool le = ((iVcc >> i) & 1) != 0;
                bool ge = ((iVcc >> (i + 8)) & 1) != 0;
                bool ce = ((iVce >> i) & 1) != 0;
                u16 r;
                if (carry)
                    {
                    if (!ne)
                        {
                        u32 sum = s + t;
                        le = ce ? (sum <= 0x10000u) : (sum == 0);
                        }
                    r = le ? (u16)(0u - t) : (u16)s;
                    }
                else
                    {
                    if (!ne) ge = (s >= t);
                    r = ge ? (u16)t : (u16)s;
                    }
                res[i] = r;
                iAcc[i] = (iAcc[i] & ~(s64)0xFFFF) | r;
                cc = (u16)(cc & ~((1u << i) | (1u << (i + 8))));
                if (le) cc |= (u16)(1u << i);
                if (ge) cc |= (u16)(1u << (i + 8));
                }
            iVcc = cc; iVco = 0; iVce = 0;
            break;
            }
        case 0x25:                                                    /* VCH */
            {
            u16 cc = 0, co = 0; u8 ce = 0;
            for (i = 0; i < 8; i++)
                {
                s32 s = (s16)a[i], t = (s16)b[i];
                bool sign = ((s ^ t) < 0);
                bool le, ge, nev, cev = false;
                u16 r;
                if (sign)
                    {
                    s32 sum = s + t;
                    le = sum <= 0;
                    ge = t < 0;
                    cev = (sum == -1);
                    nev = (sum != 0) && !cev;
                    r = le ? (u16)(-t) : (u16)s;
                    co |= (u16)(1u << i);
                    }
                else
                    {
                    s32 d = s - t;
                    le = t < 0;
                    ge = d >= 0;
                    nev = d != 0;
                    r = ge ? (u16)t : (u16)s;
                    }
                if (nev) co |= (u16)(1u << (i + 8));
                if (le) cc |= (u16)(1u << i);
                if (ge) cc |= (u16)(1u << (i + 8));
                if (cev) ce |= (u8)(1u << i);
                res[i] = r;
                iAcc[i] = (iAcc[i] & ~(s64)0xFFFF) | r;
                }
            iVcc = cc; iVco = co; iVce = ce;
            break;
            }
        case 0x26:                                                    /* VCR */
            {
            u16 cc = 0;
            for (i = 0; i < 8; i++)
                {
                s32 s = (s16)a[i], t = (s16)b[i];
                bool sign = ((s ^ t) < 0);
                bool le, ge;
                u16 r;
                if (sign)
                    {
                    le = (s + t + 1) <= 0;
                    ge = t < 0;
                    r = le ? (u16)(~t) : (u16)s;
                    }
                else
                    {
                    le = t < 0;
                    ge = (s - t) >= 0;
                    r = ge ? (u16)t : (u16)s;
                    }
                if (le) cc |= (u16)(1u << i);
                if (ge) cc |= (u16)(1u << (i + 8));
                res[i] = r;
                iAcc[i] = (iAcc[i] & ~(s64)0xFFFF) | r;
                }
            iVcc = cc; iVco = 0; iVce = 0;
            break;
            }
        case 0x27:                                                    /* VMRG */
            for (i = 0; i < 8; i++)
                {
                res[i] = ((iVcc >> i) & 1) ? a[i] : b[i];
                iAcc[i] = (iAcc[i] & ~(s64)0xFFFF) | res[i];
                }
            iVco = 0;
            break;
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D:   /* logic */
            for (i = 0; i < 8; i++)
                {
                u16 r;
                switch (fn)
                    {
                    case 0x28: r = a[i] & b[i]; break;
                    case 0x29: r = (u16)~(a[i] & b[i]); break;
                    case 0x2A: r = a[i] | b[i]; break;
                    case 0x2B: r = (u16)~(a[i] | b[i]); break;
                    case 0x2C: r = a[i] ^ b[i]; break;
                    default:   r = (u16)~(a[i] ^ b[i]); break;
                    }
                res[i] = r;
                iAcc[i] = (iAcc[i] & ~(s64)0xFFFF) | r;
                }
            break;

        case 0x30: case 0x31: case 0x32: case 0x34: case 0x35: case 0x36: case 0x33:
            {
            u32 de = vs & 7;                      /* destination lane = vs field */
            u32 src = iVr[vt][e & 7];
            for (i = 0; i < 8; i++)
                {
                iAcc[i] = (iAcc[i] & ~(s64)0xFFFF) | b[i];     /* ACCL = vt (broadcast) */
                res[i] = iVr[vd][i];
                }
            if (fn == 0x33)                                    /* VMOV */
                {
                res[de] = (u16)src;
                }
            else if (fn == 0x32 || fn == 0x36)                 /* VRCPH / VRSQH */
                {
                res[de] = (u16)((u32)iDivOut >> 16);
                iDivIn = (u16)src;
                iDivDp = true;
                }
            else                                               /* VRCP / VRCPL / VRSQ / VRSQL */
                {
                bool rsq = (fn >= 0x34);
                bool lowForm = (fn == 0x31 || fn == 0x35);
                s32 input;
                if (lowForm && iDivDp) input = (s32)(((u32)iDivIn << 16) | (u32)(u16)src);
                else                   input = (s32)(s16)src;
                s32 r = RcpCalc(input, rsq);
                iDivOut = r;
                iDivDp = false;
                res[de] = (u16)r;
                iDivOut = (s32)((u32)r);
                }
            }
            break;
        case 0x37:                                                    /* VNOP */
            return;
        default:
            iStats.unkRsp++;
            return;                                                   /* VMULQ, VRND*, VMACQ ... */
        }

    for (i = 0; i < 8; i++) iVr[vd][i] = res[i];
    }

/* ------------------------------------------------------------------ */
/* GPU cached mode: pre-decoded IMEM + lean in-place vector ops         */
/* (bit-identical to RspVec; verified by tools/fuzz_gpu.cpp)            */
/* ------------------------------------------------------------------ */
void N64Core::RspDecode()
    {
    for (u32 i = 0; i < 1024; i++)
        {
        u32 inst = iSp[1024 + i];
        N64RspDec& d = iRspDec[i];
        d.inst = inst;
        d.op = (u8)(inst >> 26);
        d.rs = (u8)((inst >> 21) & 31);
        d.rt = (u8)((inst >> 16) & 31);
        d.rd = (u8)((inst >> 11) & 31);
        }
    }

void N64Core::RspVecFast(u32 inst)
    {
    u32 fn = inst & 63;
    switch (fn)
        {
        case 0x00: case 0x01: case 0x04: case 0x05: case 0x06: case 0x07:
        case 0x08: case 0x09: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        case 0x10: case 0x11: case 0x27:
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D:
            break;
        default:
            RspVec(inst);
            return;
        }
    u32 e = (inst >> 21) & 15, vt = (inst >> 16) & 31, vs = (inst >> 11) & 31, vd = (inst >> 6) & 31;
    const u16* A = iVr[vs];
    u16 bc[8];
    const u16* B;
    u32 i;
    if (e < 2) B = iVr[vt];
    else { for (i = 0; i < 8; i++) bc[i] = iVr[vt][Bcast(e, i)]; B = bc; }
    u16* D = iVr[vd];
    s64* acc = iAcc;

    switch (fn)
        {
        case 0x00:                                                    /* VMULF */
            for (i = 0; i < 8; i++)
                {
                s64 p = (s64)(s16)A[i] * (s64)(s16)B[i] * 2 + 0x8000;
                acc[i] = Sext48(p);
                D[i] = SatS(acc[i]);
                }
            break;
        case 0x01:                                                    /* VMULU */
            for (i = 0; i < 8; i++)
                {
                s64 p = (s64)(s16)A[i] * (s64)(s16)B[i] * 2 + 0x8000;
                acc[i] = Sext48(p);
                D[i] = SatU(acc[i]);
                }
            break;
        case 0x04:                                                    /* VMUDL */
            for (i = 0; i < 8; i++)
                {
                s64 p = (s64)(((u32)A[i] * (u32)B[i]) >> 16);
                acc[i] = p;
                D[i] = SatL(p);
                }
            break;
        case 0x05:                                                    /* VMUDM */
            for (i = 0; i < 8; i++)
                {
                s64 p = Sext48((s64)(s16)A[i] * (s64)(u16)B[i]);
                acc[i] = p;
                D[i] = SatS(p);
                }
            break;
        case 0x06:                                                    /* VMUDN */
            for (i = 0; i < 8; i++)
                {
                s64 p = Sext48((s64)(u16)A[i] * (s64)(s16)B[i]);
                acc[i] = p;
                D[i] = SatL(p);
                }
            break;
        case 0x07:                                                    /* VMUDH */
            for (i = 0; i < 8; i++)
                {
                s64 p = Sext48(((s64)(s16)A[i] * (s64)(s16)B[i]) << 16);
                acc[i] = p;
                D[i] = SatS(p);
                }
            break;
        case 0x08: case 0x09:                                         /* VMACF, VMACU */
            for (i = 0; i < 8; i++)
                {
                s64 p = Sext48(acc[i] + (s64)(s16)A[i] * (s64)(s16)B[i] * 2);
                acc[i] = p;
                D[i] = (fn == 8) ? SatS(p) : SatU(p);
                }
            break;
        case 0x0C:                                                    /* VMADL */
            for (i = 0; i < 8; i++)
                {
                s64 p = Sext48(acc[i] + (s64)(((u32)A[i] * (u32)B[i]) >> 16));
                acc[i] = p;
                D[i] = SatL(p);
                }
            break;
        case 0x0D:                                                    /* VMADM */
            for (i = 0; i < 8; i++)
                {
                s64 p = Sext48(acc[i] + (s64)(s16)A[i] * (s64)(u16)B[i]);
                acc[i] = p;
                D[i] = SatS(p);
                }
            break;
        case 0x0E:                                                    /* VMADN */
            for (i = 0; i < 8; i++)
                {
                s64 p = Sext48(acc[i] + (s64)(u16)A[i] * (s64)(s16)B[i]);
                acc[i] = p;
                D[i] = SatL(p);
                }
            break;
        case 0x0F:                                                    /* VMADH */
            for (i = 0; i < 8; i++)
                {
                s64 p = Sext48(acc[i] + (((s64)(s16)A[i] * (s64)(s16)B[i]) << 16));
                acc[i] = p;
                D[i] = SatS(p);
                }
            break;
        case 0x10:                                                    /* VADD */
            {
            u32 co = iVco;
            for (i = 0; i < 8; i++)
                {
                s32 sum = (s32)(s16)A[i] + (s32)(s16)B[i] + (s32)((co >> i) & 1);
                acc[i] = (acc[i] & ~(s64)0xFFFF) | (u16)sum;
                D[i] = Clamp16(sum);
                }
            iVco = 0;
            break;
            }
        case 0x11:                                                    /* VSUB */
            {
            u32 co = iVco;
            for (i = 0; i < 8; i++)
                {
                s32 d = (s32)(s16)A[i] - (s32)(s16)B[i] - (s32)((co >> i) & 1);
                acc[i] = (acc[i] & ~(s64)0xFFFF) | (u16)d;
                D[i] = Clamp16(d);
                }
            iVco = 0;
            break;
            }
        case 0x27:                                                    /* VMRG */
            {
            u32 cc = iVcc;
            for (i = 0; i < 8; i++)
                {
                u16 r = ((cc >> i) & 1) ? A[i] : B[i];
                acc[i] = (acc[i] & ~(s64)0xFFFF) | r;
                D[i] = r;
                }
            iVco = 0;
            break;
            }
        default:                                                      /* 0x28..0x2D logic */
            for (i = 0; i < 8; i++)
                {
                u16 x = A[i], y = B[i], r;
                switch (fn)
                    {
                    case 0x28: r = x & y; break;
                    case 0x29: r = (u16)~(x & y); break;
                    case 0x2A: r = x | y; break;
                    case 0x2B: r = (u16)~(x | y); break;
                    case 0x2C: r = x ^ y; break;
                    default:   r = (u16)~(x ^ y); break;
                    }
                acc[i] = (acc[i] & ~(s64)0xFFFF) | r;
                D[i] = r;
                }
            break;
        }
    }

/* ------------------------------------------------------------------ */
/* scalar core                                                         */
/* ------------------------------------------------------------------ */
void N64Core::RunRsp(u32 aBudget)
    {
    BuildTables();
    bool fast = iGpuMode != 0;
    if (fast && iRspDecDirty) { RspDecode(); iRspDecDirty = false; }
    u32 pc = iSpPc & 0xFFCu;
    u32 npc = iRspNpc & 0xFFCu;
    u32 executed = 0;

    while (aBudget--)
        {
        if (iSpStatus & 1u) break;
        if (fast && iRspDecDirty) { RspDecode(); iRspDecDirty = false; }   /* overlay DMA into IMEM */

        u32 inst, op, rs, rt, rd;
        if (fast)
            {
            const N64RspDec& d = iRspDec[pc >> 2];
            inst = d.inst; op = d.op; rs = d.rs; rt = d.rt; rd = d.rd;
            }
        else
            {
            inst = iSp[1024 + (pc >> 2)];
            op = inst >> 26;
            rs = (inst >> 21) & 31; rt = (inst >> 16) & 31; rd = (inst >> 11) & 31;
            }
        u32 cur = pc;
        pc = npc;
        npc = (npc + 4) & 0xFFCu;
        executed++;

        u32 sa = (inst >> 6) & 31;
        s32 imm = (s32)(s16)inst;
        u32 uimm = inst & 0xFFFFu;
        u32 a = iRspR[rs], b = iRspR[rt];

        switch (op)
            {
            case 0x00:
                switch (inst & 63)
                    {
                    case 0x00: iRspR[rd] = b << sa; break;
                    case 0x02: iRspR[rd] = b >> sa; break;
                    case 0x03: iRspR[rd] = (u32)((s32)b >> sa); break;
                    case 0x04: iRspR[rd] = b << (a & 31); break;
                    case 0x06: iRspR[rd] = b >> (a & 31); break;
                    case 0x07: iRspR[rd] = (u32)((s32)b >> (a & 31)); break;
                    case 0x08: npc = a & 0xFFCu; break;
                    case 0x09: iRspR[rd] = (cur + 8) & 0xFFFu; npc = a & 0xFFCu; break;
                    case 0x0D:                                          /* BREAK */
                        iSpStatus |= 3u;
                        if (iSpStatus & 0x40u) RaiseMi(1);
                        break;
                    case 0x20: case 0x21: iRspR[rd] = a + b; break;
                    case 0x22: case 0x23: iRspR[rd] = a - b; break;
                    case 0x24: iRspR[rd] = a & b; break;
                    case 0x25: iRspR[rd] = a | b; break;
                    case 0x26: iRspR[rd] = a ^ b; break;
                    case 0x27: iRspR[rd] = ~(a | b); break;
                    case 0x2A: iRspR[rd] = ((s32)a < (s32)b) ? 1 : 0; break;
                    case 0x2B: iRspR[rd] = (a < b) ? 1 : 0; break;
                    default: iStats.unkRsp++; break;
                    }
                break;
            case 0x01:
                {
                u32 target = (cur + 4 + (u32)(imm << 2)) & 0xFFCu;
                switch (rt)
                    {
                    case 0x00: if ((s32)a < 0) npc = target; break;
                    case 0x01: if ((s32)a >= 0) npc = target; break;
                    case 0x10: iRspR[31] = (cur + 8) & 0xFFFu; if ((s32)a < 0) npc = target; break;
                    case 0x11: iRspR[31] = (cur + 8) & 0xFFFu; if ((s32)a >= 0) npc = target; break;
                    default: iStats.unkRsp++; break;
                    }
                break;
                }
            case 0x02: npc = (inst << 2) & 0xFFCu; break;
            case 0x03: iRspR[31] = (cur + 8) & 0xFFFu; npc = (inst << 2) & 0xFFCu; break;
            case 0x04: if (a == b) npc = (cur + 4 + (u32)(imm << 2)) & 0xFFCu; break;
            case 0x05: if (a != b) npc = (cur + 4 + (u32)(imm << 2)) & 0xFFCu; break;
            case 0x06: if ((s32)a <= 0) npc = (cur + 4 + (u32)(imm << 2)) & 0xFFCu; break;
            case 0x07: if ((s32)a > 0) npc = (cur + 4 + (u32)(imm << 2)) & 0xFFCu; break;
            case 0x08: case 0x09: iRspR[rt] = a + (u32)imm; break;
            case 0x0A: iRspR[rt] = ((s32)a < imm) ? 1 : 0; break;
            case 0x0B: iRspR[rt] = (a < (u32)imm) ? 1 : 0; break;
            case 0x0C: iRspR[rt] = a & uimm; break;
            case 0x0D: iRspR[rt] = a | uimm; break;
            case 0x0E: iRspR[rt] = a ^ uimm; break;
            case 0x0F: iRspR[rt] = uimm << 16; break;
            case 0x10:                                                  /* COP0 */
                if (rs == 0x00)
                    {
                    iRspR[rt] = (rd < 8) ? SpRegRd(rd) : ((rd < 16) ? DpcRd(rd - 8) : 0);
                    }
                else if (rs == 0x04)
                    {
                    if (rd < 8)
                        {
                        if (rd == 4) { iSpPc = pc; iRspNpc = npc; }
                        SpRegWr(rd, b);
                        }
                    else if (rd < 16) DpcWr(rd - 8, b);
                    }
                break;
            case 0x12:                                                  /* COP2 */
                if (rs & 0x10) { if (fast) RspVecFast(inst); else RspVec(inst); }
                else
                    {
                    u32 el = (inst >> 7) & 15;
                    switch (rs)
                        {
                        case 0x00:                                       /* MFC2 */
                            {
                            u32 lo = VB(rd, (el + 1) & 15);
                            u32 hi = VB(rd, el);
                            iRspR[rt] = (u32)(s32)(s16)((hi << 8) | lo);
                            break;
                            }
                        case 0x02:                                       /* CFC2 */
                            {
                            u32 v = (rd & 3) == 0 ? iVco : ((rd & 3) == 1 ? iVcc : iVce);
                            iRspR[rt] = (u32)(s32)(s16)v;
                            break;
                            }
                        case 0x04:                                       /* MTC2 */
                            VSetB(iVr, rd, el, (b >> 8) & 0xFF);
                            if (el != 15) VSetB(iVr, rd, el + 1, b & 0xFF);
                            break;
                        case 0x06:                                       /* CTC2 */
                            if ((rd & 3) == 0) iVco = (u16)b;
                            else if ((rd & 3) == 1) iVcc = (u16)b;
                            else iVce = (u8)b;
                            break;
                        default: iStats.unkRsp++; break;
                        }
                    }
                break;
            case 0x20: iRspR[rt] = (u32)(s32)(s8)RspRdB(a + (u32)imm); break;
            case 0x21: iRspR[rt] = (u32)(s32)(s16)((RspRdB(a + (u32)imm) << 8) | RspRdB(a + (u32)imm + 1)); break;
            case 0x23:
                iRspR[rt] = (RspRdB(a + (u32)imm) << 24) | (RspRdB(a + (u32)imm + 1) << 16) |
                            (RspRdB(a + (u32)imm + 2) << 8) | RspRdB(a + (u32)imm + 3);
                break;
            case 0x24: iRspR[rt] = RspRdB(a + (u32)imm); break;
            case 0x25: iRspR[rt] = (RspRdB(a + (u32)imm) << 8) | RspRdB(a + (u32)imm + 1); break;
            case 0x27:
                iRspR[rt] = (RspRdB(a + (u32)imm) << 24) | (RspRdB(a + (u32)imm + 1) << 16) |
                            (RspRdB(a + (u32)imm + 2) << 8) | RspRdB(a + (u32)imm + 3);
                break;
            case 0x28: RspWrB(a + (u32)imm, b); break;
            case 0x29: RspWrB(a + (u32)imm, b >> 8); RspWrB(a + (u32)imm + 1, b); break;
            case 0x2B:
                RspWrB(a + (u32)imm, b >> 24); RspWrB(a + (u32)imm + 1, b >> 16);
                RspWrB(a + (u32)imm + 2, b >> 8); RspWrB(a + (u32)imm + 3, b);
                break;
            case 0x32: RspLwc2(inst); break;
            case 0x3A: RspSwc2(inst); break;
            default: iStats.unkRsp++; break;
            }
        iRspR[0] = 0;
        }
    iStats.rspInstr += executed;
    iSpPc = pc;
    iRspNpc = npc;
    }
