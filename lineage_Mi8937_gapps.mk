#
# Copyright (C) 2021 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# GApps variant of the pepito target: everything from lineage_Mi8937.mk, plus
# a baked-in NikGapps-basic payload (vendor/pepito-gapps), so OTA updates
# through the Updater app don't wipe GApps the way a separately-flashed addon
# zip would (a full OTA reflashes the whole system image).
#
# Requires WITH_GMS=true at build time: BoardConfig.mk only relaxes the
# system partition's reserved-size margin (800MB -> 40MB) when WITH_GMS=true,
# and GApps alone is ~600MB -- it doesn't fit under the vanilla target's
# tighter margin. build-lineage23.sh sets this automatically for --gapps.
$(call inherit-product, device/xiaomi/Mi8937/lineage_Mi8937.mk)
$(call inherit-product, vendor/pepito-gapps/gapps.mk)

# The Google app (Velvet) ships as a priv-app in the NikGapps GoogleSearch
# payload; the ASSISTANT role itself doesn't grant RECORD_AUDIO, and Android
# Auto can't prompt for it while projecting, so pregrant the mic here
# (gapps-only file: vanilla builds don't ship the package).
PRODUCT_COPY_FILES += \
    device/xiaomi/Mi8937/permissions/default-permissions-google-search.xml:$(TARGET_COPY_OUT_PRODUCT)/etc/default-permissions/default-permissions-google-search.xml

PRODUCT_NAME := lineage_Mi8937_gapps
