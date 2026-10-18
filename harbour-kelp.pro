# harbour-kelp.pro

TARGET = harbour-kelp
CONFIG += sailfishapp

# Build identifier for the About page. Prefers the git hash, falls back to a
# UTC timestamp outside a repo.
BUILD_ID = $$system(git -C $$PWD rev-parse --short HEAD 2>/dev/null)
isEmpty(BUILD_ID): BUILD_ID = $$system(date -u +%Y%m%d-%H%M%S)
DEFINES += BUILD_ID=\\\"$$BUILD_ID\\\"

# Version shown on the About page: the RPM build exports APP_VERSION, manual
# qmake runs may pass it as an argument, anything else is hand-built.
isEmpty(APP_VERSION): APP_VERSION = $$getenv(APP_VERSION)
isEmpty(APP_VERSION): APP_VERSION = dev
DEFINES += APP_VERSION=\\\"$$APP_VERSION\\\"

# The "dev extras" feature bundle (src/kelpbuild.h): conveniences for building and
# testing that must not ship, today logging each share's link in the clear by
# default. The switch lives in a .pri the tests share.
include($$PWD/src/kelpbuild.pri)

# make cannot see -D flag changes, so bump the timestamp of every file whose
# compilation depends on a define (main(), and the dev-extras bundle in
# src/kelpbuild.h) to pick up the current defines on incremental builds.
# Switching build targets needs a clean on top of that (the SDK builds
# in-source): sfdk build-shell make distclean.
_version_touch = $$system(touch $$PWD/src/harbour-kelp.cpp $$PWD/src/kelpbuild.cpp $$PWD/src/kelpsettings.cpp $$PWD/src/kelpbundles.cpp)

# The QML engine, the content layer (libkelp, see libkelp/libkelp.pri) and the
# small helpers around it. Qt5Network is both ends of the LAN-direct path: the
# listener that serves bundles and the client that fetches a link.
QT += core gui qml quick network

# The relay tunnel's transport is TLS 1.3 with ALPN "kelp/1", which Qt 5.6's
# QSslSocket cannot do; src/kelptls.cpp drives OpenSSL 3's libssl for it. This is
# a second OpenSSL copy alongside the one Qt5Network drags in — the same split
# libkelp already lives with (Qt owns sockets and its own TLS is unused; libssl
# and libcrypto 3.x own ours).
LIBS += -lssl -lcrypto

include(libkelp/libkelp.pri)

SOURCES += \
    src/harbour-kelp.cpp \
    src/kelpbundles.cpp \
    src/kelpbuild.cpp \
    src/kelpclient.cpp \
    src/kelpdevicestate.cpp \
    src/kelpfetch.cpp \
    src/kelpfiles.cpp \
    src/kelplog.cpp \
    src/kelppolicy.cpp \
    src/kelpqr.cpp \
    src/kelppreviewserver.cpp \
    src/kelpserver.cpp \
    src/kelpsettings.cpp \
    src/kelpshares.cpp \
    src/kelpstore.cpp \
    src/kelptls.cpp \
    src/kelptunnel.cpp \
    src/qrcodegen.cpp

HEADERS += \
    src/kelpbundles.h \
    src/kelpbuild.h \
    src/kelpclient.h \
    src/kelpdevicestate.h \
    src/kelpfetch.h \
    src/kelpfiles.h \
    src/kelplog.h \
    src/kelppolicy.h \
    src/kelpqr.h \
    src/kelppreviewserver.h \
    src/kelpserver.h \
    src/kelpsettings.h \
    src/kelpshares.h \
    src/kelpstore.h \
    src/kelptls.h \
    src/kelptunnel.h \
    src/qrcodegen.hpp

# sailfishapp.prf installs the whole qml/ tree; this list exists so the IDE
# and qmake know about the files.
QML_FILES = \
    qml/harbour-kelp.qml \
    qml/cover/CoverPage.qml \
    qml/components/Toast.qml \
    qml/pages/BundlesPage.qml \
    qml/pages/BundlePage.qml \
    qml/pages/AddPage.qml \
    qml/pages/OpenLinkPage.qml \
    qml/pages/FolderPickPage.qml \
    qml/pages/SettingsPage.qml \
    qml/pages/AboutPage.qml
# The QR generator (src/qrcodegen.*) is MIT-licensed third-party code; install
# its notice with the app so the terms travel with a binary-only RPM too. The
# spec's %{_datadir}/harbour-kelp glob already picks the file up.
license.files = src/qrcodegen.LICENSE
license.path = /usr/share/harbour-kelp
INSTALLS += license

OTHER_FILES += \
    harbour-kelp.desktop \
    rpm/harbour-kelp.spec \
    $$QML_FILES

# Names only, not paths: sailfishapp.prf expands each entry to
# icons/<size>/<TARGET>.png and installs it into icons/hicolor/<size>/apps.
SAILFISHAPP_ICONS = 86x86 108x108 128x128 172x172 256x256
