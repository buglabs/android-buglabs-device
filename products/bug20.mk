$(call inherit-product, $(SRC_TARGET_DIR)/product/generic.mk)

PRODUCT_PACKAGES += \
    buglabs-demo-app \
    sensors.bug20 

# Overrides
PRODUCT_NAME := bug20
PRODUCT_BRAND := bug20
PRODUCT_DEVICE := bug20
