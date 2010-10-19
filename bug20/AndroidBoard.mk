LOCAL_PATH := $(call my-dir)

ifeq ($(TARGET_PROVIDES_INIT_RC),true)
file := $(TARGET_ROOT_OUT)/init.rc
$(file): $(LOCAL_PATH)/init.rc | $(ACP)
        $(transform-prebuilt-to-target)
ALL_PREBUILT += $(file)
endif

# Kernel installation - disabled for now. Kernel build is outside
# of Android scope
#
# this is here to use the pre-built kernel
#ifeq ($(TARGET_PREBUILT_KERNEL),)
#	TARGET_PREBUILT_KERNEL := $(LOCAL_PATH)/kernel
#endif
#
#file := $(INSTALLED_KERNEL_TARGET)
#ALL_PREBUILT += $(file)
#$(file): $(TARGET_PREBUILT_KERNEL) | $(ACP)
#	$(transform-prebuilt-to-target)

include $(CLEAR_VARS)
