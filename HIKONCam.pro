QT       += core gui serialport concurrent

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

QMAKE_PROJECT_DEPTH = 0

# ==================== 品牌资源 ====================
# branding/default：默认品牌，随仓库公开；branding/local：本地自定义品牌，不纳入版本库。
# 存在 branding/local/branding.ini 时自动使用本地品牌，否则使用默认品牌；两者资源前缀均为 :/branding。
BRANDING_DIR = $$PWD/branding/default
exists($$PWD/branding/local/branding.ini): BRANDING_DIR = $$PWD/branding/local
message("Branding: $$BRANDING_DIR")
RC_ICONS = $$BRANDING_DIR/app.ico

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    MvCamera.cpp \
    arrayqueue.cpp \
    logindialog.cpp \
    main.cpp \
    mainwindow.cpp \
    processthread.cpp \
    PLCManager.cpp \
    HikLightController.cpp \
    ConveyorController.cpp \
    branding.cpp

HEADERS += \
    MvCamera.h \
    arrayqueue.h \
    logindialog.h \
    mainwindow.h \
    processthread.h \
    PLCManager.h \
    HikLightController.h \
    ConveyorController.h \
    branding.h

FORMS += \
    logindialog.ui \
    mainwindow.ui

# 海康 MVS SDK（自行从 MVS 安装目录复制到 includes/ 与 lib/）
INCLUDEPATH += $$PWD/includes/
LIBS += -L$$PWD/lib/ -lMvCameraControl

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# ==================== Snap7 PLC通信库配置 ====================
# Windows 系统库依赖
win32:LIBS += -lws2_32 -lwinmm

# Snap7库头文件路径（相对路径）
INCLUDEPATH += $$PWD/snap7-full-1.4.2/release/Wrappers/c-cpp/ \
               $$PWD/snap7-full-1.4.2/src/lib/ \
               $$PWD/snap7-full-1.4.2/src/core/ \
               $$PWD/snap7-full-1.4.2/src/sys/

# Snap7库源文件（相对路径）
SOURCES += $$PWD/snap7-full-1.4.2/release/Wrappers/c-cpp/snap7.cpp \
    $$PWD/snap7-full-1.4.2/src/core/s7_client.cpp \
    $$PWD/snap7-full-1.4.2/src/core/s7_isotcp.cpp \
    $$PWD/snap7-full-1.4.2/src/core/s7_micro_client.cpp \
    $$PWD/snap7-full-1.4.2/src/core/s7_partner.cpp \
    $$PWD/snap7-full-1.4.2/src/core/s7_peer.cpp \
    $$PWD/snap7-full-1.4.2/src/core/s7_server.cpp \
    $$PWD/snap7-full-1.4.2/src/core/s7_text.cpp \
    $$PWD/snap7-full-1.4.2/src/lib/snap7_libmain.cpp \
    $$PWD/snap7-full-1.4.2/src/sys/snap_msgsock.cpp \
    $$PWD/snap7-full-1.4.2/src/sys/snap_sysutils.cpp \
    $$PWD/snap7-full-1.4.2/src/sys/snap_tcpsrvr.cpp \
    $$PWD/snap7-full-1.4.2/src/sys/snap_threads.cpp

# Snap7库头文件（相对路径）
HEADERS  += $$PWD/snap7-full-1.4.2/release/Wrappers/c-cpp/snap7.h \
    $$PWD/snap7-full-1.4.2/src/core/s7_client.h \
    $$PWD/snap7-full-1.4.2/src/core/s7_firmware.h \
    $$PWD/snap7-full-1.4.2/src/core/s7_isotcp.h \
    $$PWD/snap7-full-1.4.2/src/core/s7_micro_client.h \
    $$PWD/snap7-full-1.4.2/src/core/s7_partner.h \
    $$PWD/snap7-full-1.4.2/src/core/s7_peer.h \
    $$PWD/snap7-full-1.4.2/src/core/s7_server.h \
    $$PWD/snap7-full-1.4.2/src/core/s7_text.h \
    $$PWD/snap7-full-1.4.2/src/core/s7_types.h \
    $$PWD/snap7-full-1.4.2/src/lib/snap7_libmain.h \
    $$PWD/snap7-full-1.4.2/src/sys/snap_msgsock.h \
    $$PWD/snap7-full-1.4.2/src/sys/snap_platform.h \
    $$PWD/snap7-full-1.4.2/src/sys/snap_sysutils.h \
    $$PWD/snap7-full-1.4.2/src/sys/snap_tcpsrvr.h \
    $$PWD/snap7-full-1.4.2/src/sys/snap_threads.h \
    $$PWD/snap7-full-1.4.2/src/sys/sol_threads.h \
    $$PWD/snap7-full-1.4.2/src/sys/unix_threads.h \
    $$PWD/snap7-full-1.4.2/src/sys/win_threads.h

# ==================== TensorRT YOLO 库配置 ====================
# 使用相对路径
INCLUDEPATH += $$PWD/TRTYOLO/include
LIBS += -L$$PWD/TRTYOLO/lib/ -lcustom_plugins -ltrtyolo

RESOURCES += \
    src.qrc \
    $$BRANDING_DIR/branding.qrc
