LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := libgps

LOCAL_STATIC_LIBRARIES:= \
# include any static library dependencies

LOCAL_SHARED_LIBRARIES := \
	libutils \
	libcutils \
	libdl \
	libc
# include any shared library dependencies

LOCAL_SRC_FILES += LOCAL_SRC_FILES += \
	gps_freerunner.c 

LOCAL_CFLAGS += \
# include any needed compile flags

LOCAL_C_INCLUDES:= \
# include any needed local header files

include $(BUILD_SHARED_LIBRARY)
