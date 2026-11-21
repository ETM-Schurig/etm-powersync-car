TEMPLATE = app
TARGET = testskoda

QT += testlib network
QT -= gui
CONFIG += testcase c++17 link_pkgconfig
CONFIG -= app_bundle

QMAKE_CXXFLAGS *= -Werror

# libnymea, only for PluginMetadata (validation of integrationpluginskoda.json).
PKGCONFIG += nymea

include(../../skoda/skodacore.pri)

DEFINES += SOURCE_ROOT=\\\"$$clean_path($$PWD/../..)\\\"

SOURCES += \
    fakeskodaserver.cpp \
    testskoda.cpp

HEADERS += \
    fakeskodaserver.h
