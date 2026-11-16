#ifndef N64S60APPUI_H
#define N64S60APPUI_H

#include <aknappui.h>

class CN64S60Container;
class CN64Engine;

class CN64S60AppUi : public CAknAppUi
    {
public:
    void ConstructL();
    ~CN64S60AppUi();

private: // from CEikAppUi / CAknAppUi
    void HandleCommandL(TInt aCommand);
    void HandleResourceChangeL(TInt aType);
    void HandleForegroundEventL(TBool aForeground);

private:
    void ShowMsg(const TDesC& aText);

private:
    CN64S60Container* iContainer;
    CN64Engine*       iEngine;
    };

#endif
