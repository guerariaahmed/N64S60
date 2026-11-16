#include "N64S60Document.h"
#include "N64S60AppUi.h"

CN64S60Document* CN64S60Document::NewL(CEikApplication& aApp)
    {
    return new (ELeave) CN64S60Document(aApp);
    }

CEikAppUi* CN64S60Document::CreateAppUiL()
    {
    return new (ELeave) CN64S60AppUi;
    }
