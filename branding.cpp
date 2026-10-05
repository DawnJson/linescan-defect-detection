#include "branding.h"
#include <QSettings>

const Branding& Branding::instance()
{
    static const Branding branding = [] {
        QSettings ini(":/branding/branding.ini", QSettings::IniFormat);
        Branding b;
        b.windowTitle  = ini.value("App/WindowTitle").toString();
        b.title        = ini.value("App/Title").toString();
        b.tagline      = ini.value("App/Tagline").toString();
        b.organization = ini.value("App/Organization").toString();
        b.account      = ini.value("Login/Account").toString();
        b.password     = ini.value("Login/Password").toString();
        return b;
    }();
    return branding;
}
