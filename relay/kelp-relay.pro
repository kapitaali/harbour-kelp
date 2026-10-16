# kelp-relay — the server side of C3 (wire spec §12.1). Built separately from the
# app: it is not shipped in the app RPM.

TARGET = kelp-relay
TEMPLATE = app

QT = core network
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle

INCLUDEPATH += $$PWD $$PWD/../src

include(../libkelp/libkelp.pri)

SOURCES += \
    main.cpp \
    relay.cpp \
    ../src/kelptls.cpp

HEADERS += \
    relay.h \
    ../src/kelptls.h

LIBS += -lssl -lcrypto
