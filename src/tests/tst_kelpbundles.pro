TARGET = tst_kelpbundles
TEMPLATE = app

# The publisher reaches the LAN (KelpServer) and, for a relay, the tunnel; the
# tunnel's transport is OpenSSL 3 (kelptls), hence -lssl as well.
QT = core network
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle

INCLUDEPATH += $$PWD $$PWD/../../src
# No $$PWD/../.. (the project root): the app builds in-source, so a stale
# generated .moc/.cpp there could shadow this build's. See tst_kelpfetch.pro.

include(../../libkelp/libkelp.pri)

LIBS += -lssl

SOURCES += tst_kelpbundles.cpp \
    ../../src/kelpbundles.cpp \
    ../../src/kelpfetch.cpp \
    ../../src/kelplog.cpp \
    ../../src/kelpserver.cpp \
    ../../src/kelpsettings.cpp \
    ../../src/kelpstore.cpp \
    ../../src/kelptls.cpp \
    ../../src/kelptunnel.cpp

HEADERS += \
    ../../src/kelpbundles.h \
    ../../src/kelpfetch.h \
    ../../src/kelplog.h \
    ../../src/kelpserver.h \
    ../../src/kelpsettings.h \
    ../../src/kelpstore.h \
    ../../src/kelptls.h \
    ../../src/kelptunnel.h
