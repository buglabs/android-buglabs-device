LOCAL_PATH := $(call my-dir)

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

# Took this build code from generic as described in Google's porting guide but it doesn't seem
# to cause any change in target.  Leaving in comments here for future reference.
#
file := $(TARGET_OUT_KEYLAYOUT)/TWL4030_Keypad.kl
ALL_PREBUILT += $(file)
$(file) : $(LOCAL_PATH)/TWL4030_Keypad.kl | $(ACP)
	$(transform-prebuilt-to-target)

include $(CLEAR_VARS)
LOCAL_SRC_FILES := TWL4030_Keypad.kcm
include $(BUILD_KEY_CHAR_MAP)