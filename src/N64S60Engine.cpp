#include "N64S60Engine.h"

CN64Engine::CN64Engine()
    : iCore(NULL), iRam(NULL), iFileOpen(EFalse), iLoaded(EFalse), iRoms(NULL), iCurrent(-1)
    {
    }

CN64Engine* CN64Engine::NewL()
    {
    CN64Engine* self = new (ELeave) CN64Engine;
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop(self);
    return self;
    }

void CN64Engine::ConstructL()
    {
    iRam = (u32*)User::AllocL(N64_RDRAM_BYTES);
    iCore = new (ELeave) N64Core;
    iCore->Init(iRam);
    User::LeaveIfError(iFs.Connect());
    iRoms = new (ELeave) CDesCArrayFlat(8);

    /* breadcrumb file: if the emulator dies, the last line tells where */
    _LIT(KLogDir,  "C:\\Data\\");
    _LIT(KLogFile, "C:\\Data\\n64log.txt");
    iFs.MkDirAll(KLogDir);
    if (iLog.Replace(iFs, KLogFile, EFileWrite) == KErrNone) iLogOpen = ETrue;
    _LIT8(KReady, "engine: ready");
    LogState(KReady);
    }

void CN64Engine::Trace(const char* aMsg)
    {
    LogState(TPtrC8((const TUint8*)aMsg));
    }

void CN64Engine::LogState(const TDesC8& aText)
    {
    if (!iLogOpen) return;
    TBuf8<256> b;
    b.Copy(aText.Left(250));
    b.AppendFill(' ', 255 - b.Length());
    b.Append('\n');
    iLog.Write(0, b);
    iLog.Flush();
    }

CN64Engine::~CN64Engine()
    {
    CloseRom();
    if (iLogOpen) iLog.Close();
    iFs.Close();
    delete iRoms;
    delete iCore;
    User::Free(iRam);
    }

void CN64Engine::CloseRom()
    {
    if (iFileOpen)
        {
        iFile.Close();
        iFileOpen = EFalse;
        }
    iLoaded = EFalse;
    }

/* ---- ROM discovery ------------------------------------------------ */

TBool CN64Engine::IsRomName(const TDesC& aName)
    {
    TInt pos = aName.LocateReverse('.');
    if (pos == KErrNotFound) return EFalse;
    TPtrC ext = aName.Mid(pos);
    return ext.CompareF(_L(".z64")) == 0 ||
           ext.CompareF(_L(".n64")) == 0 ||
           ext.CompareF(_L(".v64")) == 0 ||
           ext.CompareF(_L(".rom")) == 0;
    }

void CN64Engine::ScanDirL(const TDesC& aDir)
    {
    CDir* dir = NULL;
    TInt err = iFs.GetDir(aDir, KEntryAttNormal, ESortByName, dir);
    if (err != KErrNone)
        {
        delete dir;
        return;
        }
    CleanupStack::PushL(dir);
    for (TInt i = 0; i < dir->Count(); i++)
        {
        const TEntry& e = (*dir)[i];
        if (IsRomName(e.iName) && aDir.Length() + e.iName.Length() <= KMaxFileName)
            {
            TFileName full(aDir);
            full.Append(e.iName);
            iRoms->AppendL(full);
            }
        }
    CleanupStack::PopAndDestroy(dir);
    }

void CN64Engine::ScanRomsL()
    {
    iRoms->Reset();
    _LIT(KD0, "E:\\N64\\");
    _LIT(KD1, "F:\\N64\\");
    _LIT(KD2, "C:\\Data\\N64\\");
    _LIT(KD3, "E:\\Others\\N64\\");
    _LIT(KD4, "E:\\Roms\\");
    _LIT(KD5, "E:\\");
    _LIT(KD6, "C:\\Data\\");
    ScanDirL(KD0);
    ScanDirL(KD1);
    ScanDirL(KD2);
    ScanDirL(KD3);
    ScanDirL(KD4);
    ScanDirL(KD5);
    ScanDirL(KD6);
    }

TInt CN64Engine::RomCount() const
    {
    return iRoms->MdcaCount();
    }

TPtrC CN64Engine::RomPath(TInt aIndex) const
    {
    return iRoms->MdcaPoint(aIndex);
    }

/* ---- load / run --------------------------------------------------- */

TInt CN64Engine::LoadRom(TInt aIndex)
    {
    CloseRom();
    if (aIndex < 0 || aIndex >= RomCount()) return KErrNotFound;
    _LIT8(KOpen, "engine: opening rom file");
    LogState(KOpen);

    TInt err = iFile.Open(iFs, iRoms->MdcaPoint(aIndex), EFileRead | EFileShareAny);
    if (err != KErrNone) return err;
    iFileOpen = ETrue;

    TInt size = 0;
    err = iFile.Size(size);
    if (err != KErrNone)
        {
        CloseRom();
        return err;
        }

    _LIT8(KCore, "engine: calling core LoadRom");
    LogState(KCore);
    if (iCore->LoadRom(this, (u32)size) != 0)
        {
        CloseRom();
        return KErrNotSupported;
        }
    iCurrent = aIndex;
    iLoaded = ETrue;
    return KErrNone;
    }

void CN64Engine::Reset()
    {
    if (iLoaded) iCore->Reset();
    }

void CN64Engine::RunFrame()
    {
    if (iLoaded) iCore->RunFrame();
    }

TBool CN64Engine::RunSlice(TInt aInstr)
    {
    return iLoaded ? (TBool)iCore->RunSlice((u32)aInstr) : EFalse;
    }

s32 CN64Engine::ReadRom(u32 aOffset, u8* aDst, u32 aLen)
    {
    if (!iFileOpen) return 0;
    TPtr8 ptr(aDst, 0, (TInt)aLen);
    TInt err = iFile.Read((TInt)aOffset, ptr, (TInt)aLen);
    if (err != KErrNone) return 0;
    return (s32)ptr.Length();
    }
