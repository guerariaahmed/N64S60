#include <eikstart.h>
#include "N64S60App.h"
#include "N64S60Document.h"
#include "N64S60.hrh"

const TUid KUidN64S60App = { KN64S60UidValue };

CApaDocument* CN64S60Application::CreateDocumentL()
    {
    return CN64S60Document::NewL(*this);
    }

TUid CN64S60Application::AppDllUid() const
    {
    return KUidN64S60App;
    }

LOCAL_C CApaApplication* NewApplication()
    {
    return new CN64S60Application;
    }

GLDEF_C TInt E32Main()
    {
    return EikStart::RunApplication(NewApplication);
    }
