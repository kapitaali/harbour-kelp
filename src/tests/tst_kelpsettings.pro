TARGET = tst_kelpsettings
TEMPLATE = app

QT = core
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle

# The dev-extras bundle switch: a test build carries whatever the environment asks
# for, exactly like the app, so the same test can be run in both modes (the default
# it checks is the build's — see the test's own header).
include(../../src/kelpbuild.pri)

INCLUDEPATH += $$PWD $$PWD/../../src
# No $$PWD/../.. (the project root): the app builds in-source, so a stale
# generated .moc/.cpp there could shadow this build's. See tst_kelpfetch.pro.

SOURCES += tst_kelpsettings.cpp ../../src/kelpsettings.cpp ../../src/kelplog.cpp ../../src/kelpbuild.cpp
HEADERS += ../../src/kelpsettings.h ../../src/kelplog.h ../../src/kelpbuild.h
