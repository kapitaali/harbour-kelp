TARGET = relaytest
TEMPLATE = app

QT = core network
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle

INCLUDEPATH += $$PWD $$PWD/../.. $$PWD/../../src
include(../../libkelp/libkelp.pri)

SOURCES += relaytest.cpp ../../src/kelptls.cpp
HEADERS += ../../src/kelptls.h
LIBS += -lssl -lcrypto
