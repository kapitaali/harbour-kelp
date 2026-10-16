TARGET = tst_kelpshares
TEMPLATE = app

QT = core
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle

INCLUDEPATH += $$PWD $$PWD/../../src
# No $$PWD/../.. (the project root): the app builds in-source, so a stale
# generated .moc/.cpp there could shadow this build's. See tst_kelpfetch.pro.

SOURCES += tst_kelpshares.cpp ../../src/kelpshares.cpp ../../src/kelplog.cpp
HEADERS += ../../src/kelpshares.h ../../src/kelplog.h
