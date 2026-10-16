# libkelp — the Kelp content layer.
#
# Canonical CBOR, AES-256-GCM content objects, manifests and bundle
# build/open. Pure computation: no I/O, no UI, no network. Depends only on
# QtCore and OpenSSL's libcrypto (the same crypto the platform already ships).
#
# Included by the app (when it starts using the library) and by the unit tests.

CONFIG += c++11

INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/kelp_bundle.h \
    $$PWD/kelp_cbor.h \
    $$PWD/kelp_crypto.h \
    $$PWD/kelp_link.h \
    $$PWD/kelp_manifest.h \
    $$PWD/kelp_object.h \
    $$PWD/kelp_store.h \
    $$PWD/kelp_wire.h

SOURCES += \
    $$PWD/kelp_bundle.cpp \
    $$PWD/kelp_cbor.cpp \
    $$PWD/kelp_crypto.cpp \
    $$PWD/kelp_link.cpp \
    $$PWD/kelp_manifest.cpp \
    $$PWD/kelp_object.cpp \
    $$PWD/kelp_store.cpp \
    $$PWD/kelp_wire.cpp

LIBS += -lcrypto
