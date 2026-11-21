# Code shared by the plugin and the tests. It depends on QtCore and QtNetwork
# only, not on libnymea, so the tests run without nymead.

INCLUDEPATH += $$PWD

SOURCES += \
    $$PWD/skodalogging.cpp \
    $$PWD/skodaresponse.cpp \
    $$PWD/skodaapiclient.cpp \
    $$PWD/skodapoller.cpp \
    $$PWD/skodastatemodel.cpp \
    $$PWD/skodasettings.cpp \
    $$PWD/skodathingsettings.cpp

HEADERS += \
    $$PWD/skodalogging.h \
    $$PWD/skodaresponse.h \
    $$PWD/skodaapiclient.h \
    $$PWD/skodapoller.h \
    $$PWD/skodastatemodel.h \
    $$PWD/skodasettings.h \
    $$PWD/skodathingsettings.h
