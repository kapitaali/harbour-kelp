TARGET = tst_kelpchannels
TEMPLATE = app

QT = core
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle

INCLUDEPATH += $$PWD $$PWD/../../src
# No $$PWD/../.. (the project root): the app builds in-source, so a stale
# generated .moc/.cpp there could shadow this build's. See tst_kelpfetch.pro.

include(../../libkelp/libkelp.pri)

SOURCES += tst_kelpchannels.cpp \
    ../../src/kelpchannels.cpp \
    ../../src/kelplog.cpp \
    ../../src/kelpstore.cpp

HEADERS += \
    ../../src/kelpchannels.h \
    ../../src/kelplog.h \
    ../../src/kelpstore.h
