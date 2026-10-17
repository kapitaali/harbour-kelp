# The "dev extras" feature bundle switch (src/kelpbuild.h), in one file so the app
# and the tests that link it agree on which build they are.
#
# KELP_DEV_EXTRAS=1 in the environment at qmake time defines KELP_DEV_BUILD and the
# bundle is on; anything else — including nothing at all — is a release build. That
# way the safe answer is what you get when nobody said otherwise, and nothing can
# ship by a forgotten switch. The RPM spec passes 1 while developing
# (rpm/harbour-kelp.spec); see README, "Development builds".
KELP_DEV_EXTRAS = $$getenv(KELP_DEV_EXTRAS)
isEmpty(KELP_DEV_EXTRAS): KELP_DEV_EXTRAS = 0
!equals(KELP_DEV_EXTRAS, 0) {
    DEFINES += KELP_DEV_BUILD
    message("Kelp: dev extras ON (KELP_DEV_EXTRAS=$$KELP_DEV_EXTRAS)")
} else {
    message("Kelp: dev extras off — release build")
}
