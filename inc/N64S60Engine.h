#ifndef N64S60ENGINE_H
#define N64S60ENGINE_H

#include <e32base.h>
#include <f32file.h>
#include <badesca.h>
#include "n64core.h"

/* Owns the emulator core, the RDRAM block, the open ROM file and the
   list of ROMs found on the phone/memory card. */
class CN64Engine : public CBase, public MN64RomSource
    {
public:
    static CN64Engine* NewL();
    ~CN64Engine();

    void  ScanRomsL();
    TInt  RomCount() const;
    TInt  CurrentRom() const { return iCurrent; }
    TPtrC RomPath(TInt aIndex) const;     /* by value: MdcaPoint returns a temporary */

    TInt  LoadRom(TInt aIndex);        /* KErrNone or a Symbian error code */
    void  Reset();
    void  RunFrame();
    TBool RunSlice(TInt aInstr);       /* ETrue when a frame completed */
    void  LogState(const TDesC8& aText);   /* overwrite crash-log line, flushed to disk */
    TBool Loaded() const { return iLoaded; }
    N64Core& Core() { return *iCore; }

    /* MN64RomSource */
    s32   ReadRom(u32 aOffset, u8* aDst, u32 aLen);
    void  Trace(const char* aMsg);

private:
    CN64Engine();
    void  ConstructL();
    void  CloseRom();
    void  ScanDirL(const TDesC& aDir);
    static TBool IsRomName(const TDesC& aName);

private:
    N64Core*        iCore;
    u32*            iRam;
    RFs             iFs;
    RFile           iFile;
    RFile           iLog;
    TBool           iLogOpen;
    TBool           iFileOpen;
    TBool           iLoaded;
    CDesCArrayFlat* iRoms;
    TInt            iCurrent;
    };

#endif
