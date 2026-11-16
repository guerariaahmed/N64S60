#ifndef N64S60DOCUMENT_H
#define N64S60DOCUMENT_H

#include <akndoc.h>

class CN64S60Document : public CAknDocument
    {
public:
    static CN64S60Document* NewL(CEikApplication& aApp);
private:
    CN64S60Document(CEikApplication& aApp) : CAknDocument(aApp) {}
    CEikAppUi* CreateAppUiL();
    };

#endif
