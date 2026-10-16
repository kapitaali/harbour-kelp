TARGET = tst_kelpqr
TEMPLATE = app

QT = core
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle

INCLUDEPATH += $$PWD $$PWD/../../src
# No $$PWD/../.. (the project root): the app builds in-source, so a stale
# generated .moc/.cpp there could shadow this build's. See tst_kelpfetch.pro.

SOURCES += tst_kelpqr.cpp ../../src/qrcodegen.cpp
HEADERS += ../../src/qrcodegen.hpp
