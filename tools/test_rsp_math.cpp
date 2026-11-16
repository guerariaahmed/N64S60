/* Accuracy test for the RSP reciprocal / inverse-sqrt units (vrcp, vrsq). PC only. */
#include <stdio.h>
#include <math.h>
#define private public
#include "n64core.h"
#undef private
#include "../src/n64rsp.cpp"
int main()
    {
    BuildTables();
    double maxr = 0, maxs = 0; int bad = 0;
    for (s32 x = 2; x < 0x40000000; x = (x < 40000) ? x + 1 : x + x / 37 + 1)
        {
        double ex = 2147483648.0 / x;
        if (ex < 4096.0) continue;                 /* tiny results are just integer rounding */
        double got = (double)(u32)RcpCalc(x, false);
        double e = fabs(got - ex) / ex; if (e > maxr) maxr = e;
        double ex2 = 2147483648.0 / sqrt((double)x);
        double got2 = (double)(u32)RcpCalc(x, true);
        if (ex2 > 2147483647.0) ex2 = 2147483647.0;
        double e2 = fabs(got2 - ex2) / ex2; if (e2 > maxs) maxs = e2;
        if (x < 32768 && (u32)RcpCalc(-x, false) != ~(u32)RcpCalc(x, false)) bad++;
        }
    printf("vrcp  max relative error: %.4f%%\nvrsq  max relative error: %.4f%%\nsign (one's complement) mismatches: %d\n", maxr * 100, maxs * 100, bad);
    printf("rcp(1)=%08X rcp(2)=%08X rcp(3)=%08X rcp(0)=%08X rcp(-32768)=%08X\n", (u32)RcpCalc(1,false),(u32)RcpCalc(2,false),(u32)RcpCalc(3,false),(u32)RcpCalc(0,false),(u32)RcpCalc(-32768,false));
    printf("rsq(1)=%08X rsq(4)=%08X rsq(2)=%08X\n", (u32)RcpCalc(1,true),(u32)RcpCalc(4,true),(u32)RcpCalc(2,true));
    int ok = (maxr < 0.004 && maxs < 0.006 && bad == 0);
    printf(ok ? "RSP MATH OK\n" : "RSP MATH FAIL\n");
    return ok ? 0 : 1;
    }
