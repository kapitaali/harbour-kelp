#include "kelpbuild.h"

namespace KelpBuild {

bool devExtras()
{
#ifdef KELP_DEV_BUILD
    return true;
#else
    return false;
#endif
}

} // namespace KelpBuild
