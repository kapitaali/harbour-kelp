TARGET = tst_kelppreview
TEMPLATE = app

# The loopback preview: a QTcpServer (network) and the content layer. No GUI, no
# WebView — the browser is a separate decision (design §5.1, §12.9).
QT = core network
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle

INCLUDEPATH += $$PWD $$PWD/../../src
# No $$PWD/../.. (the project root): the app builds in-source, so a stale
# generated .moc/.cpp there could shadow this build's. See tst_kelpfetch.pro.
include(../../libkelp/libkelp.pri)

# kelpstore.cpp is here for the on-disk store section: the app hands the preview
# server its real KelpObjectStore, and one section proves that path works.
# kelpfetch.cpp is here because the on-demand section drives a real KelpSource
# subclass; it pulls in KelpTls, and therefore OpenSSL, for the relay half too.
SOURCES += tst_kelppreview.cpp \
    ../../src/kelpfetch.cpp \
    ../../src/kelppreviewserver.cpp \
    ../../src/kelpstore.cpp \
    ../../src/kelptls.cpp

# See tst_kelpclient.pro: KelpTls uses OpenSSL directly.
LIBS += -lssl

HEADERS += \
    ../../src/kelpfetch.h \
    ../../src/kelppreviewserver.h \
    ../../src/kelpstore.h \
    ../../src/kelptls.h
