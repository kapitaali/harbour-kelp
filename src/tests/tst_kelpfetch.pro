TARGET = tst_kelpfetch
TEMPLATE = app

QT = core network
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle

INCLUDEPATH += $$PWD $$PWD/../../src
# Note: no $$PWD/../.. (the project root) here. The app builds in-source, so
# generated files like kelpfetch.moc also sit at the root — and a root
# -I would let the compiler pick up a stale one instead of the fresh file this
# build generates. That shadowing once produced duplicate moc symbols at link.
include(../../libkelp/libkelp.pri)

SOURCES += tst_kelpfetch.cpp ../../src/kelpfetch.cpp ../../src/kelptls.cpp
HEADERS += ../../src/kelpfetch.h ../../src/kelptls.h
LIBS += -lssl -lcrypto
