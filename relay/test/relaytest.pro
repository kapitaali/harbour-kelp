TARGET = relaytest
TEMPLATE = app

QT = core network
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle

INCLUDEPATH += $$PWD $$PWD/../.. $$PWD/../../src
include(../../libkelp/libkelp.pri)

SOURCES += relaytest.cpp ../../src/kelpfetch.cpp ../../src/kelptls.cpp \
    ../../src/kelptunnel.cpp ../../src/kelpstore.cpp
HEADERS += ../../src/kelpfetch.h ../../src/kelptls.h ../../src/kelptunnel.h \
    ../../src/kelpstore.h
LIBS += -lssl -lcrypto
