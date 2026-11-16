#ifndef N64S60APP_H
#define N64S60APP_H

#include <aknapp.h>

class CN64S60Application : public CAknApplication
    {
private:
    CApaDocument* CreateDocumentL();
    TUid AppDllUid() const;
    };

#endif
