#ifndef N64S60CONTAINER_H
#define N64S60CONTAINER_H

#include <coecntrl.h>
#include <fbs.h>
#include "N64S60Engine.h"

/* Full-screen view: runs the emulator from a timer, blits the frame,
   translates key events into N64 controller input. */
class CN64S60Container : public CCoeControl
    {
public:
    void ConstructL(const TRect& aRect, CN64Engine* aEngine);
    ~CN64S60Container();

    void Start();
    void Stop();
    void TogglePause();
    TBool Paused() const { return iPaused; }
    void ToggleInfo() { iShowInfo = !iShowInfo; DrawDeferred(); }
    void ResetInput();
    void RequestLoad(TInt aIndex);   /* load ROM from the timer (UI stays responsive) */
    void ShowMenu();                 /* back to the ROM selection list */
    void Refresh() { iHasFrame = EFalse; DrawDeferred(); }

private: // from CCoeControl
    void SizeChanged();
    void Draw(const TRect& aRect) const;
    TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);

private:
    void CreateBitmapL();
    void StartGame(TInt aIndex);
    TKeyResponse MenuKey(const TKeyEvent& aKey, TEventCode aType);
    void DrawMenu(CWindowGc& aGc, const CFont* aFont) const;
    TInt MenuRows() const;
    void ClampMenu();
    static TInt Tick(TAny* aThis);
    void DoTick();
    TBool MapKey(TInt aScanCode, TBool aDown);
    void DrawLine(CWindowGc& aGc, const CFont* aFont, TInt aLine, const TDesC& aText) const;

private:
    CN64Engine*  iEngine;
    CPeriodic*   iTimer;
    CFbsBitmap*  iBitmap;
    TBool        iHasFrame;
    TBool        iPaused;
    TBool        iShowInfo;
    TBool        iDpadMode;     /* arrows act as digital D-pad instead of analog stick */
    TUint        iBtnMask;      /* N64 button bits held */
    TBool        iUp, iDown, iLeft, iRight;
    TInt         iMode;         /* 0 = ROM menu, 1 = game */
    TInt         iSel;
    TInt         iTop;
    TInt         iPendingRom;
    TInt         iLoadPhase;
    TBuf<64>     iMsg;
    TInt         iFrames;
    TInt         iFps;
    TTime        iFpsStart;
    };

#endif
