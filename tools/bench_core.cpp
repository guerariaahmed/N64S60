#include "n64core.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
class FileSrc : public MN64RomSource { public: FILE* f; s32 ReadRom(u32 o, u8* d, u32 l){ fseek(f,(long)o,SEEK_SET); return (s32)fread(d,1,l,f);} };
int main(int argc, char** argv)
    {
    FileSrc s; s.f = fopen(argv[1], "rb"); fseek(s.f, 0, SEEK_END); long size = ftell(s.f);
    for (int mode = 0; mode < 2; mode++)
        {
        u32* ram = (u32*)malloc(N64_RDRAM_BYTES); N64Core* c = new N64Core; c->Init(ram);
        c->SetCpuMode((u32)mode); c->LoadRom(&s, (u32)size);
        clock_t t0 = clock(); int frames = 60;
        for (int f = 0; f < frames; f++) c->RunFrame();
        double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
        double mips = (double)frames * c->InstrPerFrame() / sec / 1e6;
        printf("%-12s %.1f M instr/s  (loop counter word = %08X)\n", mode ? "cached" : "interpreter", mips, c->PeekRam32(0x2000));
        free(ram); delete c;
        }
    return 0;
    }
