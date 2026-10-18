TARGET = tst_kelpclient
TEMPLATE = app

# Receiving a link: the fetch walk (network), the view's memory store, and the
# preview session it is served from (design §5.4, §5.5). No GUI, no browser — a
# stub publisher on 127.0.0.1 stands in for the sender.
QT = core network
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle

# The dev-extras bundle switch (src/kelpbuild.h): a received page-view URL is a
# capability, so whether it may be written to the log is decided by the same
# one-file switch the app uses.
include(../../src/kelpbuild.pri)

INCLUDEPATH += $$PWD $$PWD/../../src
# No $$PWD/../.. (the project root): the app builds in-source, so a stale
# generated .moc/.cpp there could shadow this build's. See tst_kelpfetch.pro.
include(../../libkelp/libkelp.pri)

# kelpbuild.cpp calls into KelpSettings, and KelpSettings logs through KelpLog,
# so the suite links the small app-layer trio as well.
LIBS += -lssl

SOURCES += tst_kelpclient.cpp \
    ../../src/kelpclient.cpp \
    ../../src/kelpbuild.cpp \
    ../../src/kelpfetch.cpp \
    ../../src/kelplog.cpp \
    ../../src/kelppreviewserver.cpp \
    ../../src/kelpsettings.cpp \
    ../../src/kelptls.cpp

HEADERS += \
    ../../src/kelpclient.h \
    ../../src/kelpbuild.h \
    ../../src/kelpfetch.h \
    ../../src/kelplog.h \
    ../../src/kelppreviewserver.h \
    ../../src/kelpsettings.h \
    ../../src/kelptls.h
