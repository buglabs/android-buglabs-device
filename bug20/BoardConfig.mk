# Definitions for the Bug20 board

TARGET_NO_BOOTLOADER := true
TARGET_NO_KERNEL := false

TARGET_PREBUILT_KERNEL := 
TARGET_KERNEL_DIR := kernel
TARGET_KERNEL_TARGET := uImage
TARGET_KERNEL_CONFIG := omap3_blandroid_defconfig

TARGET_PROVIDES_INIT_RC := true

TARGET_USE_GENERIC_AUDIO := true

TARGET_CPU_ABI := armeabi-v7a
TARGET_CPU_ABI2 := armeabi
TARGET_ARCH_VARIANT := armv7-a-neon
TARGET_BOARD_PLATFORM := omap3

BOARD_HAVE_BLUETOOTH := false

BOARD_USES_TSLIB := true

BOARD_USES_ALSA_AUDIO := true
BUILD_WITH_ALSA_UTILS := true

USE_CAMERA_STUB := true

# Wifi related defines
BOARD_WPA_SUPPLICANT_DRIVER := WEXT
WPA_SUPPLICANT_VERSION := VER_0_6_X
BOARD_WLAN_DEVICE := libertas
WIFI_DRIVER_MODULE_PATH := "/system/lib/modules/libertas_sdio.ko"
WIFI_DRIVER_FW_STA_PATH := "/system/etc/firmware/sd8686.bin"
#WIFI_DRIVER_FW_AP_PATH := "/system/etc/firmware/sd8686.bin"
WIFI_DRIVER_MODULE_ARG := "fw_name=/system/etc/firmware/sd8686.bin"
WIFI_DRIVER_MODULE_NAME := "libertas_sdio"

PRODUCT_COPY_FILES += \
	device/buglabs/bug20/vold.conf:system/etc/vold.conf \
	device/buglabs/bug20/vold.fstab:system/etc/vold.fstab \
	device/buglabs/bug20/asound.conf:system/etc/asound.conf \
	device/buglabs/bug20/initlogo.rle:root/initlogo.rle.bak \
	device/buglabs/bug20/ts.conf:system/etc/ts.conf \
	device/buglabs/bug20/ts.env:system/etc/ts.env \
	device/buglabs/bug20/calibrate.sh:system/bin/calibrate.sh \
	device/buglabs/bug20/init.rc:root/init.rc

PRODUCT_COPY_FILES += \
	device/buglabs/bug20/sd8686.bin:system/etc/firmware/sd8686.bin \
	device/buglabs/bug20/Marvell-Licence.txt:system/etc/firmware/Marvell-Licence.txt

include frameworks/base/data/sounds/AudioPackage2.mk
