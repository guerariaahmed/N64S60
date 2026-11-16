#include <eikenv.h>
#include <e32keys.h>
#include <e32std.h>
#include <w32std.h>
#include "N64S60Container.h"

_LIT(KTxtTitle,   "N64S60 emulator");
_LIT(KTxtNoRom,   "No ROM found.");
_LIT(KTxtCopy1,   "Copy .z64/.n64/.v64 to");
_LIT(KTxtCopy2,   "E:\\N64\\ or C:\\Data\\N64\\");
_LIT(KTxtCopy3,   "then Options > Rescan.");
_LIT(KTxtKeys1,   "5=A 1=B 3=Z *=Start");
_LIT(KTxtKeys2,   "7=L 9=R 2468=C-btns");
_LIT(KTxtKeys3,   "0=stick/dpad #=info");
_LIT(KTxtMenuTitle, "Select ROM  %d/%d");
_LIT(KTxtMenuHint,  "Select/5 = start   Options = menu");
_LIT(KTxtBlank,   "no video signal yet");

const TInt KFrameIntervalUs = 5000;    /* short gap between emulation bursts */
const TInt KBudgetUs = 30000;          /* emulate for this long per timer tick */
const TInt KStickMax = 80;
const TInt KSliceInstr = 30000;       /* emulated instructions per timer tick */

void CN64S60Container::ConstructL(const TRect& aRect, CN64Engine* aEngine)
    {
    iEngine = aEngine;
    iPendingRom = -1;
    iLoadPhase = 0;
    iMode = 0;
    iSel = 0;
    iTop = 0;
    iShowInfo = ETrue;
    iDpadMode = EFalse;
    iPaused = EFalse;
    iHasFrame = EFalse;
    iFrames = 0;
    iFps = 0;
    iFpsStart.UniversalTime();

    CreateWindowL();
    iTimer = CPeriodic::NewL(CActive::EPriorityLow);
    SetRect(aRect);
    ActivateL();
    }

CN64S60Container::~CN64S60Container()
    {
    delete iTimer;
    delete iBitmap;
    }

void CN64S60Container::CreateBitmapL()
    {
    delete iBitmap;
    iBitmap = NULL;
    TSize sz = Size();
    if (sz.iWidth <= 0 || sz.iHeight <= 0) return;
    CFbsBitmap* bmp = new (ELeave) CFbsBitmap;
    CleanupStack::PushL(bmp);
    User::LeaveIfError(bmp->Create(sz, EColor64K));
    CleanupStack::Pop(bmp);
    iBitmap = bmp;
    iHasFrame = EFalse;
    }

void CN64S60Container::SizeChanged()
    {
    TRAP_IGNORE(CreateBitmapL());
    }

void CN64S60Container::Start()
    {
    if (!iTimer->IsActive())
        iTimer->Start(0, KFrameIntervalUs, TCallBack(Tick, this));
    iPaused = EFalse;
    }

void CN64S60Container::Stop()
    {
    if (iTimer) iTimer->Cancel();
    }

void CN64S60Container::TogglePause()
    {
    if (iPaused) Start();
    else { Stop(); iPaused = ETrue; }
    DrawDeferred();
    }

void CN64S60Container::ResetInput()
    {
    iBtnMask = 0;
    iUp = iDown = iLeft = iRight = EFalse;
    }

/* ---- timer / frame ------------------------------------------------ */

TInt CN64S60Container::Tick(TAny* aThis)
    {
    static_cast<CN64S60Container*>(aThis)->DoTick();
    return 1;
    }

/* ---- ROM menu ------------------------------------------------------ */

TInt CN64S60Container::MenuRows() const
    {
    const CFont* font = CEikonEnv::Static()->NormalFont();
    TInt lineH = font->HeightInPixels() + 6;
    TInt rows = (Size().iHeight - 2 * lineH - 6) / lineH;
    return rows < 1 ? 1 : rows;
    }

void CN64S60Container::ClampMenu()
    {
    TInt n = iEngine ? iEngine->RomCount() : 0;
    if (iSel >= n) iSel = n - 1;
    if (iSel < 0) iSel = 0;
    TInt rows = MenuRows();
    if (iSel < iTop) iTop = iSel;
    if (iSel >= iTop + rows) iTop = iSel - rows + 1;
    if (iTop < 0) iTop = 0;
    }

void CN64S60Container::ShowMenu()
    {
    iMode = 0;
    iMsg.Zero();
    iPendingRom = -1;
    iLoadPhase = 0;
    iHasFrame = EFalse;
    TInt n = iEngine ? iEngine->RomCount() : 0;
    iSel = (iEngine && iEngine->CurrentRom() >= 0 && iEngine->CurrentRom() < n) ? iEngine->CurrentRom() : 0;
    iTop = 0;
    ClampMenu();
    DrawDeferred();
    }

void CN64S60Container::StartGame(TInt aIndex)
    {
    _LIT8(KSG, "ui: start game requested");
    iEngine->LogState(KSG);
    iMode = 1;
    iMsg.Zero();
    ResetInput();
    RequestLoad(aIndex);
    Start();
    DrawDeferred();
    }

TKeyResponse CN64S60Container::MenuKey(const TKeyEvent& aKey, TEventCode aType)
    {
    TInt sc = aKey.iScanCode;
    TBool up = (sc == EStdKeyUpArrow || sc == '2');
    TBool down = (sc == EStdKeyDownArrow || sc == '8');
    TBool sel = (sc == EStdKeyDevice3 || sc == EStdKeyEnter || sc == '5');
    if (!up && !down && !sel) return EKeyWasNotConsumed;
    if (aType == EEventKey)                      /* repeats while the key is held */
        {
        TInt n = iEngine ? iEngine->RomCount() : 0;
        if (n > 0)
            {
            if (up)        iSel = (iSel + n - 1) % n;
            else if (down) iSel = (iSel + 1) % n;
            else           { StartGame(iSel); return EKeyWasConsumed; }
            ClampMenu();
            DrawDeferred();
            }
        }
    return EKeyWasConsumed;
    }

static TPtrC FileNameOf(const TDesC& aPath)
    {
    TInt pos = aPath.LocateReverse('\\');
    return (pos >= 0) ? aPath.Mid(pos + 1) : TPtrC(aPath);
    }

void CN64S60Container::DrawMenu(CWindowGc& aGc, const CFont* aFont) const
    {
    TSize sz = Size();
    TInt n = iEngine ? iEngine->RomCount() : 0;
    TInt lineH = aFont->HeightInPixels() + 6;
    TInt base = (lineH - aFont->HeightInPixels()) / 2 + aFont->AscentInPixels();

    if (n == 0)
        {
        aGc.SetPenColor(KRgbYellow);
        DrawLine(aGc, aFont, 0, KTxtTitle);
        aGc.SetPenColor(KRgbWhite);
        DrawLine(aGc, aFont, 2, KTxtNoRom);
        DrawLine(aGc, aFont, 3, KTxtCopy1);
        DrawLine(aGc, aFont, 4, KTxtCopy2);
        DrawLine(aGc, aFont, 5, KTxtCopy3);
        DrawLine(aGc, aFont, 7, KTxtKeys1);
        DrawLine(aGc, aFont, 8, KTxtKeys2);
        DrawLine(aGc, aFont, 9, KTxtKeys3);
        return;
        }

    TBuf<40> head;
    head.Format(KTxtMenuTitle, iSel + 1, n);
    aGc.SetPenColor(KRgbYellow);
    aGc.DrawText(head, TRect(4, 0, sz.iWidth, lineH), base, CGraphicsContext::ELeft);

    TInt rows = MenuRows();
    for (TInt r = 0; r < rows; r++)
        {
        TInt i = iTop + r;
        if (i >= n) break;
        TInt y = lineH + 3 + r * lineH;
        TRect row(0, y, sz.iWidth, y + lineH);
        if (i == iSel)
            {
            aGc.SetPenStyle(CGraphicsContext::ENullPen);
            aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
            aGc.SetBrushColor(TRgb(0, 70, 150));
            aGc.DrawRect(row);
            aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
            aGc.SetPenStyle(CGraphicsContext::ESolidPen);
            aGc.SetPenColor(KRgbWhite);
            }
        else
            {
            aGc.SetPenColor(TRgb(190, 190, 190));
            }
        aGc.DrawText(FileNameOf(iEngine->RomPath(i)), TRect(8, y, sz.iWidth - 4, y + lineH), base, CGraphicsContext::ELeft);
        }
    aGc.SetPenColor(TRgb(140, 140, 140));
    aGc.DrawText(KTxtMenuHint, TRect(4, sz.iHeight - lineH, sz.iWidth, sz.iHeight), base, CGraphicsContext::ELeft);
    }

void CN64S60Container::RequestLoad(TInt aIndex)
    {
    iPendingRom = aIndex;
    iLoadPhase = 0;
    iHasFrame = EFalse;
    }

void CN64S60Container::DoTick()
    {
    User::ResetInactivityTime();      /* keep the backlight on */
    if (!iEngine || iMode == 0) return;

    /* ROM loading is done from the timer in two steps so that the
       "Loading..." text is on screen before the (slow) load starts. */
    if (iPendingRom >= 0)
        {
        if (iLoadPhase == 0)
            {
            _LIT8(KL0, "ui: loading screen drawn");
            iEngine->LogState(KL0);
            _LIT(KLoading, "Loading ROM...");
            iMsg.Copy(KLoading);
            iLoadPhase = 1;
            DrawNow();
            CEikonEnv::Static()->WsSession().Flush();
            return;
            }
        TInt idx = iPendingRom;
        iPendingRom = -1;
        iLoadPhase = 0;
        TInt err = iEngine->LoadRom(idx);
        _LIT8(KL1, "ui: LoadRom returned");
        iEngine->LogState(KL1);
        _LIT(KFail, "Cannot load ROM (bad file?)");
        _LIT(KBoot, "Booting game...");
        if (err == KErrNone) iMsg.Copy(KBoot); else iMsg.Copy(KFail);
        iHasFrame = EFalse;
        iFrames = 0;
        iFps = 0;
        iFpsStart.UniversalTime();
        DrawNow();
        return;
        }

    if (!iEngine->Loaded() || !iBitmap) return;

    N64Core& core = iEngine->Core();

    {   /* breadcrumb for crash diagnosis: state just BEFORE this slice runs */
    const N64Stats& st = core.Stats();
    TBuf8<200> lg;
    _LIT8(KLogFmt, "fr=%d pc=%08x rsp=%03x rspRuns=%d rspK=%d rdpCmd=%d tri=%d rect=%d unk=%d,%d,%d tlb=%d exc=%d vi=%d");
    lg.Format(KLogFmt, (TInt)core.Frames(), core.Pc(), core.RspPc(), (TInt)st.rspRuns, (TInt)(st.rspInstr / 1000),
              (TInt)st.rdpCmds, (TInt)st.rdpTris, (TInt)st.rdpRects, (TInt)st.unkCpu, (TInt)st.unkRsp,
              (TInt)st.unkRdp, (TInt)st.tlbFaults, (TInt)st.excs, (TInt)st.viWrites);
    iEngine->LogState(lg);
    }

    u16 buttons = (u16)iBtnMask;
    s8 sx = 0, sy = 0;
    if (iDpadMode)
        {
        if (iUp)    buttons |= N64_BTN_DUP;
        if (iDown)  buttons |= N64_BTN_DDOWN;
        if (iLeft)  buttons |= N64_BTN_DLEFT;
        if (iRight) buttons |= N64_BTN_DRIGHT;
        }
    else
        {
        sx = (s8)((iRight ? KStickMax : 0) - (iLeft ? KStickMax : 0));
        sy = (s8)((iUp ? KStickMax : 0) - (iDown ? KStickMax : 0));
        }
    core.SetInput(buttons, sx, sy);

    /* run slices back to back for ~30 ms, then let the UI breathe */
    TInt framesDone = 0;
    {
    TTime t0;
    t0.UniversalTime();
    for (;;)
        {
        if (iEngine->RunSlice(KSliceInstr)) framesDone++;
        TTime now;
        now.UniversalTime();
        if (now.MicroSecondsFrom(t0).Int64() >= (TInt64)KBudgetUs) break;
        }
    }

    if (framesDone > 0)                              /* at least one video frame finished */
        {
        TSize sz = iBitmap->SizeInPixels();
        iBitmap->LockHeap();
        u16* data = (u16*)iBitmap->DataAddress();
        TInt pitch = CFbsBitmap::ScanLineLength(sz.iWidth, EColor64K) / 2;
        core.Render565(data, sz.iWidth, sz.iHeight, pitch);
        iBitmap->UnlockHeap();
        iHasFrame = ETrue;
        iMsg.Zero();

        iFrames += framesDone;
        if (iFrames >= 3)
            {
            TTime now;
            now.UniversalTime();
            TInt64 us = now.MicroSecondsFrom(iFpsStart).Int64();
            if (us > 0) iFps = (TInt)((TInt64)iFrames * 10000000 / us);   /* tenths of fps */
            iFrames = 0;
            iFpsStart = now;
            }
        }
    DrawNow();                                       /* also refreshes the diagnostics */
    }

/* ---- drawing ------------------------------------------------------ */

void CN64S60Container::DrawLine(CWindowGc& aGc, const CFont* aFont, TInt aLine, const TDesC& aText) const
    {
    TInt h = aFont->HeightInPixels() + 3;
    aGc.DrawText(aText, TPoint(4, 2 + aFont->AscentInPixels() + aLine * h));
    }

void CN64S60Container::Draw(const TRect& /*aRect*/) const
    {
    CWindowGc& gc = SystemGc();
    TRect r(TPoint(0, 0), Size());

    if (iMode == 1 && iBitmap && iHasFrame && iEngine && iEngine->Loaded())
        {
        gc.BitBlt(r.iTl, iBitmap);
        }
    else
        {
        gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
        gc.SetBrushColor(KRgbBlack);
        gc.Clear(r);
        }

    const CFont* font = CEikonEnv::Static()->NormalFont();
    gc.UseFont(font);
    gc.SetPenColor(KRgbWhite);
    gc.SetBrushStyle(CGraphicsContext::ENullBrush);

    if (iMode == 0)
        {
        DrawMenu(gc, font);
        gc.DiscardFont();
        return;
        }

    if (!iEngine || !iEngine->Loaded())
        {
        DrawLine(gc, font, 0, KTxtTitle);
        if (iEngine && iEngine->RomCount() > 0) { }
        else
        {
        DrawLine(gc, font, 2, KTxtNoRom);
        DrawLine(gc, font, 3, KTxtCopy1);
        DrawLine(gc, font, 4, KTxtCopy2);
        DrawLine(gc, font, 5, KTxtCopy3);
        DrawLine(gc, font, 7, KTxtKeys1);
        DrawLine(gc, font, 8, KTxtKeys2);
        DrawLine(gc, font, 9, KTxtKeys3);
        }
        }
    else if (iShowInfo || iPaused)
        {
        N64Core& core = iEngine->Core();
        const N64Stats& st = core.Stats();
        TBuf<64> nm;
        nm.Copy(TPtrC8((const TUint8*)core.RomName()));
        TBuf<96> info;
        _LIT(KFmt1, "%d.%d fps %dk i/f fr%d%S");
        _LIT(KFmt2, "PC %08x RSP %03x");
        _LIT(KFmt3, "rsp %d x%dk  rdp %dk t%d r%d");
        _LIT(KFmt4, "unk cpu%d rsp%d rdp%d vi%d");
        _LIT(KPausedTxt, " PAUSED");
        _LIT(KNone, " ");
        const TDesC& tail = iPaused ? KPausedTxt() : KNone();
        gc.SetPenColor(KRgbYellow);
        DrawLine(gc, font, 0, nm);
        info.Format(KFmt1, iFps / 10, iFps % 10, (TInt)(core.InstrPerFrame() / 1000), (TInt)core.Frames(), &tail);
        DrawLine(gc, font, 1, info);
        info.Format(KFmt2, core.Pc(), core.RspPc());
        DrawLine(gc, font, 2, info);
        info.Format(KFmt3, (TInt)st.rspRuns, (TInt)(st.rspInstr / 1000), (TInt)(st.rdpCmds / 1000),
                    (TInt)st.rdpTris, (TInt)st.rdpRects);
        DrawLine(gc, font, 3, info);
        info.Format(KFmt4, (TInt)st.unkCpu, (TInt)st.unkRsp, (TInt)st.unkRdp, (TInt)st.viWrites);
        DrawLine(gc, font, 4, info);
        _LIT(KFmt5, "tlb faults %d  exc %d");
        info.Format(KFmt5, (TInt)st.tlbFaults, (TInt)st.excs);
        DrawLine(gc, font, 5, info);
        _LIT(KFmt6, "cpu %S gpu %S res %d%%");
        _LIT(KFmt7, "pad %04x polls %d dbg %x");
        _LIT(KCpuI, "interp");
        _LIT(KCpuC, "cached");
        const TDesC& cn = core.CpuMode() ? KCpuC() : KCpuI();
        const TDesC& gn = core.GpuMode() ? KCpuC() : KCpuI();
        info.Format(KFmt6, &cn, &gn, (TInt)((core.ResScale() * 100) / 256));
        DrawLine(gc, font, 6, info);
        info.Format(KFmt7, (TUint)(iBtnMask & 0xFFFF), (TInt)st.padPolls, (TUint)core.GfxDebug());
        DrawLine(gc, font, 7, info);
        if (!core.VideoOn())
            {
            gc.SetPenColor(KRgbWhite);
            DrawLine(gc, font, 8, KTxtBlank);
            }
        }
    if (iMsg.Length() > 0)
        {
        gc.SetPenColor(KRgbGreen);
        DrawLine(gc, font, 9, iMsg);
        }
    gc.DiscardFont();
    }

/* ---- keys --------------------------------------------------------- */

TBool CN64S60Container::MapKey(TInt aScan, TBool aDown)
    {
    TUint btn = 0;
    if (aScan == '*' || aScan == EStdKeyNkpAsterisk)          /* Start */
        {
        if (aDown) iBtnMask |= N64_BTN_START; else iBtnMask &= ~(TUint)N64_BTN_START;
        return ETrue;
        }
    if (aScan == '#' || aScan == EStdKeyHash)                 /* show/hide info */
        {
        if (aDown) ToggleInfo();
        return ETrue;
        }
    switch (aScan)
        {
        case EStdKeyUpArrow:    iUp = aDown;    return ETrue;
        case EStdKeyDownArrow:  iDown = aDown;  return ETrue;
        case EStdKeyLeftArrow:  iLeft = aDown;  return ETrue;
        case EStdKeyRightArrow: iRight = aDown; return ETrue;

        case EStdKeyDevice3:    /* joystick / D-pad centre */
        case EStdKeyEnter:
        case '5':
        case 'A':               btn = N64_BTN_A; break;
        case '1':
        case 'S':               btn = N64_BTN_B; break;
        case '3':
        case 'D':               btn = N64_BTN_Z; break;
        case '7':
        case 'Q':               btn = N64_BTN_L; break;
        case '9':
        case 'W':               btn = N64_BTN_R; break;
        case '2':               btn = N64_BTN_CUP; break;
        case '8':               btn = N64_BTN_CDOWN; break;
        case '4':               btn = N64_BTN_CLEFT; break;
        case '6':               btn = N64_BTN_CRIGHT; break;
        case 'E':               btn = N64_BTN_START; break;

        case '0':
            if (aDown) { iDpadMode = !iDpadMode; iUp = iDown = iLeft = iRight = EFalse; }
            return ETrue;
        default:
            return EFalse;
        }
    if (aDown) iBtnMask |= btn;
    else       iBtnMask &= ~btn;
    return ETrue;
    }

TKeyResponse CN64S60Container::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
    {
    if (iMode == 0) return MenuKey(aKeyEvent, aType);
    TInt sc = aKeyEvent.iScanCode;
    if (aType == EEventKeyDown)
        return MapKey(sc, ETrue) ? EKeyWasConsumed : EKeyWasNotConsumed;
    if (aType == EEventKeyUp)
        return MapKey(sc, EFalse) ? EKeyWasConsumed : EKeyWasNotConsumed;
    if (aType == EEventKey)
        {
        /* swallow the character event of keys we use, pass the rest on */
        if (sc == '*' || sc == '#' || sc == EStdKeyNkpAsterisk || sc == EStdKeyHash) return EKeyWasConsumed;
        switch (sc)
            {
            case EStdKeyUpArrow: case EStdKeyDownArrow:
            case EStdKeyLeftArrow: case EStdKeyRightArrow:
            case EStdKeyDevice3: case EStdKeyEnter:
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
            default:
                break;
            }
        }
    return EKeyWasNotConsumed;
    }
