LOCAL_PATH:= $(call my-dir)
LOCAL_C_INCLUDES := $(LOCAL_PATH)
include $(CLEAR_VARS)
LOCAL_C_INCLUDES := jni/core
LOCAL_EXPORT_C_INCLUDE_DIRS := /Users/granpc/Downloads/base.git-android-4.4.2_r1-cmds-bootanimation/jni/lazy
# LOCAL_LDLIBS += /Users/granpc/Downloads/android-ndk-r9c/platforms/android-19/arch-arm/usr/lib #ugh
# LOCAL_C_INCLUDES += /Users/granpc/Downloads/android-ndk-r9c/platforms/android-19/arch-arm/usr/include #UGH
# LOCAL_CFLAGS += HAVE_PTHREADS
APP_PLATFORM := android-19
APP_ABI := armeabi armeabi-v7a

LOCAL_SRC_FILES:= \
	bootanimation_main.cpp \
	BootAnimation.cpp

LOCAL_CFLAGS += -DGL_GLEXT_PROTOTYPES -DEGL_EGLEXT_PROTOTYPES

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	liblog \
	libandroidfw \
	libutils \
	libbinder \
    libui \
	libskia \
    libEGL \
    libGLESv1_CM \
    libgui

LOCAL_LDLIBS += -lcutils -llog -landroidfw -lutils -lbinder -lui -lskia -lEGL -lGLESv1_CM -lgui

LOCAL_C_INCLUDES := \
	$(call include-path-for, corecg graphics)

LOCAL_MODULE:= bootanimation


include $(BUILD_EXECUTABLE)
