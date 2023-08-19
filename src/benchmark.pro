TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG += qt

LIBS += -L/usr/local/lib/x86_64-linux-gnu -lwayland-client -lrt -lgbm -ldrm

INCLUDEPATH += /usr/include/drm

SOURCES += \
        linux-dmabuf-unstable-v1.c \
        main.cpp \
        shm.cpp \
        wl_drm.c \
        xdg-shell-protocol.c

HEADERS += \
    linux-dmabuf-unstable-v1.h \
    shm.h \
    wl_drm.h \
    xdg-shell-client-protocol.h
