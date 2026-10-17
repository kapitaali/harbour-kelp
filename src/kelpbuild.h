/*
 * The "dev extras" feature bundle.
 *
 * Some conveniences are for building and testing rather than for shipping, and
 * the bundle is where they live together so that one switch turns all of them on
 * or off and the next one cannot quietly ship by being forgotten.
 *
 * Today the bundle holds one: writing each share's kelp:// link — and so that
 * bundle's key — into the log by default (KelpSettings::includeLinksInLog), so a
 * device with no usable clipboard can be handed a link. A release build must not
 * do that, and must not offer the switch that does, so here it is.
 *
 * The switch is a build-time value, and off is the answer when nobody said
 * otherwise: KELP_DEV_EXTRAS=1 at build time (the RPM spec passes it while
 * developing — see README) defines KELP_DEV_BUILD; a bare qmake is a release.
 *
 * C++ asks KelpBuild::devExtras(). QML asks the `kelpFeatures` context property,
 * an object of flags (kelpFeatures.devExtras), so a feature added here is
 * reachable from QML without another context property.
 */

#ifndef KELPBUILD_H
#define KELPBUILD_H

namespace KelpBuild {

// Whether this build carries the dev/testing extras.
bool devExtras();

} // namespace KelpBuild

#endif // KELPBUILD_H
