#
# Copyright (C) 2021 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/product_launched_with_n_mr1.mk)

# Inherit some common LineageOS stuff.
$(call inherit-product, vendor/lineage/config/common_full_phone.mk)

# Inherit from Mi8937 device
PRODUCT_HARDWARE := Mi8937
TARGET_DEVICE_PEPITO := true
$(call inherit-product, device/xiaomi/Mi8937/device.mk)

# Overlays
DEVICE_PACKAGE_OVERLAYS += \
    $(LOCAL_PATH)/overlay-lineage

PRODUCT_PACKAGES += \
    xiaomi_prada_overlay_lineage \
    xiaomi_ulysse_overlay_lineage \
    xiaomi_wt8937_overlay_lineage

ifeq ($(TARGET_DEVICE_PEPITO),true)
PRODUCT_PACKAGES += \
    xiaomi_pepito_overlay_lineage
endif

# Device identifier. This must come after all inclusions
PRODUCT_DEVICE := Mi8937
PRODUCT_NAME := lineage_Mi8937
BOARD_VENDOR := Palm
PRODUCT_BRAND := Palm
PRODUCT_MODEL := PVG100
PRODUCT_MANUFACTURER := Palm
TARGET_VENDOR := Palm

PRODUCT_GMS_CLIENTID_BASE := android-palm

PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildDesc="Pepito-user 8.1.0 OPM1.171019.019 v1AML-0 release-keys" \
    BuildFingerprint=Palm/PVG100/Pepito:8.1.0/OPM1.171019.019/v1AML-0:user/release-keys
