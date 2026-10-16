TARGET = tst_kelp
TEMPLATE = app

QT = core
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle

INCLUDEPATH += $$PWD/..
include(../libkelp.pri)

SOURCES += tst_kelp.cpp
