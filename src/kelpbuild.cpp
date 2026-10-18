#include "kelpbuild.h"

#include <QByteArray>
#include <QString>

#include "kelpsettings.h"

namespace KelpBuild {

bool devExtras()
{
#ifdef KELP_DEV_BUILD
    return true;
#else
    return false;
#endif
}

bool linksInLog(const KelpSettings *settings)
{
    if (!devExtras())
        return false;
    return (settings && settings->includeLinksInLog())
           || !qgetenv("KELP_DEBUG_LINK").isEmpty();
}

} // namespace KelpBuild
