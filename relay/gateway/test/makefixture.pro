# makefixture — seals the fixture the gateway's JavaScript tests are checked
# against (design §5.2, C4b-plan.md). Not shipped, and not part of the relay.

TARGET = makefixture
TEMPLATE = app

QT = core
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle

include($$PWD/../../../libkelp/libkelp.pri)

SOURCES += makefixture.cpp
