#include <eikon.hrh>
#include <avkon.hrh>
#include <aknappui.h>
#include <eikenv.h>
#include <eikstart.h>
#include "N64S60.hrh"
#include "N64S60AppUi.h"
#include "N64S60Container.h"
#include "N64S60Engine.h"

void CN64S60AppUi::ConstructL()
    {
    BaseConstructL(EAknEnableSkin);

    /* landscape = 320x240 = native N64 resolution, 1:1 pixels */
    TRAPD(oerr, SetOrientationL(CAknAppUiBase::EAppUiOrientationLandscape));
    (void)oerr;

    if (StatusPane()) StatusPane()->MakeVisible(EFalse);

    iEngine = CN64Engine::NewL();
    iEngine->ScanRomsL();

    iContainer = new (ELeave) CN64S60Container;
    iContainer->SetMopParent(this);
    iContainer->ConstructL(ClientRect(), iEngine);
    AddToStackL(iContainer);

    iContainer->ShowMenu();          /* start on the ROM list */
    _LIT8(KMenu, "ui: menu shown");
    iEngine->LogState(KMenu);
    iContainer->Start();
    }

CN64S60AppUi::~CN64S60AppUi()
    {
    if (iContainer)
        {
        iContainer->Stop();
        RemoveFromStack(iContainer);
        delete iContainer;
        }
    delete iEngine;
    }

void CN64S60AppUi::ShowMsg(const TDesC& aText)
    {
    CEikonEnv::Static()->InfoMsg(aText);
    }

void CN64S60AppUi::HandleCommandL(TInt aCommand)
    {
    switch (aCommand)
        {
        case EEikCmdExit:
        case EAknSoftkeyExit:
        case EAknCmdExit:
            Exit();
            break;

        case ECmdRomMenu:
            iContainer->ResetInput();
            iContainer->ShowMenu();
            break;

        case ECmdRescan:
            {
            iEngine->ScanRomsL();
            iContainer->ResetInput();
            iContainer->ShowMenu();
            break;
            }

        case ECmdReset:
            iContainer->ResetInput();
            iEngine->Reset();
            iContainer->Refresh();
            break;

        case ECmdPause:
            iContainer->TogglePause();
            break;

        case ECmdCpuMode:
            {
            N64Core& core = iEngine->Core();
            core.SetCpuMode(core.CpuMode() ? 0 : 1);
            _LIT(KC, "CPU: cached blocks (faster)");
            _LIT(KI, "CPU: interpreter (safest)");
            ShowMsg(core.CpuMode() ? KC() : KI());
            break;
            }

        case ECmdGpuMode:
            {
            N64Core& core = iEngine->Core();
            core.SetGpuMode(core.GpuMode() ? 0 : 1);
            _LIT(KC, "GPU: cached (faster)");
            _LIT(KI, "GPU: interpreter (reference)");
            ShowMsg(core.GpuMode() ? KC() : KI());
            break;
            }

        case ECmdTapStart:
            {
            iEngine->Core().PressButton(N64_BTN_START);
            _LIT(KT, "START tapped");
            ShowMsg(KT);
            break;
            }
        case ECmdTapA:
            {
            iEngine->Core().PressButton(N64_BTN_A);
            _LIT(KT, "A tapped");
            ShowMsg(KT);
            break;
            }
        case ECmdTapB:
            {
            iEngine->Core().PressButton(N64_BTN_B);
            _LIT(KT, "B tapped");
            ShowMsg(KT);
            break;
            }

        case ECmdDbgZ:
        case ECmdDbgAlpha:
        case ECmdDbgBilinear:
        case ECmdDbgEdge:
            {
            N64Core& core = iEngine->Core();
            u32 bit = (aCommand == ECmdDbgZ) ? 1 : (aCommand == ECmdDbgAlpha) ? 2 : (aCommand == ECmdDbgBilinear) ? 4 : 8;
            core.SetGfxDebug(core.GfxDebug() ^ bit);
            TBool on = (core.GfxDebug() & bit) != 0;
            _LIT(KZ0, "z-test ON"); _LIT(KZ1, "z-test OFF");
            _LIT(KA0, "alpha-test ON"); _LIT(KA1, "alpha-test OFF");
            _LIT(KB0, "bilinear ON"); _LIT(KB1, "bilinear OFF (point)");
            _LIT(KE0, "edges: wide (no cracks)"); _LIT(KE1, "edges: exact");
            if (bit == 1)      ShowMsg(on ? KZ1() : KZ0());
            else if (bit == 2) ShowMsg(on ? KA1() : KA0());
            else if (bit == 4) ShowMsg(on ? KB1() : KB0());
            else               ShowMsg(on ? KE1() : KE0());
            break;
            }

        case ECmdResolution:
            {
            N64Core& core = iEngine->Core();
            static const TInt kQ[4] = { 256, 230, 192, 128 };
            TInt idx = 0;
            for (TInt i = 0; i < 4; i++) if ((TInt)core.ResScale() == kQ[i]) idx = i;
            idx = (idx + 1) % 4;
            core.SetResScale((u32)kQ[idx]);
            _LIT(K0, "Resolution 1.0 (native)");
            _LIT(K1, "Resolution 0.9");
            _LIT(K2, "Resolution 0.75");
            _LIT(K3, "Resolution 0.5");
            const TDesC* names[4] = { &K0(), &K1(), &K2(), &K3() };
            ShowMsg(*names[idx]);
            break;
            }

        case ECmdFaster:
            iEngine->Core().SetInstrPerFrame(iEngine->Core().InstrPerFrame() * 2);
            break;

        case ECmdSlower:
            iEngine->Core().SetInstrPerFrame(iEngine->Core().InstrPerFrame() / 2);
            break;

        case ECmdToggleInfo:
            iContainer->ToggleInfo();
            break;

        case ECmdHelp:
            {
            _LIT(KHelp, "5=A 1=B 3=Z *=Start 7/9=L/R 2468=C  0=stick/dpad");
            ShowMsg(KHelp);
            break;
            }

        default:
            break;
        }
    }

void CN64S60AppUi::HandleResourceChangeL(TInt aType)
    {
    CAknAppUi::HandleResourceChangeL(aType);
    if (iContainer)
        {
        TRect r = ClientRect();
        if (r != iContainer->Rect()) iContainer->SetRect(r);
        }
    }

void CN64S60AppUi::HandleForegroundEventL(TBool aForeground)
    {
    CAknAppUi::HandleForegroundEventL(aForeground);
    if (!iContainer) return;
    if (!aForeground)
        {
        iContainer->ResetInput();
        iContainer->Stop();
        }
    else if (!iContainer->Paused())
        {
        iContainer->Start();
        }
    }
