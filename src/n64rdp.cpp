/*
 * n64rdp.cpp - Reality Display Processor, software implementation.
 *
 * Implemented: all 64-bit RDP commands, fill / copy / 1-cycle / 2-cycle
 * pipelines, shade+texture+z triangles, texture rectangles, TMEM loads
 * (block/tile/TLUT, all formats), point + 3-tap bilinear sampling,
 * clamp/mirror/mask/shift, full colour combiner, simplified blender,
 * 16-bit compressed z-buffer, 16/32-bit colour images.
 *
 * Simplified / missing: anti-aliasing + coverage, mip-map LOD selection,
 * fog, noise, dithering, chroma key, YUV conversion, 4/8-bit colour images.
 */
#include "n64core.h"

struct Px { s32 r, g, b, a; };

static inline s32 Clamp255(s32 v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static inline u32 Ex5(u32 v) { return (v << 3) | (v >> 2); }

/* ---------------------------- RDRAM access ------------------------- */
static inline u32 RdW(const u32* ram, u32 a)
    { return a < N64_RDRAM_SIZE ? ram[a >> 2] : 0; }
static inline u32 RdB(const u32* ram, u32 a)
    { return a < N64_RDRAM_SIZE ? (ram[a >> 2] >> ((3 - (a & 3)) * 8)) & 0xFF : 0; }
static inline u32 RdH(const u32* ram, u32 a)
    { if (a >= N64_RDRAM_SIZE) return 0; u32 w = ram[a >> 2]; return (a & 2) ? (w & 0xFFFF) : (w >> 16); }
static inline void WrH(u32* ram, u32 a, u32 v)
    {
    if (a >= N64_RDRAM_SIZE) return;
    u32 i = a >> 2, sh = (a & 2) ? 0 : 16;
    ram[i] = (ram[i] & ~(0xFFFFu << sh)) | ((v & 0xFFFFu) << sh);
    }
static inline void WrW(u32* ram, u32 a, u32 v)
    { if (a < N64_RDRAM_SIZE) ram[a >> 2] = v; }

/* ---------------------------- z-buffer format ---------------------- */
static const u32 kZBase[8]  = { 0x00000, 0x20000, 0x30000, 0x38000, 0x3C000, 0x3E000, 0x3F000, 0x3F800 };
static const u8  kZShift[8] = { 6, 5, 4, 3, 2, 1, 0, 0 };

static u32 ZDecode(u32 z16)           /* 16-bit memory word -> 18-bit z */
    {
    u32 v = (z16 >> 2) & 0x3FFF;
    u32 e = v >> 11, m = v & 0x7FF;
    return (m << kZShift[e]) + kZBase[e];
    }
static u32 ZEncode(u32 z18)           /* 18-bit z -> 16-bit memory word */
    {
    u32 e = 7;
    while (e > 0 && z18 < kZBase[e]) e--;
    u32 m = (z18 - kZBase[e]) >> kZShift[e];
    if (m > 0x7FF) m = 0x7FF;
    return ((e << 11) | m) << 2;
    }

/* ---------------------------- textures ----------------------------- */
static void DecodeTexel(const N64Rdp& r, const N64Tile& t, u32 idxFirst, u32 s, Px& o)
    {
    /* idxFirst = byte index in TMEM already swizzled for the row */
    const u8* tm = r.tmem;
    u32 fmt = t.fmt, siz = t.siz;
    u32 v;
    if (siz == 3)                                   /* 32-bit RGBA, two banks */
        {
        u32 i = idxFirst & 0x7FE;
        o.r = tm[i]; o.g = tm[i + 1]; o.b = tm[0x800 + i]; o.a = tm[0x800 + i + 1];
        if (fmt != 0) { o.g = o.b = o.r; }
        return;
        }
    if (siz == 2)
        {
        v = ((u32)tm[idxFirst & 0xFFE] << 8) | tm[(idxFirst & 0xFFE) + 1];
        }
    else
        {
        u32 b = tm[idxFirst & 0xFFF];
        if (siz == 1) v = b;
        else v = (s & 1) ? (b & 15) : (b >> 4);
        }

    if (fmt == 2)                                   /* colour index -> TLUT */
        {
        u32 entry = (siz == 0) ? (((u32)t.pal << 4) | v) : v;
        u32 pi = (0x800 + entry * 8) & 0xFFE;
        u32 c = ((u32)tm[pi] << 8) | tm[pi + 1];
        if ((r.omHi >> 14) & 1) { o.r = o.g = o.b = c >> 8; o.a = c & 0xFF; }
        else { o.r = Ex5((c >> 11) & 31); o.g = Ex5((c >> 6) & 31); o.b = Ex5((c >> 1) & 31); o.a = (c & 1) ? 255 : 0; }
        return;
        }
    switch (fmt)
        {
        case 0:                                     /* RGBA */
            if (siz == 2) { o.r = Ex5((v >> 11) & 31); o.g = Ex5((v >> 6) & 31); o.b = Ex5((v >> 1) & 31); o.a = (v & 1) ? 255 : 0; }
            else { o.r = o.g = o.b = (siz == 1) ? (s32)v : (s32)(v * 17); o.a = 255; }
            break;
        case 1:                                     /* YUV (approx: luma only) */
            o.r = o.g = o.b = (siz == 2) ? (s32)(v & 0xFF) : (s32)v; o.a = 255;
            break;
        case 3:                                     /* IA */
            if (siz == 0)      { u32 i3 = (v >> 1) & 7; o.r = o.g = o.b = (i3 << 5) | (i3 << 2) | (i3 >> 1); o.a = (v & 1) ? 255 : 0; }
            else if (siz == 1) { o.r = o.g = o.b = ((v >> 4) << 4) | (v >> 4); o.a = ((v & 15) << 4) | (v & 15); }
            else               { o.r = o.g = o.b = v >> 8; o.a = v & 0xFF; }
            break;
        default:                                    /* I */
            if (siz == 0)      { o.r = o.g = o.b = o.a = (v << 4) | v; }
            else if (siz == 1) { o.r = o.g = o.b = o.a = v; }
            else               { o.r = o.g = o.b = o.a = v >> 8; }
            break;
        }
    }

static void TexelAt(const N64Rdp& r, const N64Tile& t, s32 s, s32 tt, Px& o)
    {
    s32 smax = ((s32)t.sh - (s32)t.sl) >> 2;
    s32 tmax = ((s32)t.th - (s32)t.tl) >> 2;
    if (t.cmS & 2) { if (s < 0) s = 0; else if (s > smax) s = smax; }
    if (t.cmT & 2) { if (tt < 0) tt = 0; else if (tt > tmax) tt = tmax; }
    if (t.maskS) { if ((t.cmS & 1) && (s & (1 << t.maskS))) s = ~s; s &= (1 << t.maskS) - 1; }
    if (t.maskT) { if ((t.cmT & 1) && (tt & (1 << t.maskT))) tt = ~tt; tt &= (1 << t.maskT) - 1; }

    u32 base = (u32)t.tmem * 8 + (u32)tt * (u32)t.line * 8;
    u32 xr = (tt & 1) ? 4u : 0u;
    u32 off;
    switch (t.siz)
        {
        case 0:  off = base + ((u32)s >> 1); break;
        case 1:  off = base + (u32)s; break;
        default: off = base + (u32)s * 2; break;
        }
    DecodeTexel(r, t, (off ^ xr) & 0xFFF, (u32)s, o);
    }

static s32 ApplyShift(s32 v, u32 sh)
    {
    if (sh == 0) return v;
    if (sh <= 10) return v >> sh;
    return v << (16 - sh);
    }

static void TexSample(const N64Rdp& r, u32 tileIdx, s32 sss, s32 sst, Px& o)
    {
    const N64Tile& t = r.tiles[tileIdx & 7];
    s32 s = ApplyShift(sss, t.shiftS) - ((s32)t.sl << 3);
    s32 tt = ApplyShift(sst, t.shiftT) - ((s32)t.tl << 3);
    if (!((r.omHi >> 13) & 1) || (r.dbg & 4))
        {
        TexelAt(r, t, s >> 5, tt >> 5, o);
        return;
        }
    s -= 16; tt -= 16;
    s32 s0 = s >> 5, t0 = tt >> 5, fs = s & 31, ft = tt & 31;
    Px a, b, c, d;                                  /* 00, 10, 01, 11 */
    TexelAt(r, t, s0, t0, a);
    TexelAt(r, t, s0 + 1, t0, b);
    TexelAt(r, t, s0, t0 + 1, c);
    if (fs + ft < 32)
        {
        o.r = a.r + (((b.r - a.r) * fs + (c.r - a.r) * ft) >> 5);
        o.g = a.g + (((b.g - a.g) * fs + (c.g - a.g) * ft) >> 5);
        o.b = a.b + (((b.b - a.b) * fs + (c.b - a.b) * ft) >> 5);
        o.a = a.a + (((b.a - a.a) * fs + (c.a - a.a) * ft) >> 5);
        }
    else
        {
        TexelAt(r, t, s0 + 1, t0 + 1, d);
        o.r = d.r + (((c.r - d.r) * (32 - fs) + (b.r - d.r) * (32 - ft)) >> 5);
        o.g = d.g + (((c.g - d.g) * (32 - fs) + (b.g - d.g) * (32 - ft)) >> 5);
        o.b = d.b + (((c.b - d.b) * (32 - fs) + (b.b - d.b) * (32 - ft)) >> 5);
        o.a = d.a + (((c.a - d.a) * (32 - fs) + (b.a - d.a) * (32 - ft)) >> 5);
        }
    }

/* ---------------------------- colour combiner ---------------------- */
struct CombIn { Px comb, t0, t1, prim, env, shade; s32 lod, primLod; };

static void SelRgb(const CombIn& I, u32 kind, u32 s, s32* o)
    {
    const Px* p = 0;
    s32 a = -1;
    switch (kind)
        {
        case 0:  /* A */
            switch (s) { case 0: p=&I.comb; break; case 1: p=&I.t0; break; case 2: p=&I.t1; break; case 3: p=&I.prim; break;
                         case 4: p=&I.shade; break; case 5: p=&I.env; break; case 6: o[0]=o[1]=o[2]=255; return;
                         case 7: o[0]=o[1]=o[2]=128; return; default: o[0]=o[1]=o[2]=0; return; }
            break;
        case 1:  /* B */
            switch (s) { case 0: p=&I.comb; break; case 1: p=&I.t0; break; case 2: p=&I.t1; break; case 3: p=&I.prim; break;
                         case 4: p=&I.shade; break; case 5: p=&I.env; break; default: o[0]=o[1]=o[2]=0; return; }
            break;
        case 2:  /* C */
            switch (s) { case 0: p=&I.comb; break; case 1: p=&I.t0; break; case 2: p=&I.t1; break; case 3: p=&I.prim; break;
                         case 4: p=&I.shade; break; case 5: p=&I.env; break;
                         case 7: a=I.comb.a; break; case 8: a=I.t0.a; break; case 9: a=I.t1.a; break; case 10: a=I.prim.a; break;
                         case 11: a=I.shade.a; break; case 12: a=I.env.a; break; case 13: a=I.lod; break; case 14: a=I.primLod; break;
                         default: a=0; break; }
            if (a >= 0) { o[0]=o[1]=o[2]=a; return; }
            break;
        default: /* D */
            switch (s) { case 0: p=&I.comb; break; case 1: p=&I.t0; break; case 2: p=&I.t1; break; case 3: p=&I.prim; break;
                         case 4: p=&I.shade; break; case 5: p=&I.env; break; case 6: o[0]=o[1]=o[2]=255; return;
                         default: o[0]=o[1]=o[2]=0; return; }
            break;
        }
    o[0] = p->r; o[1] = p->g; o[2] = p->b;
    }

static s32 SelAlpha(const CombIn& I, u32 kind, u32 s)
    {
    if (kind == 2)                                 /* C */
        {
        switch (s) { case 0: return I.lod; case 1: return I.t0.a; case 2: return I.t1.a; case 3: return I.prim.a;
                     case 4: return I.shade.a; case 5: return I.env.a; case 6: return I.primLod; default: return 0; }
        }
    switch (s)
        {
        case 0: return I.comb.a; case 1: return I.t0.a; case 2: return I.t1.a; case 3: return I.prim.a;
        case 4: return I.shade.a; case 5: return I.env.a; case 6: return 255; default: return 0;
        }
    }

static void Combine(const N64Rdp& r, const CombIn& I, u32 cyc, Px& out)
    {
    u32 hi = r.combHi, lo = r.combLo;
    u32 ar, br, cr, dr, aa, ba, ca, da;
    if (cyc == 0)
        {
        ar = (hi >> 20) & 15; cr = (hi >> 15) & 31; aa = (hi >> 12) & 7; ca = (hi >> 9) & 7;
        br = (lo >> 28) & 15; dr = (lo >> 15) & 7;  ba = (lo >> 12) & 7; da = (lo >> 9) & 7;
        }
    else
        {
        ar = (hi >> 5) & 15;  cr = hi & 31;         aa = (lo >> 21) & 7; ca = (lo >> 18) & 7;
        br = (lo >> 24) & 15; dr = (lo >> 6) & 7;   ba = (lo >> 3) & 7;  da = lo & 7;
        }
    s32 A[3], B[3], C[3], D[3];
    SelRgb(I, 0, ar, A); SelRgb(I, 1, br, B); SelRgb(I, 2, cr, C); SelRgb(I, 3, dr, D);
    s32 res[3];
    for (int k = 0; k < 3; k++)
        {
        s32 c = C[k] + (C[k] >> 7);
        res[k] = Clamp255((((A[k] - B[k]) * c + 0x80) >> 8) + D[k]);
        }
    out.r = res[0]; out.g = res[1]; out.b = res[2];
    s32 a = SelAlpha(I, 0, aa), b = SelAlpha(I, 1, ba), c = SelAlpha(I, 2, ca), d = SelAlpha(I, 3, da);
    c += (c >> 7);
    out.a = Clamp255((((a - b) * c + 0x80) >> 8) + d);
    }

/* ---------------------------- blender / framebuffer ---------------- */
static void ReadFb(const N64Rdp& r, const u32* ram, s32 x, s32 y, Px& m)
    {
    u32 idx = (u32)y * r.ciWidth + (u32)x;
    if (r.ciSiz == 3)
        {
        u32 w = RdW(ram, r.ciAddr + idx * 4);
        m.r = w >> 24; m.g = (w >> 16) & 0xFF; m.b = (w >> 8) & 0xFF; m.a = w & 0xFF;
        }
    else
        {
        u32 c = RdH(ram, r.ciAddr + idx * 2);
        m.r = Ex5((c >> 11) & 31); m.g = Ex5((c >> 6) & 31); m.b = Ex5((c >> 1) & 31); m.a = (c & 1) ? 255 : 0;
        }
    }

static void WriteFb(const N64Rdp& r, u32* ram, s32 x, s32 y, const Px& p)
    {
    u32 idx = (u32)y * r.ciWidth + (u32)x;
    if (r.ciSiz == 3)
        WrW(ram, r.ciAddr + idx * 4, ((u32)p.r << 24) | ((u32)p.g << 16) | ((u32)p.b << 8) | (u32)p.a);
    else if (r.ciSiz == 2)
        WrH(ram, r.ciAddr + idx * 2, ((u32)(p.r >> 3) << 11) | ((u32)(p.g >> 3) << 6) | ((u32)(p.b >> 3) << 1) | 1u);
    }

static void BlendStage(const N64Rdp& r, const Px& P, s32 pa, s32 sa, const Px& M, u32 m1a, u32 m1b, u32 m2a, u32 m2b, bool force, Px& out)
    {
    Px B, F;
    B.r = r.blendColor >> 24; B.g = (r.blendColor >> 16) & 0xFF; B.b = (r.blendColor >> 8) & 0xFF; B.a = r.blendColor & 0xFF;
    F.r = r.fogColor >> 24;   F.g = (r.fogColor >> 16) & 0xFF;   F.b = (r.fogColor >> 8) & 0xFF;   F.a = r.fogColor & 0xFF;
    const Px* sel[4] = { &P, &M, &B, &F };
    const Px& A1 = *sel[m1a];
    out = A1;
    if (!force) return;
    const Px& A2 = *sel[m2a];
    s32 wa = (m1b == 0) ? pa : (m1b == 1 ? F.a : (m1b == 2 ? sa : 0));
    s32 wb = (m2b == 0) ? (255 - wa) : (m2b == 1 ? M.a : (m2b == 2 ? 255 : 0));
    s32 sum = wa + wb;
    if (sum <= 0) return;
    out.r = (A1.r * wa + A2.r * wb) / sum;
    out.g = (A1.g * wa + A2.g * wb) / sum;
    out.b = (A1.b * wa + A2.b * wb) / sum;
    }

struct TexCtx { s32 s, t; u32 tile; };

static bool DrawPixel(N64Rdp& r, u32* ram, s32 x, s32 y, const Px& shade, const TexCtx* tc, bool hasZ, u32 z18)
    {
    if ((x << 2) < (s32)r.scXH || (x << 2) >= (s32)r.scXL || (y << 2) < (s32)r.scYH || (y << 2) >= (s32)r.scYL) return false;
    u32 cyc = (r.omHi >> 20) & 3;
    u32 om = r.omLo;

    bool zOn = ((om & 0x10) || (om & 0x20));
    u32 zaddr = r.ziAddr + ((u32)y * r.ciWidth + (u32)x) * 2;
    if (!hasZ && (om & 4)) { hasZ = true; z18 = ((u32)r.primZ << 2) & 0x3FFFF; }
    if (hasZ && zOn)
        {
        u32 zold16 = RdH(ram, zaddr);
        u32 zold = ZDecode(zold16);
        if ((om & 0x10) && !(r.dbg & 1))
            {
            if (((om >> 10) & 3) == 3)                  /* decal: pass when (nearly) coplanar */
                {
                u32 step = (1u << kZShift[((zold16 >> 2) >> 11) & 7]) * 2;
                if (z18 > zold + step || z18 + step < zold) return false;
                }
            else if (z18 > zold) return false;
            }
        }

    Px t0 = { 0, 0, 0, 255 }, t1 = { 0, 0, 0, 255 };
    if (tc)
        {
        TexSample(r, tc->tile, tc->s, tc->t, t0);
        if (cyc == 1) TexSample(r, (tc->tile + 1) & 7, tc->s, tc->t, t1);
        }

    CombIn I;
    I.comb.r = I.comb.g = I.comb.b = I.comb.a = 0;
    I.t0 = t0; I.t1 = t1; I.shade = shade;
    I.prim.r = r.primColor >> 24; I.prim.g = (r.primColor >> 16) & 0xFF; I.prim.b = (r.primColor >> 8) & 0xFF; I.prim.a = r.primColor & 0xFF;
    I.env.r = r.envColor >> 24;   I.env.g = (r.envColor >> 16) & 0xFF;   I.env.b = (r.envColor >> 8) & 0xFF;   I.env.a = r.envColor & 0xFF;
    I.lod = 255; I.primLod = r.primLodFrac;

    Px c;
    if (cyc == 1)
        {
        Combine(r, I, 0, c);
        I.comb = c;
        I.t0 = t1; I.t1 = t0;
        Combine(r, I, 1, c);
        }
    else Combine(r, I, 0, c);

    if ((om & 1) && !(r.dbg & 2))                       /* alpha compare */
        {
        if (!(om & 2) && c.a < (s32)(r.blendColor & 0xFF)) return false;
        if ((om & 2) && c.a < 0x40) return false;
        }

    bool force = (om & 0x4000) != 0;
    Px mem = { 0, 0, 0, 255 };
    if (force || (om & 0x40)) ReadFb(r, ram, x, y, mem);

    Px o;
    if (cyc == 1)
        {
        Px o0;
        BlendStage(r, c, c.a, shade.a, mem, (r.omLo >> 30) & 3, (r.omLo >> 26) & 3, (r.omLo >> 22) & 3, (r.omLo >> 18) & 3, true, o0);
        BlendStage(r, o0, c.a, shade.a, mem, (r.omLo >> 28) & 3, (r.omLo >> 24) & 3, (r.omLo >> 20) & 3, (r.omLo >> 16) & 3, force, o);
        }
    else
        BlendStage(r, c, c.a, shade.a, mem, (r.omLo >> 30) & 3, (r.omLo >> 26) & 3, (r.omLo >> 22) & 3, (r.omLo >> 18) & 3, force, o);
    o.a = c.a;
    o.r = Clamp255(o.r); o.g = Clamp255(o.g); o.b = Clamp255(o.b);

    WriteFb(r, ram, x, y, o);
    if (hasZ && (om & 0x20)) WrH(ram, zaddr, ZEncode(z18));
        return true;
    }

/* resolution scaling: replicate an already rendered pixel (colour + z) */
static void CopyPixel(const N64Rdp& r, u32* ram, s32 sx, s32 sy, s32 dx, s32 dy)
    {
    u32 si = (u32)sy * r.ciWidth + (u32)sx, di = (u32)dy * r.ciWidth + (u32)dx;
    if (r.ciSiz == 3) WrW(ram, r.ciAddr + di * 4, RdW(ram, r.ciAddr + si * 4));
    else if (r.ciSiz == 2) WrH(ram, r.ciAddr + di * 2, RdH(ram, r.ciAddr + si * 2));
    if (r.omLo & 0x20) WrH(ram, r.ziAddr + di * 2, RdH(ram, r.ziAddr + si * 2));
    }

/* fill-mode pixel */
static void FillPixel(const N64Rdp& r, u32* ram, s32 x, s32 y)
    {
    if ((x << 2) < (s32)r.scXH || (x << 2) >= (s32)r.scXL || (y << 2) < (s32)r.scYH || (y << 2) >= (s32)r.scYL) return;
    u32 idx = (u32)y * r.ciWidth + (u32)x;
    if (r.ciSiz == 3) WrW(ram, r.ciAddr + idx * 4, r.fillColor);
    else if (r.ciSiz == 2) WrH(ram, r.ciAddr + idx * 2, (x & 1) ? (r.fillColor & 0xFFFF) : (r.fillColor >> 16));
    }

/* ---------------------------- pipe analysis ------------------------ */
/* which inputs does the current combiner/blender really read?  (cached mode
   skips computing the others; conservative = answers "used" when unsure) */
static void UpdatePipe(N64Rdp& r)
    {
    r.pipeDirty = 0;
    r.usesTex = 1; r.usesShade = 1;
    u32 cyc = (r.omHi >> 20) & 3;
    if (cyc >= 2) return;
    u32 hi = r.combHi, lo = r.combLo;
    bool tex = false, sh = false;
    u32 passes = (cyc == 1) ? 2 : 1;
    for (u32 c = 0; c < passes; c++)
        {
        u32 ar, br, cr, dr, aa, ba, ca, da;
        if (c == 0)
            {
            ar = (hi >> 20) & 15; cr = (hi >> 15) & 31; aa = (hi >> 12) & 7; ca = (hi >> 9) & 7;
            br = (lo >> 28) & 15; dr = (lo >> 15) & 7;  ba = (lo >> 12) & 7; da = (lo >> 9) & 7;
            }
        else
            {
            ar = (hi >> 5) & 15;  cr = hi & 31;         aa = (lo >> 21) & 7; ca = (lo >> 18) & 7;
            br = (lo >> 24) & 15; dr = (lo >> 6) & 7;   ba = (lo >> 3) & 7;  da = lo & 7;
            }
        u32 gen[7] = { ar, br, dr, aa, ba, ca, da };
        for (u32 k = 0; k < 7; k++)
            {
            if (gen[k] == 1 || gen[k] == 2) tex = true;
            if (gen[k] == 4) sh = true;
            }
        if (cr == 1 || cr == 2 || cr == 8 || cr == 9) tex = true;
        if (cr == 4 || cr == 11) sh = true;
        }
    if (((r.omLo >> 26) & 3) == 2) sh = true;                 /* blender cycle 0 uses shade alpha */
    if (cyc == 1 && ((r.omLo >> 24) & 3) == 2) sh = true;     /* blender cycle 1 */
    r.usesTex = tex ? 1 : 0;
    r.usesShade = sh ? 1 : 0;
    }

/* ---------------------------- triangles ---------------------------- */
/* x position of both edges at sub-scanline ys */
static inline void EdgesAt(s32 ys, s32 baseY, s32 ym, s32 xh, s32 dxhdy, s32 xm, s32 dxmdy, s32 xl, s32 dxldy,
                           bool lft, s64& xhv, s64& left, s64& right)
    {
    s32 dyp = ys - baseY;
    xhv = (s64)xh + (((s64)dxhdy * dyp) >> 2);
    s64 xmv = (ys < ym) ? (s64)xm + (((s64)dxmdy * dyp) >> 2)
                        : (s64)xl + (((s64)dxldy * (ys - ym)) >> 2);
    left = lft ? xhv : xmv;
    right = lft ? xmv : xhv;
    }

static s32 Sext14(u32 v) { v &= 0x3FFF; return (v & 0x2000) ? (s32)v - 0x4000 : (s32)v; }
static s32 Fx(u32 iw, u32 fw, u32 half)    /* 16.16 from int/frac words, half 0=high 1=low */
    {
    u32 sh = half ? 0 : 16;
    return (s32)((((iw >> sh) & 0xFFFFu) << 16) | ((fw >> sh) & 0xFFFFu));
    }

static void Triangle(N64Rdp& r, u32* ram, const u32* q, u32 cmd)
    {
    bool hasShade = (cmd & 4) != 0, hasTex = (cmd & 2) != 0, hasZ = (cmd & 1) != 0;
    bool lft = ((q[0] >> 23) & 1) != 0;
    u32 tile = (q[0] >> 16) & 7;
    s32 yl = Sext14(q[0]), ym = Sext14(q[1] >> 16), yh = Sext14(q[1]);
    s32 xl = (s32)q[2], dxldy = (s32)q[3], xh = (s32)q[4], dxhdy = (s32)q[5], xm = (s32)q[6], dxmdy = (s32)q[7];

    u32 pos = 8;
    s32 sh0[4] = {0,0,0,0}, shDx[4] = {0,0,0,0}, shDe[4] = {0,0,0,0};
    s32 tx0[3] = {0,0,0}, txDx[3] = {0,0,0}, txDe[3] = {0,0,0};
    s32 z0 = 0, zDx = 0, zDe = 0;
    u32 k;
    if (hasShade)
        {
        for (k = 0; k < 4; k++)
            {
            u32 w = k >> 1, h = k & 1;
            sh0[k]  = Fx(q[pos + w],     q[pos + 4 + w],  h);
            shDx[k] = Fx(q[pos + 2 + w], q[pos + 6 + w],  h);
            shDe[k] = Fx(q[pos + 8 + w], q[pos + 12 + w], h);
            }
        pos += 16;
        }
    if (hasTex)
        {
        for (k = 0; k < 3; k++)
            {
            u32 w = k >> 1, h = k & 1;
            tx0[k]  = Fx(q[pos + w],     q[pos + 4 + w],  h);
            txDx[k] = Fx(q[pos + 2 + w], q[pos + 6 + w],  h);
            txDe[k] = Fx(q[pos + 8 + w], q[pos + 12 + w], h);
            }
        pos += 16;
        }
    if (hasZ)
        {
        z0 = (s32)q[pos]; zDx = (s32)q[pos + 1]; zDe = (s32)q[pos + 2];
        }

    u32 cyc = (r.omHi >> 20) & 3;
    bool persp = ((r.omHi >> 19) & 1) != 0;
    bool fastp = r.fast != 0;
    if (fastp && r.pipeDirty) UpdatePipe(r);
    bool wide = !(r.dbg & 8) && !(r.omLo & 0x4000);   /* wide edges unless alpha blending / exact mode */

    s32 y0 = yh >> 2, y1 = (yl + 3) >> 2;          /* include partially covered first/last rows */
    s32 baseY = yh & ~3;                            /* edge x / attributes are given at the snapped scanline */
    s32 sy0 = ((s32)r.scYH + 3) >> 2, sy1 = ((s32)r.scYL + 3) >> 2;
    if (y0 < sy0) y0 = sy0;
    if (y1 > sy1) y1 = sy1;
    s32 sx0 = ((s32)r.scXH + 3) >> 2, sx1 = ((s32)r.scXL + 3) >> 2;
    if (sx1 > 1024) sx1 = 1024;

    u32 qs = r.resQ ? r.resQ : 256;
    bool scaled = qs < 256;
    u8 flagBuf[2][1024];
    u8* prevF = flagBuf[0];
    u8* curF = flagBuf[1];
    s32 pxs = 0, pxe = 0;                             /* span covered by prevF */

    for (s32 y = y0; y < y1; y++)
        {
        s32 ys = y * 4;
        s32 ysamp = (ys < yh) ? yh : ys;                /* first valid sub-scanline of this row */
        s32 dyp = ysamp - baseY;
        s64 xhv, left, right;
        EdgesAt(ysamp, baseY, ym, xh, dxhdy, xm, dxmdy, xl, dxldy, lft, xhv, left, right);
        s32 xs, xe;
        if (wide)                                       /* conservative: any pixel touched by this row */
            {
            s32 yb = ys + 3;
            if (yb > yl - 1) yb = yl - 1;
            if (yb < ysamp) yb = ysamp;
            s64 xh2, left2, right2;
            EdgesAt(yb, baseY, ym, xh, dxhdy, xm, dxmdy, xl, dxldy, lft, xh2, left2, right2);
            s64 lmin = (left < left2) ? left : left2;
            s64 rmax = (right > right2) ? right : right2;
            xs = (s32)(lmin >> 16);
            xe = (s32)((rmax + 0xFFFF) >> 16);
            }
        else
            {
            xs = (s32)((left + 0x8000) >> 16);
            xe = (s32)((right + 0x8000) >> 16);
            }
        if (xs < sx0) xs = sx0;
        if (xe > sx1) xe = sx1;
        if (xs >= xe) { pxs = pxe = 0; continue; }

        if (scaled && y > y0 && ((((u32)y * qs) >> 8) == (((u32)(y - 1) * qs) >> 8)))
            {
            for (s32 x = xs; x < xe; x++)               /* skipped row: copy the row above */
                {
                bool src = (x >= pxs && x < pxe && prevF[x]);
                if (src) CopyPixel(r, ram, x, y - 1, x, y);
                curF[x] = src ? 1 : 0;
                }
            { u8* t = prevF; prevF = curF; curF = t; }
            pxs = xs; pxe = xe;
            continue;
            }

        s64 shRow[4], txRow[3], zRow = 0;
        for (k = 0; k < 4; k++) shRow[k] = (s64)sh0[k] + (((s64)shDe[k] * dyp) >> 2);
        for (k = 0; k < 3; k++) txRow[k] = (s64)tx0[k] + (((s64)txDe[k] * dyp) >> 2);
        if (hasZ) zRow = (s64)z0 + (((s64)zDe * dyp) >> 2);

        s64 shV[4] = {0,0,0,0}, txV[3] = {0,0,0}, zV = 0;
        if (fastp)                                      /* start values; then just add the x-slopes */
            {
            s64 dx0 = ((s64)xs << 16) - xhv;
            for (k = 0; k < 4; k++) shV[k] = shRow[k] + ((shDx[k] * dx0) >> 16);
            for (k = 0; k < 3; k++) txV[k] = txRow[k] + ((txDx[k] * dx0) >> 16);
            if (hasZ) zV = zRow + ((zDx * dx0) >> 16);
            }

        for (s32 x = xs; x < xe; x++)
            {
            if (fastp && x > xs)
                {
                for (k = 0; k < 4; k++) shV[k] += shDx[k];
                for (k = 0; k < 3; k++) txV[k] += txDx[k];
                if (hasZ) zV += zDx;
                }
            s64 dx = ((s64)x << 16) - xhv;
            if (scaled && x > xs && ((((u32)x * qs) >> 8) == (((u32)(x - 1) * qs) >> 8)) && curF[x - 1])
                {
                CopyPixel(r, ram, x - 1, y, x, y);      /* skipped column: copy left neighbour */
                curF[x] = 1;
                continue;
                }
            if (cyc == 3) { FillPixel(r, ram, x, y); curF[x] = 1; continue; }

            Px shade = { 255, 255, 255, 255 };
            if (hasShade && !(fastp && !r.usesShade))
                {
                if (fastp)
                    {
                    shade.r = Clamp255((s32)(shV[0] >> 16));
                    shade.g = Clamp255((s32)(shV[1] >> 16));
                    shade.b = Clamp255((s32)(shV[2] >> 16));
                    shade.a = Clamp255((s32)(shV[3] >> 16));
                    }
                else
                    {
                    shade.r = Clamp255((s32)((shRow[0] + ((shDx[0] * dx) >> 16)) >> 16));
                    shade.g = Clamp255((s32)((shRow[1] + ((shDx[1] * dx) >> 16)) >> 16));
                    shade.b = Clamp255((s32)((shRow[2] + ((shDx[2] * dx) >> 16)) >> 16));
                    shade.a = Clamp255((s32)((shRow[3] + ((shDx[3] * dx) >> 16)) >> 16));
                    }
                }
            TexCtx tc;
            const TexCtx* tcp = 0;
            if (hasTex && !(fastp && !r.usesTex))
                {
                s64 S, T, W;
                if (fastp) { S = txV[0]; T = txV[1]; W = txV[2]; }
                else
                    {
                    S = txRow[0] + ((txDx[0] * dx) >> 16);
                    T = txRow[1] + ((txDx[1] * dx) >> 16);
                    W = txRow[2] + ((txDx[2] * dx) >> 16);
                    }
                if (persp)
                    {
                    if (W <= 0) W = 1;
                    tc.s = (s32)((S * 32768) / W);
                    tc.t = (s32)((T * 32768) / W);
                    }
                else { tc.s = (s32)(S >> 16); tc.t = (s32)(T >> 16); }
                tc.tile = tile;
                tcp = &tc;
                }
            u32 z18 = 0;
            if (hasZ)
                {
                s64 Z = fastp ? zV : (zRow + ((zDx * dx) >> 16));
                if (Z < 0) Z = 0;
                z18 = (u32)((u64)Z >> 13);
                if (z18 > 0x3FFFF) z18 = 0x3FFFF;
                }
            curF[x] = DrawPixel(r, ram, x, y, shade, tcp, hasZ, z18) ? 1 : 0;
            }
        { u8* t = prevF; prevF = curF; curF = t; }
        pxs = xs; pxe = xe;
        }
    }

/* ---------------------------- rectangles --------------------------- */
static void FillRect(N64Rdp& r, u32* ram, u32 hi, u32 lo)
    {
    s32 xl = (hi >> 12) & 0xFFF, yl = hi & 0xFFF, xh = (lo >> 12) & 0xFFF, yh = lo & 0xFFF;
    u32 cyc = (r.omHi >> 20) & 3;
    if (cyc >= 2) { xl += 4; yl += 4; }              /* fill/copy: lower-right inclusive */
    Px shade = { 0, 0, 0, 255 };
    s32 ya = yh >> 2, yb = (yl + 3) >> 2, xa = xh >> 2, xb = (xl + 3) >> 2;
    if (ya < ((s32)r.scYH >> 2)) ya = (s32)r.scYH >> 2;
    if (yb > (((s32)r.scYL + 3) >> 2)) yb = ((s32)r.scYL + 3) >> 2;
    if (xa < ((s32)r.scXH >> 2)) xa = (s32)r.scXH >> 2;
    if (xb > (((s32)r.scXL + 3) >> 2)) xb = ((s32)r.scXL + 3) >> 2;
    for (s32 y = ya; y < yb; y++)
        for (s32 x = xa; x < xb; x++)
            {
            if (cyc == 3) FillPixel(r, ram, x, y);
            else DrawPixel(r, ram, x, y, shade, 0, false, 0);
            }
    }

static void TexRect(N64Rdp& r, u32* ram, const u32* q)
    {
    u32 hi = q[0], lo = q[1];
    s32 xl = (hi >> 12) & 0xFFF, yl = hi & 0xFFF, xh = (lo >> 12) & 0xFFF, yh = lo & 0xFFF;
    u32 tile = (lo >> 24) & 7;
    s32 S = (s16)(q[2] >> 16), T = (s16)q[2];
    s32 dsdx = (s16)(q[3] >> 16), dtdy = (s16)q[3];
    u32 cyc = (r.omHi >> 20) & 3;
    bool copy = (cyc == 2);
    if (cyc >= 2) { xl += 4; yl += 4; }
    if (copy) dsdx >>= 2;
    Px shade = { 0, 0, 0, 255 };
    s32 x0 = xh >> 2, y0 = yh >> 2, x1 = (xl + 3) >> 2, y1 = (yl + 3) >> 2;
    s32 ya = y0, yb = y1, xa = x0, xb = x1;
    if (ya < ((s32)r.scYH >> 2)) ya = (s32)r.scYH >> 2;
    if (yb > (((s32)r.scYL + 3) >> 2)) yb = ((s32)r.scYL + 3) >> 2;
    if (xa < ((s32)r.scXH >> 2)) xa = (s32)r.scXH >> 2;
    if (xb > (((s32)r.scXL + 3) >> 2)) xb = ((s32)r.scXL + 3) >> 2;
    for (s32 y = ya; y < yb; y++)
        for (s32 x = xa; x < xb; x++)
            {
            TexCtx tc;
            tc.tile = tile;
            tc.s = S + ((dsdx * (x - x0)) >> 5);
            tc.t = T + ((dtdy * (y - y0)) >> 5);
            if (copy)
                {
                if ((x << 2) < (s32)r.scXH || (x << 2) >= (s32)r.scXL || (y << 2) < (s32)r.scYH || (y << 2) >= (s32)r.scYL) continue;
                Px t;
                TexSample(r, tile, tc.s, tc.t, t);
                if ((r.omLo & 1) && t.a < (s32)(r.blendColor & 0xFF)) continue;
                t.r = Clamp255(t.r); t.g = Clamp255(t.g); t.b = Clamp255(t.b);
                WriteFb(r, ram, x, y, t);
                }
            else DrawPixel(r, ram, x, y, shade, &tc, false, 0);
            }
    }

/* ---------------------------- texture memory loads ----------------- */
static void LoadBlock(N64Rdp& r, const u32* ram, u32 hi, u32 lo)
    {
    u32 sl = (hi >> 12) & 0xFFF, tl = hi & 0xFFF, tile = (lo >> 24) & 7, sh = (lo >> 12) & 0xFFF, dxt = lo & 0xFFF;
    u32 count = (sh >= sl) ? sh - sl + 1 : 0;
    u32 base = (u32)r.tiles[tile].tmem * 8;
    u32 src = r.tiAddr + (((tl * (u32)r.tiWidth + sl) << r.tiSiz) >> 1);
    u32 i, k;
    if (r.tiSiz == 3)
        {
        for (i = 0; i < count; i++)
            {
            u32 w = RdW(ram, src + i * 4);
            u32 t = ((i >> 2) * dxt) >> 11;
            u32 idx = ((base + i * 2) ^ ((t & 1) ? 4u : 0u)) & 0x7FE;
            r.tmem[idx] = (u8)(w >> 24); r.tmem[idx + 1] = (u8)(w >> 16);
            r.tmem[0x800 + idx] = (u8)(w >> 8); r.tmem[0x800 + idx + 1] = (u8)w;
            }
        return;
        }
    u32 bytes = (r.tiSiz == 0) ? (count + 1) >> 1 : (count << (r.tiSiz - 1));
    u32 words = (bytes + 7) >> 3;
    for (i = 0; i < words; i++)
        {
        u32 t = (i * dxt) >> 11;
        u32 sw = (t & 1) ? 4u : 0u;
        for (k = 0; k < 8; k++)
            r.tmem[(base + i * 8 + (k ^ sw)) & 0xFFF] = (u8)RdB(ram, src + i * 8 + k);
        }
    }

static void LoadTile(N64Rdp& r, const u32* ram, u32 hi, u32 lo)
    {
    u32 sl = (hi >> 12) & 0xFFF, tl = hi & 0xFFF, tile = (lo >> 24) & 7, sh = (lo >> 12) & 0xFFF, th = lo & 0xFFF;
    N64Tile& T = r.tiles[tile];
    T.sl = (u16)sl; T.tl = (u16)tl; T.sh = (u16)sh; T.th = (u16)th;
    u32 s0 = sl >> 2, s1 = sh >> 2, t0 = tl >> 2, t1 = th >> 2;
    u32 base = (u32)T.tmem * 8;
    for (u32 t = t0; t <= t1 && t < t0 + 512; t++)
        {
        u32 row = t - t0;
        u32 sw = (row & 1) ? 4u : 0u;
        u32 src = r.tiAddr + (((t * (u32)r.tiWidth + s0) << r.tiSiz) >> 1);
        u32 dst = base + row * (u32)T.line * 8;
        if (r.tiSiz == 3)
            {
            for (u32 s = s0; s <= s1; s++)
                {
                u32 w = RdW(ram, src + (s - s0) * 4);
                u32 idx = ((dst + (s - s0) * 2) ^ sw) & 0x7FE;
                r.tmem[idx] = (u8)(w >> 24); r.tmem[idx + 1] = (u8)(w >> 16);
                r.tmem[0x800 + idx] = (u8)(w >> 8); r.tmem[0x800 + idx + 1] = (u8)w;
                }
            continue;
            }
        u32 nb = (r.tiSiz == 0) ? ((s1 - s0 + 2) >> 1) : ((s1 - s0 + 1) << (r.tiSiz - 1));
        for (u32 b = 0; b < nb; b++)
            r.tmem[((dst + b) ^ sw) & 0xFFF] = (u8)RdB(ram, src + b);
        }
    }

static void LoadTlut(N64Rdp& r, const u32* ram, u32 hi, u32 lo)
    {
    u32 sl = (hi >> 12) & 0xFFF, tl = hi & 0xFFF, tile = (lo >> 24) & 7, sh = (lo >> 12) & 0xFFF;
    u32 count = ((sh - sl) >> 2) + 1;
    u32 src = r.tiAddr + (((tl >> 2) * (u32)r.tiWidth + (sl >> 2)) << 1);
    u32 dst = (u32)r.tiles[tile].tmem * 8;
    for (u32 n = 0; n < count && n < 256; n++)
        {
        u32 hb = RdB(ram, src + n * 2), lb = RdB(ram, src + n * 2 + 1);
        for (u32 k = 0; k < 4; k++)
            {
            u32 i = (dst + n * 8 + k * 2) & 0xFFE;
            r.tmem[i] = (u8)hb; r.tmem[i + 1] = (u8)lb;
            }
        }
    }

/* ---------------------------- command processor -------------------- */
void N64Core::RdpReset()
    {
    u8* p = (u8*)&iRdp;
    for (u32 i = 0; i < sizeof(N64Rdp); i++) p[i] = 0;
    iRdp.scXL = 320 * 4; iRdp.scYL = 240 * 4;
    iRdp.ciWidth = 320;
    iRdp.resQ = iResQ;
    iRdp.dbg = iGfxDbg;
    iRdp.fast = iGpuMode;
    iRdp.pipeDirty = 1;
    }

static u32 CmdLen(u32 cmd)
    {
    switch (cmd)
        {
        case 0x08: return 4;  case 0x09: return 6;  case 0x0A: return 12; case 0x0B: return 14;
        case 0x0C: return 12; case 0x0D: return 14; case 0x0E: return 20; case 0x0F: return 22;
        case 0x24: case 0x25: return 2;
        }
    return 1;
    }

u32 N64Core::RdpRun(u32 aStart, u32 aEnd, bool aDmem)
    {
    u32 cur = aStart;
    u32 q[48];
    N64Rdp& r = iRdp;
    while (cur + 8 <= aEnd)
        {
        u32 hi = aDmem ? iSp[(cur & 0xFFF) >> 2] : ((cur < N64_RDRAM_SIZE) ? iRam[cur >> 2] : 0);
        u32 cmd = (hi >> 24) & 0x3F;
        u32 n = CmdLen(cmd);
        if (cur + n * 8 > aEnd) break;
        for (u32 k = 0; k < n * 2; k++)
            {
            u32 a = cur + k * 4;
            q[k] = aDmem ? iSp[(a & 0xFFF) >> 2] : ((a < N64_RDRAM_SIZE) ? iRam[a >> 2] : 0);
            }
        cur += n * 8;
        iStats.rdpCmds++;
        u32 h = q[0], l = q[1];

        switch (cmd)
            {
            case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
                iStats.rdpTris++;
                Triangle(r, iRam, q, cmd);
                break;
            case 0x24: case 0x25:
                iStats.rdpRects++;
                TexRect(r, iRam, q);
                break;
            case 0x26: case 0x27: case 0x28: break;                       /* syncs */
            case 0x29: RaiseMi(0x20); break;                              /* SYNC_FULL -> DP interrupt */
            case 0x2A: case 0x2B: case 0x2C: case 0x2E: 
                if (cmd == 0x2E) { r.primZ = (u16)(l >> 16); r.primDz = (u16)l; }
                break;
            case 0x2D:
                r.scXH = (u16)((h >> 12) & 0xFFF); r.scYH = (u16)(h & 0xFFF);
                r.scXL = (u16)((l >> 12) & 0xFFF); r.scYL = (u16)(l & 0xFFF);
                break;
            case 0x2F: r.omHi = h & 0xFFFFFF; r.omLo = l; r.pipeDirty = 1; break;
            case 0x30: LoadTlut(r, iRam, h, l); break;
            case 0x32:
                {
                N64Tile& t = r.tiles[(l >> 24) & 7];
                t.sl = (u16)((h >> 12) & 0xFFF); t.tl = (u16)(h & 0xFFF);
                t.sh = (u16)((l >> 12) & 0xFFF); t.th = (u16)(l & 0xFFF);
                break;
                }
            case 0x33: LoadBlock(r, iRam, h, l); break;
            case 0x34: LoadTile(r, iRam, h, l); break;
            case 0x35:
                {
                N64Tile& t = r.tiles[(l >> 24) & 7];
                t.fmt = (u8)((h >> 21) & 7); t.siz = (u8)((h >> 19) & 3);
                t.line = (u16)((h >> 9) & 0x1FF); t.tmem = (u16)(h & 0x1FF);
                t.pal = (u8)((l >> 20) & 15);
                t.cmT = (u8)((l >> 18) & 3); t.maskT = (u8)((l >> 14) & 15); t.shiftT = (u8)((l >> 10) & 15);
                t.cmS = (u8)((l >> 8) & 3);  t.maskS = (u8)((l >> 4) & 15);  t.shiftS = (u8)(l & 15);
                break;
                }
            case 0x36: FillRect(r, iRam, h, l); break;
            case 0x37: r.fillColor = l; break;
            case 0x38: r.fogColor = l; break;
            case 0x39: r.blendColor = l; break;
            case 0x3A: r.primColor = l; r.primLodFrac = (u8)(h & 0xFF); break;
            case 0x3B: r.envColor = l; break;
            case 0x3C: r.combHi = h & 0xFFFFFF; r.combLo = l; r.pipeDirty = 1; break;
            case 0x3D:
                r.tiFmt = (u8)((h >> 21) & 7); r.tiSiz = (u8)((h >> 19) & 3);
                r.tiWidth = (u16)((h & 0x3FF) + 1); r.tiAddr = l & 0x3FFFFFF;
                break;
            case 0x3E: r.ziAddr = l & 0x3FFFFFF; break;
            case 0x3F:
                r.ciFmt = (u8)((h >> 21) & 7); r.ciSiz = (u8)((h >> 19) & 3);
                r.ciWidth = (u16)((h & 0x3FF) + 1); r.ciAddr = l & 0x3FFFFFF;
                break;
            default: iStats.unkRdp++; break;
            }
        }
    return cur;
    }
