# N64S60 – N64 emulator for Nokia S60 3rd Edition (S60v3)

Native Symbian C++ app (EXE, no Open C needed, capabilities: none).
Targets S60 3rd Edition (N73, N95, E-series, 5xxx, ...), Symbian 9.1–9.3.

## READ THIS FIRST – status and honest limits

* The **emulator core is tested on a PC** (`tools/`, ~50 checks, all pass): CPU, FPU,
  interrupts, DMA, controller, **RSP microcode CPU incl. vector unit, RDP rasteriser**
  (fill, shaded triangle, TMEM texture load, texture rectangle).
* The **Symbian front-end (UI, timer, keys, file access) is only compile-checked by
  eye** – no Symbian SDK was available while writing it. The app itself did start in
  the S60 3rd FP1 emulator (WINSCW) and showed the start screen.
* **I could not test with a real commercial ROM (e.g. GoldenEye).** The RSP/RDP code
  was written from knowledge of the hardware and verified only with the small test ROM.
  A real game exercises thousands of details; expect some bugs (wrong colours, missing
  geometry, hangs). The on-screen diagnostics (below) tell you where it stops.

### What is emulated
VR4300 CPU (64-bit ISA, exceptions, interrupts, Count/Compare, **full TLB** – GoldenEye
runs its game code from TLB-mapped addresses like 0x7F000000), FPU in both FR=0
(even/odd register pairs, what libultra uses) and FR=1, 4 MB RDRAM, PI/SI/VI/MI/AI
registers, PI DMA from the ROM file, controller 1 + 4 Kbit EEPROM on the PIF,
**RSP** (scalar + complete vector unit, vector load/store, SP DMA, IMEM/DMEM),
**RDP** (all commands, 1/2-cycle/copy/fill, combiner, blender, z-buffer, bilinear,
clamp/mirror/mask/shift, 16/32-bit colour images), HLE boot (no PIF/IPL3 ROM).

### Not emulated / approximate
Audio (no sound), Controller Pak, Rumble, anti-aliasing/coverage, mip-map LOD,
fog, dithering, chroma key, YUV, 8-bit colour images, RSP ops VMULQ/VMACQ/VRNDx and
LFV/SFV (no-ops). VCH/VCL/VCR (used by clipping) are written from memory.

### CPU type and resolution (Options menu)
* **CPU: interpreter / cached.** *Cached* pre-fetches code into blocks and runs them without
  re-fetching, re-translating addresses or checking interrupts per instruction (about 1.9x
  faster than the interpreter on a PC benchmark; results are bit-identical on the test ROMs).
  It is **not** a real ARM recompiler - generating and running ARM machine code needs
  executable memory and a code generator that could not be tested here.
  Switch back to *interpreter* if a game misbehaves in cached mode.
* **Resolution 1.0 / 0.9 / 0.75 / 0.5.** The RDP draws a reduced pixel grid inside triangles
  and replicates pixels (colour + z) for the rest, saving roughly the square of the factor in
  triangle work. Rectangles/sprites stay full resolution.
* The emulator now runs in ~30 ms bursts back to back (before: one tiny slice per 40 ms),
  which on its own gave a big speed-up.

### GPU mode (RSP + RDP) and glitch switches (Options menu)
* **GPU: interpreter/cached.** *Cached* pre-decodes the RSP's instruction memory and uses lean
  in-place vector ops; the RDP skips shade/texture work the combiner never reads and interpolates
  incrementally. `tools/fuzz_gpu.cpp` runs thousands of random microcode programs and random
  RDP command streams through both modes and requires **bit-identical** results (it passes).
  Measured on a PC: RSP about 1.4x faster; the RDP gain on a simple shaded triangle test was ~0
  (the phone's slower 64-bit arithmetic should benefit more, but I cannot measure that here).
* **Debug: z-test / alpha-test / bilinear / edges.** Turn single stages off to find which one
  causes a visual glitch. *edges wide* (default) lets each triangle touch every pixel it
  partly covers, which removes hairline cracks between polygons.
* **Tap START / A / B.** Press from the menu; the press is held until the game has read the pad.
* The pad is **latched**: a key press is kept until the game has polled the controller once, so
  quick taps are not missed even when the game polls only every few seconds.
  The overlay line `pad xxxx polls n dbg x` shows buttons held, how many times the game has read
  the controller (if `polls` never increases, the game is not reading input yet), and the active
  debug switches (1 = z-test off, 2 = alpha-test off, 4 = bilinear off, 8 = exact edges).

### Fixes learned from real games (Mario 64 / GoldenEye / Star Fox)
* **PIF auto-polling.** After the game writes the controller command block once, the real PIF keeps
  polling the pads by itself; libultra then only re-reads the result buffer. The emulator now
  refreshes the controller data on every PIF read (`tools/test_pif.cpp`). Before this only games that
  rewrite the command every time (Star Fox) saw any input; the overlay showed `polls 1` forever.
* **RSP reciprocal / inverse-sqrt tables were wrong** (only entry 0 was right), which put 3D vertices in
  the wrong place (shredded polygons, smeared textures). Tables are now derived from the maths and
  checked to <0.2% error (`tools/test_rsp_math.cpp`).
* **GPU cached mode** now re-decodes the RSP instruction cache whenever microcode overlays are DMA'd
  into IMEM *while the RSP is running* (the fuzz test covers this case).

### Speed
Interpreted RSP+RDP on a ~330 MHz ARM11 will be *very* slow: think seconds per frame.
The emulator runs a fixed number of CPU instructions per video frame (default 300 000,
Options → more/fewer instr). The CPU timer is scaled so games still see 60 Hz; they
just get fewer instructions, so they run in slow motion. Be patient: GoldenEye needs
millions of instructions just to boot (minutes on the phone).

### Keys / testing input
The overlay's last line shows `pad xxxx` = the N64 buttons currently held (A=8000 B=4000 Z=2000
Start=1000, L=0020, R=0010, C-up/down/left/right=0008/0004/0002/0001). Press `*` (or `E` on a PC
keyboard in the emulator) and you should see `pad 1000`. Titles like Mario 64 and GoldenEye need
**Start** to leave the title screen. Arrow keys = analog stick (`0` switches them to the D-pad).

### Start-up behaviour
The app opens instantly and shows `Loading ROM...` (green line) before it reads the ROM, then
`Booting game...` until the first video frame finishes. Emulation runs in small slices
(30 000 instructions per timer tick) so the UI and the Options menu stay responsive.
If the screen stays white, the app crashed or hung *before* the first draw – tell me what
the emulator's Console/Output says.

### If the app crashes or the emulator closes
While a game runs, the app overwrites one line in `C:\Data\n64log.txt` every tick (state just
before the slice that was about to run). In the S60 emulator the file is at
`<SDK>\epoc32\winscw\c\Data\n64log.txt`. After a crash open it and send me that line, plus the
panic text if a dialog such as `KERN-EXEC 3` or `E32USER-CBase 46` appeared.

### On-screen diagnostics (Options → Show/hide info, or `#`)
```
GOLDENEYE                    <- ROM name from header
1 fps 300k i/f fr12          <- fps, instructions/frame (thousands), frames run
PC 80123456 RSP 0F8          <- CPU program counter, RSP program counter
rsp 5 x120k  rdp 3k t210 r40 <- RSP runs / instr(k); RDP commands(k), triangles, rects
unk cpu0 rsp0 rdp0 vi14      <- unknown opcodes seen; number of VI register writes
tlb faults 0  exc 3          <- TLB faults taken; non-interrupt exceptions taken
```
* `vi` stays 0 → game has not initialised video yet (still booting).
* `PC` never changes between looks → the game is in a wait loop (tell me the value).
* `rsp 0` → game never started microcode; `rdp t0` → RSP produced no triangles.
* any `unk` counter > 0 → an instruction I did not implement was executed.
If it does not work, send me those 5 lines for your ROM and I can pinpoint the problem.

### ROM requirements
`.z64` / `.n64` / `.v64` (all byte orders detected). ROM is streamed from the file, so
large ROMs are fine. Games that need the 8 MB Expansion Pak will not work (4 MB RDRAM).

## What you need

* S60 3rd Edition SDK (e.g. "S60 3rd Edition SDK for Symbian OS, Feature Pack 1/2")
  + GCCE (CodeSourcery arm-none-symbianelf) – or Carbide.c++ with that SDK.
* Strawberry Perl (SDK requirement), Java if the SDK needs it.
* A self-sign certificate (`makekeys`) or Symbian Signed "Open Signed Online"
  (needs your phone IMEI). Capability set is NONE, so self-signing works.

## Build – command line

1. Put the folder `N64S60` on the **same drive** as your SDK's `\epoc32`
   (use `subst X: C:\path` for the SDK if needed).
2. `devices -setdefault @S60_3rd_FP2:com.nokia.s60` (or your SDK name).
3. `cd N64S60\group` and run `build.bat`
   (= `bldmake bldfiles`, `abld build gcce urel`, `makesis N64S60.pkg`).
4. Sign:  `signsis N64S60_unsigned.sis N64S60.sis mycert.cer mykey.key`
5. Install `N64S60.sis` on the phone.

## Build – Carbide.c++

File > Import > Symbian OS Bld.inf file → `group\bld.inf`, select
"Phone Release (GCCE)" for your S60 3rd SDK, Build, then "Create SIS".

## Using it

* Copy ROMs (`.z64`, `.n64`, `.v64`) to `E:\N64\` (memory card) or
  `C:\Data\N64\`. Also scanned: `F:\N64\`, `E:\Others\N64\`, `E:\Roms\`, root of `E:\`, `C:\Data\`.
* Start the app → the **ROM menu** appears first. Up/Down (or 2/8) moves, Select/5 starts the
  highlighted ROM (it then shows *Loading ROM...* / *Booting game...*). Options → *ROM menu*
  returns to the list at any time; *Rescan ROMs* refreshes it.
* The app forces landscape so the 320x240 N64 picture maps 1:1.
* Keys:  Joystick/arrows = analog stick (or D-pad, toggle with `0`) ·
  Select or `5` = A · `1` = B · `3` = Z · `*` = Start · `7`/`9` = L/R ·
  `2`/`4`/`6`/`8` = C-up/left/right/down · `#` = show/hide info ·
  QWERTY phones: A=A, S=B, D=Z, Q=L, W=R, E=Start.
* Options menu: ROM menu, Rescan, Reset, Pause, instr/frame ×2 or ÷2, info, help.
* A first sanity check: copy `tools/testrom.z64` to `E:\N64\`.
  It fills the whole screen **red** (and runs ~25 FPU/integer self-tests).

## Core self-test (PC, proves the emulator logic)

```
cd tools
python3 make_testrom.py .
g++ -O2 -I../inc test_core.cpp ../src/n64core.cpp ../src/n64cpu.cpp ../src/n64rsp.cpp ../src/n64rdp.cpp -o test_core
./test_core testrom.z64 testrom.v64 testrom.n64     # -> ALL TESTS PASSED
```
`test_pif`, `test_rsp_math` and `fuzz_gpu` are PC tests of the input path, the RSP maths and GPU-cached equivalence.
Run it also with `--cached`, `--gpu` and `--res=192` (0.75). `fuzz_gpu` proves GPU cached == interpreter. `bench_core` (see `tools/make_benchrom.py`) compares
interpreter vs cached speed.
The test ROM checks FPU ops, mult/div, 64-bit shifts, LWL/LWR/SWL/SWR, likely
branches, JAL/JR, interrupts (VI), PI DMA, the controller poll, an RSP microcode run
(vector add/multiply/reciprocal, loads/stores), RDP fill + shaded triangle + texture
load/rectangle, and the framebuffer → RGB565 scaler, for all three ROM byte orders.

## Project layout

```
group/   bld.inf, N64S60.mmp, N64S60.pkg, build.bat
inc/     n64core.h (portable core API), UI headers, N64S60.hrh (UID, commands)
src/     n64core.cpp (memory+devices+boot+video)  n64cpu.cpp (CPU+FPU)
         n64rsp.cpp (RSP + vector unit)  n64rdp.cpp (RDP rasteriser)
         N64S60*.cpp (Symbian app: App, Document, AppUi, Container, Engine)
data/    N64S60.rss (menu), N64S60_reg.rss (app registration)
tools/   PC test harness + test ROM generator
```

## Likely first-build problems / knobs

* UID `0xE64B0001` is in the test range – change in `N64S60.hrh`, `.mmp`,
  `N64S60_reg.rss`, `.pkg` together if it clashes or you get a real UID.
* `.pkg` has absolute paths for `C:\\Symbian\\9.2\\S60_3rd_FP1`; edit them for your SDK.
  Build the **Phone Release (GCCE)** configuration first (WINSCW makes no phone .exe).
* `SetOrientationL(...Landscape)` needs S60 3rd Ed. with orientation support
  (all FP1+ devices). On very old firmware it is ignored (picture is scaled).
* If `EStdKeyHash`/`EStdKeyNkpAsterisk` are undefined in your SDK, replace them
  by the numeric scan codes of `#` and `*` for your device.
* Heap: `EPOCHEAPSIZE 0x100000 0x1000000` (16 MB max). 4 MB RDRAM is allocated at start.
* Core tuning: `SetInstrPerFrame()` in `n64core.cpp`; CIC detection/HLE boot is
  best-effort (`Reset()`); only 6101/6102 ROMs are well covered.

## Ideas for improving it (not done)

* Dynamic recompiler / cached decode (big speed win),
* a tiny HLE RDP "fill/texture-rectangle" path for 2D homebrew,
* Controller Pak / SRAM saves, audio (AI → CMdaAudioOutputStream).
