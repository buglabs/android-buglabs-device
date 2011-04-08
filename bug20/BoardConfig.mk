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
BOARD_GPS_LIBRARIES := libgps

# This ends up as 'ro.product.board'
TARGET_BOOTLOADER_BOARD_NAME := bug20

BOARD_HAVE_BLUETOOTH := true

BOARD_USES_TSLIB := true

# this done by copying all the kernel modules
TARGET_NO_BUILD_WIFI := true

BOARD_USES_ALSA_AUDIO := true
BUILD_WITH_ALSA_UTILS := true

USE_CAMERA_STUB := true

# Wifi related defines
BOARD_WPA_SUPPLICANT_DRIVER := WEXT
WPA_SUPPLICANT_VERSION := VER_0_6_X
BOARD_WLAN_DEVICE := libertas
WIFI_DRIVER_MODULE_PATH := "/system/lib/modules/kernel/drivers/net/wireless/libertas/libertas_sdio.ko"
#WIFI_DRIVER_FW_STA_PATH := "/etc/firmware/sd8686.bin"
#WIFI_DRIVER_FW_AP_PATH := "/etc/firmware/sd8686.bin"
WIFI_DRIVER_MODULE_NAME := "libertas_sdio"

# Grouped the files top copy a bit for better readability

# Most basic things
PRODUCT_COPY_FILES += \
	device/buglabs/bug20/initlogo.rle:root/initlogo.rle.bak \
	device/buglabs/bug20/init.rc:root/init.rc

# Configurations for the low-level services
PRODUCT_COPY_FILES += \
	device/buglabs/bug20/vold.fstab:system/etc/vold.fstab \
	device/buglabs/bug20/gps.conf:system/etc/gps.conf \
	device/buglabs/bug20/asound.conf:system/etc/asound.conf \
	device/buglabs/bug20/dhcpcd.conf:system/etc/dhcpcd/dhcpcd.conf

# Wifi firmware and wpa_supplicant configuration
PRODUCT_COPY_FILES += \
	device/buglabs/bug20/sd8686.bin:system/etc/firmware/sd8686.bin \
	device/buglabs/bug20/sd8686_helper.bin:system/etc/firmware/sd8686_helper.bin \
	device/buglabs/bug20/Marvell-Licence.txt:system/etc/firmware/Marvell-Licence.txt \
	device/buglabs/bug20/wpa_supplicant.conf:system/etc/wifi/wpa_supplicant.conf 

# All things touchscreen
PRODUCT_COPY_FILES += \
	device/buglabs/bug20/ts.conf:system/etc/ts.conf \
	device/buglabs/bug20/ts.env:system/etc/ts.env \
	device/buglabs/bug20/pointercal:data/system/tslib/pointercal \
	device/buglabs/bug20/calibrate.sh:system/bin/calibrate.sh

# Ends up in 'default.prop'
ADDITIONAL_DEFAULT_PROPERTIES += \
	wifi.interface=wlan0	

include frameworks/base/data/sounds/AudioPackage2.mk
