#
# Copyright (C) 2021 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Debloat (persistent/boot-started services this device can't use). Flags must
# be set BEFORE the inherits below — they're checked at include time.
#  - AudioFX: android:persistent app; DSP offload path unused here
#  - SecureElement (com.android.se): persistent OMAPI service, no SE HAL/eSE
#  - ONS: opportunistic-network service, no eUICC on this device
#  - WAPPushManager: legacy WAP push router, nothing registers with it
TARGET_EXCLUDES_AUDIOFX := true
TARGET_EXCLUDES_SECURE_ELEMENT := true
TARGET_EXCLUDES_ONS := true
TARGET_EXCLUDES_WAPPUSH := true

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
    xiaomi_pepito_overlay_lineage \
    xiaomi_pepito_overlay_lineagesettings
endif

ifeq ($(TARGET_DEVICE_PEPITO),true)
# Android-Go-style build-time-only tuning for this low-RAM (~2.87GB) device.
# See PLAN-perf-battery.md "Category 1" - unlike the gotweaks Category 2
# properties, none of these have a live runtime knob, so they're baked in
# here rather than exposed as a Pepito Tweaks toggle.

# Speed-profile (not the default filter) for system_server + wifi-service,
# to reduce RAM and storage.
PRODUCT_SYSTEM_SERVER_COMPILER_FILTER := speed-profile

# Skip building the debug ART variant (libartd) - saves storage, no runtime
# effect on a non-eng build that would never load it anyway.
PRODUCT_ART_TARGET_INCLUDE_DEBUG_BUILD := false

# Strip the dex local variable table/type table to shrink the system image.
# Only affects JDWP-level local-variable debugging, not stack traces.
PRODUCT_MINIMIZE_JAVA_DEBUG_INFO := true

# Link the low-memory native (jemalloc) allocator variant to reduce RSS, at
# some allocation-speed cost. Matches upstream's own eng exclusion - this
# device's build target is userdebug (PLAN.md), so this is always live, but
# skip it if anyone ever builds this product as eng.
ifeq (,$(filter eng,$(TARGET_BUILD_VARIANT)))
MALLOC_LOW_MEMORY := true
endif
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

# NOTE: no build-prop / fingerprint override here — the device ships its honest
# Android 16 fingerprint. A stock-8.1 fingerprint mimic was trialled and dropped
# (2026-07-27): it buys neither Play Protect certification (handled by GSF-ID
# uncertified registration) nor Play Integrity (which reads the TEE's sealed
# attestation, not props). Play Integrity -12 on this device is a server-side
# recognition/500, not a build-tunable. Full lane writeup: PLAN-integrity.md.
