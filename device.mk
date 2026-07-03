#
# Copyright (C) 2021 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Cryptfshw
TARGET_EXCLUDE_CRYPTFSHW := true

ifeq ($(TARGET_DEVICE_PEPITO),true)
TARGET_USES_DEVICE_SPECIFIC_KEYMASTER := true
endif

# Inherit from mithorium-common
$(call inherit-product, device/xiaomi/mithorium-common/mithorium.mk)

# Sibling Mi8937 devices keep the 2 GB phone heap profile. Pepito uses
# Android Go defaults from lineage_Mi8937.mk instead.
ifneq ($(TARGET_DEVICE_PEPITO),true)
$(call inherit-product, frameworks/native/build/phone-xhdpi-2048-dalvik-heap.mk)
endif

# Boot animation
TARGET_SCREEN_HEIGHT := 1280
TARGET_SCREEN_WIDTH := 720

# Dynamic Partitions
PRODUCT_USE_DYNAMIC_PARTITIONS := false

# Init
$(call soong_config_set,libinit,vendor_init_lib,//$(LOCAL_PATH):init_xiaomi_mi8937)

# Overlays
DEVICE_PACKAGE_OVERLAYS += \
    $(LOCAL_PATH)/overlay

ifeq ($(PRODUCT_HARDWARE),Mi8917)
PRODUCT_PACKAGES += \
    xiaomi_rolex_overlay \
    xiaomi_riva_overlay \
    xiaomi_ugglite_overlay
else ifeq ($(PRODUCT_HARDWARE),Mi8937)
PRODUCT_PACKAGES += \
    xiaomi_prada_overlay \
    xiaomi_prada_overlay_Settings \
    xiaomi_ugg_overlay \
    xiaomi_wt8937_overlay \
    xiaomi_wt8937_overlay_Settings

ifeq ($(TARGET_DEVICE_PEPITO),true)
PRODUCT_PACKAGES += \
    VolumeTile \
    xiaomi_pepito_overlay \
    xiaomi_pepito_overlay_systemui
endif
endif

# Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.fingerprint.xml:$(TARGET_COPY_OUT_ODM)/etc/permissions/sku_fingerprint/android.hardware.fingerprint.xml

# Audio
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*.xml,$(LOCAL_PATH)/audio/mixer_paths/,$(TARGET_COPY_OUT_VENDOR)/etc/) \
    $(call find-copy-subdir-files,*.xml,$(LOCAL_PATH)/audio/platform_info/,$(TARGET_COPY_OUT_VENDOR)/etc/) \
    $(call find-copy-subdir-files,*,$(LOCAL_PATH)/audio/acdbdata/pepito/,$(TARGET_COPY_OUT_VENDOR)/etc/acdbdata/pepito/)

# Camera
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_ODM)/bin/mm-qcamera-daemon \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_ODM)/etc/camera/.placeholder \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_ODM)/lib/.placeholder \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_VENDOR)/lib/overlayfs/pepito/libandroidicu.so \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_VENDOR)/lib/overlayfs/pepito/libicu.so \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_VENDOR)/lib/overlayfs/pepito/libicui18n.so \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_VENDOR)/lib/overlayfs/pepito/libicuuc.so

# Disable camera builds — missing legacy headers in Lineage 23.2
# PRODUCT_PACKAGES += \
#     camera.ulysse \
#     camera.wingtech

# ifeq ($(PRODUCT_HARDWARE),Mi8937)
# PRODUCT_PACKAGES += \
#     camera.land
# endif

# Pepito camera HAL — source-built camera.pepito (QCamera2 HAL3). Live preview +
# capture verified 2026-07-02 (see PLAN-camera.md). Its shared_libs
# (libPmcamera_interface, libPmjpeg_interface, libPomx_core) install alongside it to
# /vendor/odm/lib automatically. The provider is pointed at it via
# ro.hardware.camera=pepito (rootdir/etc/init.xiaomi.device.rc). The stock AML0
# camera.msm8937.so blob is no longer loaded (still staged in the pepito overlay as
# dead weight; retire pepito_camera_msm8937 + libVDBeautyShotAPI + the linker fragment
# below as a follow-up once the clean-build camera is confirmed).
# TODO(layering): gate behind TARGET_DEVICE_PEPITO once wired — sibling Mi8937
# variants select their own camera.<variant>.
PRODUCT_PACKAGES += \
    camera.pepito

# Dumpstate
PRODUCT_PACKAGES += \
    libdumpstate_device

# Linker config — expose libandroid.so and libjnigraphics.so to vendor namespace
# Required by libVDBeautyShotAPI.so (DT_NEEDED by camera.msm8937.so)
PRODUCT_VENDOR_LINKER_CONFIG_FRAGMENTS += \
    device/xiaomi/Mi8937/configs/linker.config.json

# Filesystem
PRODUCT_PACKAGES += \
    e2fsck_ramdisk \
    tune2fs_ramdisk \
    resize2fs_ramdisk

# Enable project quotas and casefolding for emulated storage without sdcardfs
$(call inherit-product, $(SRC_TARGET_DIR)/product/emulated_storage.mk)

# Use FUSE passthrough
PRODUCT_PRODUCT_PROPERTIES += \
    persist.sys.fuse.passthrough.enable=true

ifeq ($(TARGET_DEVICE_PEPITO),true)
# Pepito's stock QTI keymaster wrapper rejects Android 16 OS version tags during
# configure. The blob is patched to read these stock-compatible values instead
# of ro.build.version.* for the configure command.
PRODUCT_SYSTEM_PROPERTIES += \
    ro.keymaster.xxx.release=8.1.0 \
    ro.keymaster.xxx.security_patch=2020-09-01
endif

# Fingerprint
ifeq ($(PRODUCT_HARDWARE),Mi8937)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_ODM)/bin/gx_fpd

PRODUCT_PACKAGES += \
    android.hardware.biometrics.fingerprint@2.1-service.xiaomi_ulysse \
    android.hardware.biometrics.fingerprint@2.1-service.xiaomi_wt8937

PRODUCT_PACKAGES += \
    liblzma.vendor:64
endif

# Input
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,$(LOCAL_PATH)/keylayout/,$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/) \
    $(foreach f, msm8917-sku5-snd-card_Button_Jack.kl msm8920-sku7-snd-card_Button_Jack.kl msm8952-sku1-snd-card_Button_Jack.kl, \
        $(LOCAL_PATH)/keylayout/msm8952-snd-card-mtp_Button_Jack.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/$(f))

# LiveDisplay
$(call soong_config_set_bool,livedisplay_sysfs,enable_re,true)

# Placeholder
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_ODM)/bin/.placeholder \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_ODM)/lib64/.placeholder

# Power
$(call soong_config_set,qtipower,tap_to_wake_node,/proc/sys/dev/xiaomi_msm8937_touchscreen/enable_dt2w)

# Recovery
ifeq ($(PRODUCT_HARDWARE),Mi8937)
PRODUCT_COPY_FILES += \
    vendor/xiaomi/Mi8937/proprietary/vendor/bin/hvdcp_opti:$(TARGET_COPY_OUT_RECOVERY)/root/system/bin/hvdcp_opti
endif

# Rootdir
PRODUCT_PACKAGES += \
    fstab.qcom.ramdisk \
    init.baseband.sh \
    init.xiaomi.device.rc \
    init.xiaomi.device.sh

ifeq ($(TARGET_DEVICE_PEPITO),true)
PRODUCT_PACKAGES += \
    init.pepito.qseecom.rc \
    init.pepito.qseecom.sh \
    android.hardware.keymaster@3.0-service-qti \
    android.hardware.keymaster@3.0-impl-qti \
    android.hardware.keymaster@3.0.vendor \
    libkeymasterdeviceutils \
    libkeymasterutils

PRODUCT_COPY_FILES += \
    vendor/xiaomi/Mi8937/proprietary/vendor/etc/keymaster-firmware/keymaster.b00:$(TARGET_COPY_OUT_VENDOR)/etc/keymaster-firmware/keymaster.b00 \
    vendor/xiaomi/Mi8937/proprietary/vendor/etc/keymaster-firmware/keymaster.b01:$(TARGET_COPY_OUT_VENDOR)/etc/keymaster-firmware/keymaster.b01 \
    vendor/xiaomi/Mi8937/proprietary/vendor/etc/keymaster-firmware/keymaster.b02:$(TARGET_COPY_OUT_VENDOR)/etc/keymaster-firmware/keymaster.b02 \
    vendor/xiaomi/Mi8937/proprietary/vendor/etc/keymaster-firmware/keymaster.b03:$(TARGET_COPY_OUT_VENDOR)/etc/keymaster-firmware/keymaster.b03 \
    vendor/xiaomi/Mi8937/proprietary/vendor/etc/keymaster-firmware/keymaster.b04:$(TARGET_COPY_OUT_VENDOR)/etc/keymaster-firmware/keymaster.b04 \
    vendor/xiaomi/Mi8937/proprietary/vendor/etc/keymaster-firmware/keymaster.b05:$(TARGET_COPY_OUT_VENDOR)/etc/keymaster-firmware/keymaster.b05 \
    vendor/xiaomi/Mi8937/proprietary/vendor/etc/keymaster-firmware/keymaster.b06:$(TARGET_COPY_OUT_VENDOR)/etc/keymaster-firmware/keymaster.b06 \
    vendor/xiaomi/Mi8937/proprietary/vendor/etc/keymaster-firmware/keymaster.mdt:$(TARGET_COPY_OUT_VENDOR)/etc/keymaster-firmware/keymaster.mdt
endif

ifeq ($(PRODUCT_HARDWARE),Mi8937)
PRODUCT_PACKAGES += \
    init.goodix.sh
endif

# Shims
PRODUCT_PACKAGES += \
    libshims_android \
    libshims_ui \
    libwui

ifeq ($(PRODUCT_HARDWARE),Mi8937)
PRODUCT_PACKAGES += \
    libbinder_shim.vendor \
    libc_mutexdestroy_shim \
    libc_pthreadts_shim \
    libfakelogprint

PRODUCT_COPY_FILES += \
    prebuilts/vndk/v32/arm64/arch-arm64-armv8-a/shared/vndk-sp/libhidlbase.so:$(TARGET_COPY_OUT_ODM)/lib64/libhidlbase-v32.so
endif

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(LOCAL_PATH)

# Wifi
ifeq ($(PRODUCT_HARDWARE),Mi8937)
PRODUCT_PACKAGES += \
    WifiOverlay_prada
endif

# Inherit from vendor blobs
ifeq ($(PRODUCT_HARDWARE),Mi8917)
$(call inherit-product, vendor/xiaomi/Mi8917/Mi8917-vendor.mk)
else ifeq ($(PRODUCT_HARDWARE),Mi8937)
$(call inherit-product, vendor/xiaomi/Mi8937/Mi8937-vendor.mk)
endif

# Pepito-specific hals.conf adds sensors.native.so (BST BHy HAL) alongside the
# SSC sub-HAL.  This overrides the mithorium-common base which only has sensors.ssc.so.
# sensors.native.so is only installed for this build (Mi8937-vendor.mk), so other
# variants picking up this file would get a harmless "not found" warning from multihal.
PRODUCT_COPY_FILES += \
    device/xiaomi/Mi8937/configs/sensors/hals.conf:$(TARGET_COPY_OUT_VENDOR)/etc/sensors/hals.conf
